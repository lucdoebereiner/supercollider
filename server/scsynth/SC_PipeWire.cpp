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

    // Shared capture buffer: input stream writes here on its process
    // callback; output stream reads on its process callback. Planar layout
    // (one channel slab after another), sized for up to kMaxQuantum frames.
    // Both run on pw_thread_loop's single data thread, so serialized — no
    // locking needed. mCaptureFrames is the valid-sample-count as of the
    // last input callback.
    std::vector<float> mCaptureBuf;
    int mCaptureFrames = 0;
    int mCaptureChannels = 0;
    bool mCaptureValid = false;

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

    mOutStream = pw_stream_new_simple(pw_thread_loop_get_loop(mLoop), "SuperCollider out", props,
                                      &kOutEvents, this);
    if (!mOutStream)
        return false;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = uint32_t(numChannels);
    info.rate = 0; // let pipewire negotiate
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

    mInStream = pw_stream_new_simple(pw_thread_loop_get_loop(mLoop), "SuperCollider in", props,
                                     &kInEvents, this);
    if (!mInStream)
        return false;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = uint32_t(numChannels);
    info.rate = 0;
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
    if (mMaxOutputLatency > 0.0)
        scprintf("%s: reported output latency %.2f ms\n", kPwDriverIdent, mMaxOutputLatency * 1e3);

    if (numInputs > 0) {
        mCaptureChannels = numInputs;
        mCaptureBuf.assign(size_t(numInputs) * size_t(mNegotiatedBufSize), 0.0f);
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

// =====================================================================
// RunInput: capture stream process callback
// Just stash the most recent capture buffer in the shared planar buffer.

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

    const int channels = mCaptureChannels > 0 ? mCaptureChannels : mNegotiatedInChannels;
    if (channels <= 0) {
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

    if (nFrames > 0 && int(nFrames) * channels <= int(mCaptureBuf.size())) {
        for (int k = 0; k < channels; ++k) {
            float* dst = mCaptureBuf.data() + size_t(k) * size_t(nFrames);
            const float* src = interleaved + k;
            for (uint32_t n = 0; n < nFrames; ++n)
                dst[n] = src[n * uint32_t(channels)];
        }
        mCaptureFrames = int(nFrames);
        mCaptureValid = true;
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

    // Setup handshake: on first callback with a real buffer, record the
    // quantum and signal DriverSetup. Keep outputting silence until the
    // format param has also been seen.
    if (!mSetupReady) {
        if (interleaved && nFrames > 0 && mNegotiatedBufSize == 0) {
            mNegotiatedBufSize = int(nFrames);
            mCurrentQuantum = nFrames;
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

    // Quantum or rate change detection: if the graph renegotiated, tell
    // the World via the public Reset path. We can't actually *change*
    // mNumSamplesPerCallback safely mid-run without a lot of care, so for
    // v1 we log, skip the buffer, and hope the graph settles. This is a
    // rare path — quantum changes usually happen once at startup.
    if (nFrames != mCurrentQuantum && nFrames > 0) {
        static int warned = 0;
        if (warned < 3) {
            scprintf("%s: quantum changed %u -> %u (not yet supported mid-run, dropping buffer)\n",
                     kPwDriverIdent, mCurrentQuantum, nFrames);
            warned++;
        }
        zeroAndQueue();
        mAudioSync.Signal();
        return;
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

        // Per-sub-block capture data: slice of our shared capture buffer.
        // mCaptureBuf is planar (channel-major) for the most recent input
        // callback, sized mCaptureChannels * mCaptureFrames. If we don't
        // have a full quantum yet, zero-fill.
        const bool haveCapture = mCaptureValid && mCaptureFrames >= numSamples && numInputs > 0
            && mCaptureChannels >= numInputs;

        int bufFramePos = 0;
        int64 oscTime = mOSCbuftime = int64((mDLL.PeriodTime() + mMaxOutputLatency) * kSecondsToOSCunits + .5);
        int64 oscInc = mOSCincrement = int64((mDLL.Period() / numBufs) * kSecondsToOSCunits + .5);
        mSmoothSampleRate = mDLL.SampleRate();
        double oscToSamples = mOSCtoSamples = mSmoothSampleRate * kOSCtoSecs;

        for (int i = 0; i < numBufs; ++i, world->mBufCounter++, bufFramePos += bufFrames) {
            int32 bufCounter = world->mBufCounter;

            // Copy + touch inputs (if any).
            if (numInputs > 0) {
                for (int k = 0; k < numInputs; ++k) {
                    float* dst = inBuses + k * bufFrames;
                    if (haveCapture) {
                        const float* src = mCaptureBuf.data() + size_t(k) * size_t(mCaptureFrames) + bufFramePos;
                        std::memcpy(dst, src, sizeof(float) * size_t(bufFrames));
                    } else {
                        std::memset(dst, 0, sizeof(float) * size_t(bufFrames));
                    }
                    inTouched[k] = bufCounter;
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
