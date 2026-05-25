/*
    PipeWire native audio driver for SuperCollider.

    Local experimental backend (v1) — not upstream. Output + input full
    duplex via two pw_streams. See tools/jack_signal_bench/STATUS.md for
    the design notes, the v0→v1 delta, and the (still) unimplemented
    pieces. The RT process loop mirrors SC_Jack.cpp's shape.

    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#include "SC_CoreAudio.h"
#include "SC_HiddenWorld.h"
#include "SC_Prototypes.h"
#include "SC_WorldOptions.h"
#include "SC_TimeDLL.hpp"
#include "SC_Time.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/parser.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// =====================================================================
// Free functions expected by the rest of scsynth (mirrors SC_Jack.cpp).
// These are intentionally not pipewire-specific; SC's internal clock uses
// std::chrono so any backend that lives on a vanilla Linux host looks the
// same here.

static const char* kPwDriverIdent = "PipeWireDriver";
static const char* kPwDefaultClientName = "SuperCollider";

int32 server_timeseed() { return timeSeed(); }

int64 oscTimeNow() { return OSCTime(getTime()); }

static double pwOscTimeSeconds() { return OSCTime(getTime()) * kOSCtoSecs; }

void initializeScheduler() {}

void sc_SetDenormalFlags();

// =====================================================================
// Helpers

static inline uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// Split a "-H" string into (outputTarget, inputTarget). Format:
//   "outSink"             — target outSink for playback, default for capture
//   ":inSource"           — default playback, target inSource for capture
//   "outSink:inSource"    — target both
//   ""                    — default both
static void parse_device_name(const char* raw, std::string& outTarget, std::string& inTarget) {
    if (!raw || !*raw)
        return;
    const char* colon = strchr(raw, ':');
    if (!colon) {
        outTarget = raw;
        return;
    }
    outTarget.assign(raw, colon - raw);
    inTarget.assign(colon + 1);
}

// =====================================================================
// SC_PipeWireDriver

class SC_PipeWireDriver : public SC_AudioDriver {
public:
    SC_PipeWireDriver(struct World* inWorld);
    ~SC_PipeWireDriver() override;

    // RT entry points (called from PipeWire's data thread)
    void RunOutput();
    void RunInput();

    // Main thread events
    void OnOutStateChanged(enum pw_stream_state old, enum pw_stream_state state, const char* error);
    void OnInStateChanged(enum pw_stream_state old, enum pw_stream_state state, const char* error);
    void OnOutParamChanged(uint32_t id, const struct spa_pod* param);
    void OnInParamChanged(uint32_t id, const struct spa_pod* param);

protected:
    bool DriverSetup(int* outNumSamplesPerCallback, double* outSampleRate) override;
    bool DriverStart() override;
    bool DriverStop() override;

private:
    // PipeWire objects
    struct pw_thread_loop* mLoop = nullptr;
    struct pw_stream* mOutStream = nullptr;
    struct pw_stream* mInStream = nullptr;
    struct spa_hook mOutListener {};
    struct spa_hook mInListener {};

    // Capture handoff: a lock-free single-producer/single-consumer ring.
    // The input and output streams are independent PipeWire graph nodes
    // driven on *different* RT threads with independent phase (verified:
    // the two process callbacks report different pthread ids and drift by
    // up to one quantum). The capture stream (RunInput) is the producer;
    // the playback stream (RunOutput, which drives the DSP loop) is the
    // consumer. The ring decouples the two clocks: it absorbs the per-
    // callback phase jitter and removes the data race that a single shared
    // buffer had. Layout is interleaved frames (channel-minor), capacity
    // kCapRingFrames (power of two) frames per channel. mCapWrite/mCapRead
    // are monotonic frame counters; (mCapWrite - mCapRead) is the fill.
    static constexpr uint32_t kCapRingFrames = 32768; // power of two; >= 4 * max quantum
    static constexpr uint32_t kCapRingMask = kCapRingFrames - 1;
    std::vector<float> mCapRing; // kCapRingFrames * mCaptureChannels, interleaved
    int mCaptureChannels = 0;
    std::atomic<uint32_t> mCapWrite { 0 }; // frames produced (RunInput)
    std::atomic<uint32_t> mCapRead { 0 }; // frames consumed (RunOutput)

    // Driver state
    SC_TimeDLL mDLL;
    double mMaxOutputLatency = 0.0; // seconds
    double mPeriodNs = 0.0;
    int mDropoutReportCounter = 0;
    std::atomic<bool> mStreaming { false };
    bool mHadShutdown = false;

    // Setup handshake — DriverSetup blocks until first output process() has
    // seen a quantum and a negotiated format. mSetupReady flips true under
    // mSetupMutex so the main-thread condition_variable wait observes the
    // state.
    std::mutex mSetupMutex;
    std::condition_variable mSetupCond;
    bool mSetupReady = false;
    int mNegotiatedBufSize = 0;
    double mNegotiatedRate = 0.0;
    int mNegotiatedOutChannels = 0;
    int mNegotiatedInChannels = 0;
    bool mOutFormatKnown = false;
    bool mInFormatKnown = false;
    // Used during the setup handshake to detect when PipeWire's resampler
    // has settled. The first process() callback after format negotiation
    // is a resampler-warmup transient that returns a non-aligned frame
    // count (e.g. 2081 for a 192k stream against a 48k graph, before
    // settling on 2048). We track the previous nFrames and only lock the
    // negotiated quantum in when we see the same value twice in a row.
    uint32_t mSetupPrevFrames = 0;

    // Runtime quantum/rate tracking for renegotiation detection.
    uint32_t mCurrentQuantum = 0;
    uint32_t mCurrentRate = 0;

    // Dropout counting (for /status peakCPU == instantaneous ratio, plus
    // mAvgCPU EMA — matches SC_Jack.cpp's local experimental accounting).
    // Tracked directly from per-callback wall time.

    // Targets from -H
    std::string mOutTarget;
    std::string mInTarget;

    bool createOutputStream(int numChannels);
    bool createInputStream(int numChannels);
    void tryCompleteSetup();
    void signalSetup();
    // Adapt to a quantum change from PipeWire. Called on the RT data thread
    // from RunOutput (the ring consumer) only. Returns false if the new
    // quantum is unworkable (zero, > kMaxAcceptableQuantum, or not an
    // integer multiple of world->mBufLength), in which case the caller must
    // drop the buffer. Allocation-free: the capture ring is fixed-size.
    bool handleQuantumChange(uint32_t newFrames);

    static constexpr uint32_t kMaxAcceptableQuantum = 8192;
};

SC_AudioDriver* SC_NewAudioDriver(struct World* inWorld) { return new SC_PipeWireDriver(inWorld); }

// =====================================================================
// pw_stream_events glue

static void sc_pw_out_process_cb(void* userdata) { static_cast<SC_PipeWireDriver*>(userdata)->RunOutput(); }

static void sc_pw_in_process_cb(void* userdata) { static_cast<SC_PipeWireDriver*>(userdata)->RunInput(); }

static void sc_pw_out_state_changed_cb(void* userdata, enum pw_stream_state old, enum pw_stream_state state,
                                       const char* error) {
    static_cast<SC_PipeWireDriver*>(userdata)->OnOutStateChanged(old, state, error);
}

static void sc_pw_in_state_changed_cb(void* userdata, enum pw_stream_state old, enum pw_stream_state state,
                                      const char* error) {
    static_cast<SC_PipeWireDriver*>(userdata)->OnInStateChanged(old, state, error);
}

static void sc_pw_out_param_changed_cb(void* userdata, uint32_t id, const struct spa_pod* param) {
    static_cast<SC_PipeWireDriver*>(userdata)->OnOutParamChanged(id, param);
}

static void sc_pw_in_param_changed_cb(void* userdata, uint32_t id, const struct spa_pod* param) {
    static_cast<SC_PipeWireDriver*>(userdata)->OnInParamChanged(id, param);
}

static const struct pw_stream_events kOutEvents = [] {
    pw_stream_events e = {};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = sc_pw_out_process_cb;
    e.state_changed = sc_pw_out_state_changed_cb;
    e.param_changed = sc_pw_out_param_changed_cb;
    return e;
}();

static const struct pw_stream_events kInEvents = [] {
    pw_stream_events e = {};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = sc_pw_in_process_cb;
    e.state_changed = sc_pw_in_state_changed_cb;
    e.param_changed = sc_pw_in_param_changed_cb;
    return e;
}();

// =====================================================================
// Construction / destruction

SC_PipeWireDriver::SC_PipeWireDriver(struct World* inWorld): SC_AudioDriver(inWorld) {}

SC_PipeWireDriver::~SC_PipeWireDriver() {
    if (mLoop) {
        pw_thread_loop_lock(mLoop);
        if (mInStream) {
            pw_stream_disconnect(mInStream);
            pw_stream_destroy(mInStream);
            mInStream = nullptr;
        }
        if (mOutStream) {
            pw_stream_disconnect(mOutStream);
            pw_stream_destroy(mOutStream);
            mOutStream = nullptr;
        }
        pw_thread_loop_unlock(mLoop);
        pw_thread_loop_stop(mLoop);
        pw_thread_loop_destroy(mLoop);
        mLoop = nullptr;
    }
    pw_deinit();
}

// =====================================================================
// Stream creation

bool SC_PipeWireDriver::createOutputStream(int numChannels) {
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                             PW_KEY_MEDIA_CATEGORY, "Playback",
                                             PW_KEY_MEDIA_ROLE, "DSP",
                                             PW_KEY_NODE_NAME, "SuperCollider",
                                             PW_KEY_NODE_DESCRIPTION, "SuperCollider playback",
                                             nullptr);
    if (!mOutTarget.empty())
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, mOutTarget.c_str());
    // The stream's sample rate is communicated via info.rate (below).
    // PW_KEY_NODE_FORCE_RATE is for forcing the *graph* rate, not just
    // declaring the node's format rate. On systems where the requested
    // rate isn't listed in clock.allowed-rates, FORCE_RATE produced bad
    // interactions with PipeWire's resampler (audible buzzing /
    // modulation on otherwise-clean ratios like 96k and 192k). The
    // graph stays at its own rate and PipeWire resamples our stream to
    // match — same behaviour any other audio client gets.
    // PW_KEY_NODE_FORCE_QUANTUM stays: it pins the graph quantum to a
    // bufLength-friendly value, which makes the resampler's steady-state
    // chunk size land on a multiple of world->mBufLength.
    {
        const uint32_t quantum = mPreferredHardwareBufferFrameSize ? mPreferredHardwareBufferFrameSize : 1024;
        char qStr[32];
        std::snprintf(qStr, sizeof(qStr), "%u", quantum);
        pw_properties_set(props, PW_KEY_NODE_FORCE_QUANTUM, qStr);
    }

    mOutStream = pw_stream_new_simple(pw_thread_loop_get_loop(mLoop), "SuperCollider out", props,
                                      &kOutEvents, this);
    if (!mOutStream)
        return false;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = uint32_t(numChannels);
    info.rate = mPreferredSampleRate; // 0 == let pipewire pick the graph rate
    // Without an explicit channel position array, pipewire defaults to
    // stereo FL/FR and silently narrows any larger channel count down to
    // two during format negotiation (qpwgraph then only shows output_FL
    // and output_FR). Map every channel to AUX0..AUXN-1 so the channels
    // are treated as "arbitrary, no surround meaning" — same idea as
    // JACK's out_1..out_N ports.
    const int nCh = sc_min(numChannels, int(SPA_AUDIO_MAX_CHANNELS));
    for (int i = 0; i < nCh; ++i)
        info.position[i] = uint32_t(SPA_AUDIO_CHANNEL_AUX0 + i);
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(mOutStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS
                                                 | PW_STREAM_FLAG_RT_PROCESS),
                          params, 1)
        < 0) {
        return false;
    }
    return true;
}

bool SC_PipeWireDriver::createInputStream(int numChannels) {
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                             PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "DSP",
                                             PW_KEY_NODE_NAME, "SuperCollider capture",
                                             PW_KEY_NODE_DESCRIPTION, "SuperCollider capture",
                                             nullptr);
    if (!mInTarget.empty())
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, mInTarget.c_str());
    {
        const uint32_t quantum = mPreferredHardwareBufferFrameSize ? mPreferredHardwareBufferFrameSize : 1024;
        char qStr[32];
        std::snprintf(qStr, sizeof(qStr), "%u", quantum);
        pw_properties_set(props, PW_KEY_NODE_FORCE_QUANTUM, qStr);
    }

    mInStream = pw_stream_new_simple(pw_thread_loop_get_loop(mLoop), "SuperCollider in", props,
                                     &kInEvents, this);
    if (!mInStream)
        return false;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = uint32_t(numChannels);
    info.rate = mPreferredSampleRate;
    // See createOutputStream: map each channel to AUX0..AUXN-1 so
    // pipewire doesn't silently narrow the count down to stereo.
    const int nCh = sc_min(numChannels, int(SPA_AUDIO_MAX_CHANNELS));
    for (int i = 0; i < nCh; ++i)
        info.position[i] = uint32_t(SPA_AUDIO_CHANNEL_AUX0 + i);
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(mInStream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS
                                                 | PW_STREAM_FLAG_RT_PROCESS),
                          params, 1)
        < 0) {
        return false;
    }
    return true;
}

// =====================================================================
// Setup: create streams, wait for format + first process callback

bool SC_PipeWireDriver::DriverSetup(int* outNumSamples, double* outSampleRate) {
    pw_init(nullptr, nullptr);

    const int numOutputs = static_cast<int>(mWorld->mNumOutputs);
    const int numInputs = static_cast<int>(mWorld->mNumInputs);
    if (numOutputs <= 0) {
        scprintf("%s: no output channels requested\n", kPwDriverIdent);
        return false;
    }

    parse_device_name(mWorld->hw->mInDeviceName, mOutTarget, mInTarget);
    if (!mOutTarget.empty())
        scprintf("%s: output target: %s\n", kPwDriverIdent, mOutTarget.c_str());
    if (!mInTarget.empty())
        scprintf("%s: input target: %s\n", kPwDriverIdent, mInTarget.c_str());

    mLoop = pw_thread_loop_new("sc_pipewire", nullptr);
    if (!mLoop) {
        scprintf("%s: pw_thread_loop_new failed\n", kPwDriverIdent);
        return false;
    }

    pw_thread_loop_lock(mLoop);

    if (!createOutputStream(numOutputs)) {
        scprintf("%s: failed to create output stream\n", kPwDriverIdent);
        pw_thread_loop_unlock(mLoop);
        return false;
    }

    if (numInputs > 0) {
        if (!createInputStream(numInputs)) {
            scprintf("%s: failed to create input stream\n", kPwDriverIdent);
            pw_thread_loop_unlock(mLoop);
            return false;
        }
    } else {
        // No input wanted — mark input-format-known so the setup handshake
        // only waits on the output side.
        mInFormatKnown = true;
    }

    pw_thread_loop_unlock(mLoop);

    if (pw_thread_loop_start(mLoop) < 0) {
        scprintf("%s: pw_thread_loop_start failed\n", kPwDriverIdent);
        return false;
    }

    // Block until we've observed a negotiated format on the output stream
    // AND a first process() callback (which tells us the actual quantum).
    {
        std::unique_lock<std::mutex> lk(mSetupMutex);
        if (!mSetupCond.wait_for(lk, std::chrono::seconds(5), [this] { return mSetupReady; })) {
            scprintf("%s: timed out waiting for stream setup\n", kPwDriverIdent);
            return false;
        }
    }

    scprintf("%s: negotiated %d samples @ %.1f Hz, %d out / %d in channel(s)\n", kPwDriverIdent,
             mNegotiatedBufSize, mNegotiatedRate, mNegotiatedOutChannels, numInputs);
    // After the setup handshake waits for steady-state (see RunOutput),
    // any rate with a clean integer ratio to the graph rate yields a
    // bufLength-aligned quantum (e.g. 24k/96k/192k against a 48k graph
    // give 512/1024/2048-frame callbacks). Rates without a clean ratio
    // (e.g. 44.1k against 48k) have a steady state that genuinely
    // oscillates between two adjacent non-aligned integers and can't
    // work without internal re-blocking.
    if (mWorld->mBufLength > 0 && (mNegotiatedBufSize % mWorld->mBufLength) != 0) {
        scprintf("%s: steady-state quantum %d is not a multiple of scsynth block size %d.\n",
                 kPwDriverIdent, mNegotiatedBufSize, mWorld->mBufLength);
        if (mPreferredSampleRate > 0
            && std::fabs(mNegotiatedRate - double(mPreferredSampleRate)) < 0.5) {
            scprintf("%s: the requested stream rate (%u Hz) has no clean integer ratio with the\n"
                     "  %s PipeWire graph rate, so the resampler oscillates between adjacent\n"
                     "  %s chunk sizes. Options:\n"
                     "  %s   - add the rate to PipeWire's allowed-rates so the graph switches:\n"
                     "  %s       pw-metadata -n settings 0 clock.allowed-rates '[ %u 48000 ]'\n"
                     "  %s   - omit -S to use the current graph rate\n"
                     "  %s   - use a rate with a clean integer ratio (24k/96k/192k against a\n"
                     "  %s     48k graph all work via PipeWire's resampler)\n",
                     kPwDriverIdent, mPreferredSampleRate, kPwDriverIdent, kPwDriverIdent, kPwDriverIdent,
                     mPreferredSampleRate, kPwDriverIdent, kPwDriverIdent, kPwDriverIdent);
        }
        return false;
    }
    if (mMaxOutputLatency > 0.0)
        scprintf("%s: reported output latency %.2f ms\n", kPwDriverIdent, mMaxOutputLatency * 1e3);

    if (numInputs > 0) {
        mCaptureChannels = numInputs;
        // Allocate the capture ring once, here on the main thread. Sized
        // for kCapRingFrames frames per channel (>= 4 * max quantum), so
        // the RT producer/consumer never allocate or resize.
        mCapRing.assign(size_t(kCapRingFrames) * size_t(numInputs), 0.0f);
        mCapWrite.store(0, std::memory_order_relaxed);
        mCapRead.store(0, std::memory_order_relaxed);
    }

    *outNumSamples = mNegotiatedBufSize;
    *outSampleRate = mNegotiatedRate;
    mPeriodNs = 1e9 * double(mNegotiatedBufSize) / mNegotiatedRate;
    mCurrentQuantum = uint32_t(mNegotiatedBufSize);
    mCurrentRate = uint32_t(mNegotiatedRate);
    return true;
}

bool SC_PipeWireDriver::DriverStart() {
    if (!mOutStream)
        return false;
    mDLL.Reset(mSampleRate, mNumSamplesPerCallback, SC_TIME_DLL_BW, pwOscTimeSeconds());
    mStreaming.store(true);
    return true;
}

bool SC_PipeWireDriver::DriverStop() {
    mStreaming.store(false);
    if (mLoop) {
        pw_thread_loop_lock(mLoop);
        if (mOutStream)
            pw_stream_set_active(mOutStream, false);
        if (mInStream)
            pw_stream_set_active(mInStream, false);
        pw_thread_loop_unlock(mLoop);
    }
    return true;
}

// =====================================================================
// State / param event handling

void SC_PipeWireDriver::OnOutStateChanged(enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    scprintf("%s: out %s -> %s%s%s\n", kPwDriverIdent, pw_stream_state_as_string(old),
             pw_stream_state_as_string(state), error ? " error=" : "", error ? error : "");
    if ((state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) && mStreaming.load()
        && !mHadShutdown) {
        mHadShutdown = true;
        scprintf("%s: lost output stream, asking scsynth to quit\n", kPwDriverIdent);
        mWorld->hw->mTerminating = true;
        mWorld->hw->mQuitProgram->post();
    }
}

void SC_PipeWireDriver::OnInStateChanged(enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    scprintf("%s: in  %s -> %s%s%s\n", kPwDriverIdent, pw_stream_state_as_string(old),
             pw_stream_state_as_string(state), error ? " error=" : "", error ? error : "");
}

void SC_PipeWireDriver::OnOutParamChanged(uint32_t id, const struct spa_pod* param) {
    if (!param)
        return;
    if (id == SPA_PARAM_Format) {
        spa_audio_info info = {};
        if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
            return;
        if (info.media_type != SPA_MEDIA_TYPE_audio || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
            return;
        if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
            return;
        mNegotiatedRate = double(info.info.raw.rate);
        mNegotiatedOutChannels = int(info.info.raw.channels);
        mOutFormatKnown = true;
        tryCompleteSetup();
    } else if (id == SPA_PARAM_Latency) {
        spa_latency_info lat = {};
        if (spa_latency_parse(param, &lat) < 0)
            return;
        if (lat.direction == SPA_DIRECTION_OUTPUT) {
            // Prefer nanosecond latency if provided, otherwise derive from rate.
            double seconds = 0.0;
            if (lat.max_ns > 0)
                seconds = double(lat.max_ns) / 1e9;
            else if (lat.max_rate > 0 && mNegotiatedRate > 0.0)
                seconds = double(lat.max_rate) / mNegotiatedRate;
            else if (lat.max_quantum > 0.0 && mNegotiatedBufSize > 0 && mNegotiatedRate > 0.0)
                seconds = double(lat.max_quantum) * double(mNegotiatedBufSize) / mNegotiatedRate;
            if (seconds >= 0.0)
                mMaxOutputLatency = seconds;
        }
    }
}

void SC_PipeWireDriver::OnInParamChanged(uint32_t id, const struct spa_pod* param) {
    if (!param)
        return;
    if (id != SPA_PARAM_Format)
        return;
    spa_audio_info info = {};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_audio || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
        return;
    mNegotiatedInChannels = int(info.info.raw.channels);
    // If in/out rates disagree, trust the output rate for the DLL (we
    // resample nothing; PipeWire handles any cross-stream conversion).
    if (mNegotiatedRate == 0.0)
        mNegotiatedRate = double(info.info.raw.rate);
    mInFormatKnown = true;
    tryCompleteSetup();
}

void SC_PipeWireDriver::tryCompleteSetup() {
    // Called from param_changed callbacks. Setup completes when both
    // formats are known AND the first process callback has observed a
    // quantum; the latter lives in RunOutput() which calls signalSetup().
    // Nothing to do here except note that formats are ready; RunOutput
    // will check.
}

void SC_PipeWireDriver::signalSetup() {
    {
        std::lock_guard<std::mutex> lk(mSetupMutex);
        if (mSetupReady)
            return;
        if (!(mOutFormatKnown && mInFormatKnown && mNegotiatedBufSize > 0 && mNegotiatedRate > 0.0))
            return;
        mSetupReady = true;
    }
    mSetupCond.notify_one();
}

bool SC_PipeWireDriver::handleQuantumChange(uint32_t newFrames) {
    if (newFrames == 0 || newFrames > kMaxAcceptableQuantum)
        return false;
    const int bufFrames = mWorld ? mWorld->mBufLength : 0;
    if (bufFrames <= 0 || int(newFrames) % bufFrames != 0)
        return false;

    mCurrentQuantum = newFrames;
    mNumSamplesPerCallback = int(newFrames);
    if (mNegotiatedRate > 0.0)
        mPeriodNs = 1e9 * double(newFrames) / mNegotiatedRate;

    // The capture ring stores plain interleaved frames, so its layout does
    // not depend on the quantum and needs no resize here. We do flush it
    // (drop whatever is buffered) so the consumer re-establishes its
    // latency cushion against the new quantum rather than reading frames
    // captured under the old timing. handleQuantumChange is only ever
    // called from RunOutput (the ring consumer), so advancing mCapRead here
    // keeps the single-producer/single-consumer invariant intact.
    if (mCaptureChannels > 0)
        mCapRead.store(mCapWrite.load(std::memory_order_acquire), std::memory_order_release);

    if (mSampleRate > 0.0)
        mDLL.Reset(mSampleRate, mNumSamplesPerCallback, SC_TIME_DLL_BW, pwOscTimeSeconds());
    return true;
}

// =====================================================================
// RunInput: capture stream process callback (ring producer)
// Push the captured frames into the lock-free ring. Runs on a different RT
// thread than RunOutput, so it only ever writes the ring storage and the
// mCapWrite counter; it never touches mCapRead.

void SC_PipeWireDriver::RunInput() {
    if (!mInStream)
        return;
    struct pw_buffer* pwBuf = pw_stream_dequeue_buffer(mInStream);
    if (!pwBuf)
        return;
    struct spa_buffer* sbuf = pwBuf->buffer;
    if (sbuf->n_datas == 0 || !sbuf->datas[0].data) {
        pw_stream_queue_buffer(mInStream, pwBuf);
        return;
    }

    const int channels = mCaptureChannels;
    if (channels <= 0 || mCapRing.empty()) {
        pw_stream_queue_buffer(mInStream, pwBuf);
        return;
    }
    const uint32_t stride = sizeof(float) * uint32_t(channels);
    const struct spa_chunk* chunk = sbuf->datas[0].chunk;
    const uint32_t offset = chunk ? chunk->offset : 0;
    const uint32_t size = chunk ? chunk->size : 0;
    const uint32_t nFrames = size / stride;

    const float* interleaved = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(sbuf->datas[0].data) + offset);

    if (nFrames > 0) {
        const uint32_t w = mCapWrite.load(std::memory_order_relaxed);
        float* ring = mCapRing.data();
        for (uint32_t n = 0; n < nFrames; ++n) {
            float* dst = ring + size_t((w + n) & kCapRingMask) * size_t(channels);
            const float* src = interleaved + size_t(n) * size_t(channels);
            for (int k = 0; k < channels; ++k)
                dst[k] = src[k];
        }
        // Publish: release so the consumer's acquire-load of mCapWrite sees
        // all the ring writes above.
        mCapWrite.store(w + nFrames, std::memory_order_release);
    }
    pw_stream_queue_buffer(mInStream, pwBuf);
}

// =====================================================================
// RunOutput: playback stream process callback — drives the DSP loop

void SC_PipeWireDriver::RunOutput() {
    sc_SetDenormalFlags();

    struct pw_buffer* pwBuf = pw_stream_dequeue_buffer(mOutStream);
    if (!pwBuf)
        return;
    struct spa_buffer* sbuf = pwBuf->buffer;
    float* interleaved = (sbuf->n_datas > 0) ? static_cast<float*>(sbuf->datas[0].data) : nullptr;
    const int numChannels = static_cast<int>(mWorld->mNumOutputs);
    const uint32_t stride = sizeof(float) * uint32_t(numChannels);
    uint32_t nFrames = 0;
    if (interleaved) {
        nFrames = sbuf->datas[0].maxsize / stride;
        if (pwBuf->requested && pwBuf->requested < nFrames)
            nFrames = pwBuf->requested;
    }

    auto zeroAndQueue = [&]() {
        if (interleaved && nFrames > 0)
            std::memset(interleaved, 0, nFrames * stride);
        if (interleaved && nFrames > 0 && sbuf->datas[0].chunk) {
            sbuf->datas[0].chunk->offset = 0;
            sbuf->datas[0].chunk->stride = int32_t(stride);
            sbuf->datas[0].chunk->size = nFrames * stride;
        }
        pw_stream_queue_buffer(mOutStream, pwBuf);
    };

    // Setup handshake: capture the *steady-state* quantum, not the first
    // callback's value. After a stream rate change PipeWire's resampler
    // emits a transient on cb#0 (e.g. 2081 at -S 192000) before settling
    // on its actual cycle size (2048 in that example). Wait until we see
    // nFrames stay the same for two callbacks in a row, then lock it in
    // and signal DriverSetup. Until then, keep outputting silence.
    if (!mSetupReady) {
        if (interleaved && nFrames > 0 && mNegotiatedBufSize == 0) {
            if (mSetupPrevFrames == nFrames) {
                mNegotiatedBufSize = int(nFrames);
                mCurrentQuantum = nFrames;
            } else {
                mSetupPrevFrames = nFrames;
            }
        }
        signalSetup();
        zeroAndQueue();
        return;
    }

    // Before DriverStart flips mStreaming, the World isn't yet hooked up
    // for DSP — silence.
    if (!mStreaming.load()) {
        zeroAndQueue();
        return;
    }

    // Quantum-change handling. PipeWire is allowed to renegotiate the
    // graph quantum at any time (most commonly right after startup while
    // the graph rate is converging, but also after suspend/resume or when
    // another client joins with a stricter latency request). Adapt the
    // driver bookkeeping in place; only drop the buffer if the new
    // quantum is unworkable (e.g. not divisible by world->mBufLength).
    if (nFrames != mCurrentQuantum && nFrames > 0) {
        const uint32_t oldQuantum = mCurrentQuantum;
        if (handleQuantumChange(nFrames)) {
            scprintf("%s: quantum changed %u -> %u (adapted)\n", kPwDriverIdent, oldQuantum, nFrames);
        } else {
            if (mDropoutReportCounter <= 0) {
                scprintf("%s: quantum changed %u -> %u (unworkable: not aligned to bufLength %d "
                         "or > %u; dropping buffer)\n",
                         kPwDriverIdent, oldQuantum, nFrames, mWorld ? mWorld->mBufLength : 0,
                         kMaxAcceptableQuantum);
                mDropoutReportCounter = mMaxPeakCounter;
            } else {
                --mDropoutReportCounter;
            }
            zeroAndQueue();
            mAudioSync.Signal();
            return;
        }
    }

    const uint64_t rtT0 = monotonic_ns();

    mDLL.Update(pwOscTimeSeconds());

    World* world = mWorld;

    try {
        mFromEngine.Free();
        mToEngine.Perform();
        mOscPacketsToEngine.Perform();

        const int numOutputs = numChannels;
        const int numInputs = int(world->mNumInputs);
        const int numSamples = int(nFrames);
        const int bufFrames = world->mBufLength;
        if (bufFrames <= 0 || numSamples <= 0 || numSamples % bufFrames != 0) {
            zeroAndQueue();
            mAudioSync.Signal();
            return;
        }
        const int numBufs = numSamples / bufFrames;

        float* outBuses = world->mAudioBus;
        float* inBuses = world->mAudioBus + world->mNumOutputs * bufFrames;
        int32* outTouched = world->mAudioBusTouched;
        int32* inTouched = world->mAudioBusTouched + world->mNumOutputs;

        // Capture handoff (ring consumer). The capture stream runs on a
        // separate RT thread; pull this cycle's input frames from the
        // lock-free ring, keeping a small latency cushion (~2 quanta) so
        // phase jitter between the two streams doesn't cause spurious
        // underruns. capRBase is the ring frame index for the first frame
        // of this callback; the per-sub-block loop below deinterleaves out
        // of the ring at capRBase + bufFramePos.
        uint32_t capRBase = 0;
        bool haveCapture = false;
        if (numInputs > 0 && mCaptureChannels >= numInputs) {
            const uint32_t w = mCapWrite.load(std::memory_order_acquire);
            uint32_t r = mCapRead.load(std::memory_order_relaxed);
            const uint32_t avail = w - r; // wrap-safe
            if (avail >= uint32_t(numSamples)) {
                // If the producer has run far ahead (startup, or slow
                // clock drift), drop the oldest frames so latency stays
                // bounded at ~2 quanta rather than growing without limit.
                if (avail > uint32_t(numSamples) * 3)
                    r = w - uint32_t(numSamples) * 2;
                capRBase = r;
                mCapRead.store(r + uint32_t(numSamples), std::memory_order_release);
                haveCapture = true;
            }
            // else: underrun this cycle -> zero-fill; leave mCapRead so the
            // cushion rebuilds from the producer side.
        }

        int bufFramePos = 0;
        int64 oscTime = mOSCbuftime = int64((mDLL.PeriodTime() + mMaxOutputLatency) * kSecondsToOSCunits + .5);
        int64 oscInc = mOSCincrement = int64((mDLL.Period() / numBufs) * kSecondsToOSCunits + .5);
        mSmoothSampleRate = mDLL.SampleRate();
        double oscToSamples = mOSCtoSamples = mSmoothSampleRate * kOSCtoSecs;

        for (int i = 0; i < numBufs; ++i, world->mBufCounter++, bufFramePos += bufFrames) {
            int32 bufCounter = world->mBufCounter;

            // Copy + touch inputs (if any). The ring is interleaved, so
            // deinterleave each channel into its scsynth input bus.
            if (numInputs > 0) {
                if (haveCapture) {
                    const float* ring = mCapRing.data();
                    const int capCh = mCaptureChannels;
                    for (int k = 0; k < numInputs; ++k) {
                        float* dst = inBuses + k * bufFrames;
                        for (int n = 0; n < bufFrames; ++n) {
                            const uint32_t slot = (capRBase + uint32_t(bufFramePos + n)) & kCapRingMask;
                            dst[n] = ring[size_t(slot) * size_t(capCh) + k];
                        }
                        inTouched[k] = bufCounter;
                    }
                } else {
                    for (int k = 0; k < numInputs; ++k) {
                        std::memset(inBuses + k * bufFrames, 0, sizeof(float) * size_t(bufFrames));
                        inTouched[k] = bufCounter;
                    }
                }
            }

            // Run scheduled events in this sub-block.
            int64 schedTime;
            int64 nextTime = oscTime + oscInc;
            while ((schedTime = mScheduler.NextTime()) <= nextTime) {
                float diffTime = (float)(schedTime - oscTime) * oscToSamples + 0.5;
                float diffTimeFloor = floorf(diffTime);
                world->mSampleOffset = (int)diffTimeFloor;
                world->mSubsampleOffset = diffTime - diffTimeFloor;

                if (world->mSampleOffset < 0)
                    world->mSampleOffset = 0;
                else if (world->mSampleOffset >= world->mBufLength)
                    world->mSampleOffset = world->mBufLength - 1;

                SC_ScheduledEvent event = mScheduler.Remove();
                event.Perform();
            }

            world->mSampleOffset = 0;
            world->mSubsampleOffset = 0.f;
            World_Run(world);

            // Interleave outputs into pipewire buffer (planar → interleaved).
            for (int k = 0; k < numOutputs; ++k) {
                float* src = outBuses + k * bufFrames;
                float* dstBase = interleaved + bufFramePos * numChannels + k;
                if (outTouched[k] == bufCounter) {
                    for (int n = 0; n < bufFrames; ++n)
                        dstBase[n * numChannels] = src[n];
                } else {
                    for (int n = 0; n < bufFrames; ++n)
                        dstBase[n * numChannels] = 0.0f;
                }
            }

            mOSCbuftime = oscTime = nextTime;
        }
    } catch (std::exception& exc) {
        scprintf("%s: exception in real time: %s\n", kPwDriverIdent, exc.what());
    } catch (...) {
        scprintf("%s: unknown exception in real time\n", kPwDriverIdent);
    }

    if (sbuf->datas[0].chunk) {
        sbuf->datas[0].chunk->offset = 0;
        sbuf->datas[0].chunk->stride = int32_t(stride);
        sbuf->datas[0].chunk->size = nFrames * stride;
    }
    pw_stream_queue_buffer(mOutStream, pwBuf);

    // CPU load + xrun detection (same as the local patch in SC_Jack.cpp,
    // kept in sync so the scsynth_sweep harness compares like-for-like).
    const uint64_t rtT1 = monotonic_ns();
    const double busyNs = double(rtT1 - rtT0);
    const double cpuUsage = mPeriodNs > 0.0 ? 100.0 * (busyNs / mPeriodNs) : 0.0;
    mAvgCPU = mAvgCPU + 0.1 * (cpuUsage - mAvgCPU);
    if (cpuUsage > mPeakCPU || --mPeakCounter <= 0) {
        mPeakCPU = cpuUsage;
        mPeakCounter = mMaxPeakCounter;
    }
    if (mPeriodNs > 0.0 && busyNs > mPeriodNs) {
        if (mDropoutReportCounter <= 0) {
            scprintf("%s: xrun (callback %.2f ms, period %.2f ms)\n", kPwDriverIdent, busyNs / 1e6,
                     mPeriodNs / 1e6);
            mDropoutReportCounter = mMaxPeakCounter;
        } else {
            mDropoutReportCounter--;
        }
    }

    mAudioSync.Signal();
}
