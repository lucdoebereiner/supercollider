// pw_signal_bench.cpp
//
// Native-PipeWire twin of jack_signal_bench. Uses pw_stream as a stereo
// playback source (autoconnected to the default sink), with
// PW_STREAM_FLAG_RT_PROCESS so process() runs on PipeWire's RT data thread.
// Same three signaling modes (none / cv_any / sem_t), same per-callback
// timing ring, same stats. Build:
//
//   g++ -O2 -std=c++17 -pthread pw_signal_bench.cpp \
//       $(pkg-config --cflags libpipewire-0.3) \
//       $(pkg-config --libs libpipewire-0.3) -o pw_signal_bench

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <semaphore.h>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Signalers — byte-for-byte same as jack_signal_bench.cpp

class CvAnySignaler {
public:
    void Signal() { ++write; available.notify_one(); }
    void WaitNext() {
        std::unique_lock<std::mutex> lock(mutex);
        read = write;
        while (read == write) available.wait(lock);
    }
private:
    std::condition_variable_any available;
    std::mutex mutex;
    int read = 0, write = 0;
};

class SemSignaler {
public:
    SemSignaler() { sem_init(&sem, 0, 0); }
    ~SemSignaler() { sem_destroy(&sem); }
    void Signal() { sem_post(&sem); }
    void WaitNext() {
        while (sem_trywait(&sem) == 0) { }
        while (sem_wait(&sem) != 0 && errno == EINTR) { }
    }
private:
    sem_t sem;
};

// ---------------------------------------------------------------------------
// Synthetic DSP load — keep byte-for-byte identical with jack_signal_bench.cpp.

class LoadWork {
public:
    static constexpr int kScratchN = 256;
    LoadWork() { for (int i = 0; i < kScratchN; ++i) scratch[i] = 0.5f + 0.001f * float(i); }
    __attribute__((noinline))
    void do_work(int iters) {
        float* s = scratch;
        for (int k = 0; k < iters; ++k) {
            for (int i = 0; i < kScratchN; ++i)
                s[i] = s[i] * 1.0000001f + 0.5f - 0.5f;
        }
        asm volatile("" : : "r"(s[0]) : "memory");
    }
    int calibrate(double target_us) {
        if (target_us <= 0.0) return 0;
        do_work(200);
        const int probe = 4000;
        const int runs = 7;
        uint64_t best = UINT64_MAX;
        for (int r = 0; r < runs; ++r) {
            timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            do_work(probe);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            uint64_t dur = uint64_t(t1.tv_sec - t0.tv_sec) * 1000000000ull
                         + (t1.tv_nsec - t0.tv_nsec);
            if (dur < best) best = dur;
        }
        double ns_per_iter = double(best) / double(probe);
        double iters = (target_us * 1000.0) / ns_per_iter;
        return iters > 1.0 ? int(iters) : 1;
    }
private:
    float scratch[kScratchN];
};

// ---------------------------------------------------------------------------
// Sample ring

struct Sample { uint32_t dur_ns, gap_ns; };

struct SampleRing {
    static constexpr size_t kCap = 1u << 20;
    static constexpr size_t kMask = kCap - 1;
    std::vector<Sample> data{ kCap, Sample{0, 0} };
    std::atomic<uint64_t> head{ 0 };
    std::atomic<uint64_t> dropped{ 0 };
    void push(Sample s) {
        uint64_t h = head.load(std::memory_order_relaxed);
        if (h >= kCap) { dropped.fetch_add(1); return; }
        data[h & kMask] = s;
        head.store(h + 1, std::memory_order_release);
    }
};

// ---------------------------------------------------------------------------

enum class Mode { None, Cv, Sem };

struct Bench {
    Mode mode = Mode::None;
    int num_outputs = 2;

    SampleRing ring;
    CvAnySignaler cv;
    SemSignaler sem;
    std::mutex nrt_lock;

    std::atomic<bool> running{ true };
    std::atomic<uint64_t> callbacks{ 0 };
    std::atomic<uint64_t> wakeups{ 0 };
    std::atomic<uint64_t> deadline_misses{ 0 };
    uint64_t last_callback_start_ns = 0;
    double period_ns = 0.0;
    uint64_t period_ns_int = 0; // rounded, used by the RT thread for deadline checks

    LoadWork load;
    int load_iters = 0;

    struct pw_thread_loop* loop = nullptr;
    struct pw_stream* stream = nullptr;
};

static inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---------------------------------------------------------------------------
// RT process callback

static void on_process(void* userdata) {
    Bench* b = static_cast<Bench*>(userdata);
    const uint64_t t0 = now_ns();

    struct pw_buffer* pw_buf = pw_stream_dequeue_buffer(b->stream);
    if (pw_buf) {
        struct spa_buffer* buf = pw_buf->buffer;
        if (buf->n_datas > 0) {
            float* samples = static_cast<float*>(buf->datas[0].data);
            // Frames available: min(maxsize / stride, requested). For interleaved
            // float stereo, stride = sizeof(float)*channels.
            const uint32_t stride = sizeof(float) * b->num_outputs;
            uint32_t n_frames = buf->datas[0].maxsize / stride;
            if (pw_buf->requested && pw_buf->requested < n_frames)
                n_frames = pw_buf->requested;

            // DC fill all channels, interleaved.
            for (uint32_t i = 0; i < n_frames * uint32_t(b->num_outputs); ++i)
                samples[i] = 0.25f;

            buf->datas[0].chunk->offset = 0;
            buf->datas[0].chunk->stride = int32_t(stride);
            buf->datas[0].chunk->size = n_frames * stride;
        }
        pw_stream_queue_buffer(b->stream, pw_buf);
    }

    if (b->load_iters > 0) b->load.do_work(b->load_iters);

    switch (b->mode) {
    case Mode::None:                 break;
    case Mode::Cv:  b->cv.Signal();  break;
    case Mode::Sem: b->sem.Signal(); break;
    }

    const uint64_t t1 = now_ns();
    const uint64_t dur = t1 - t0;
    const uint64_t gap = b->last_callback_start_ns == 0 ? 0 : (t0 - b->last_callback_start_ns);
    b->last_callback_start_ns = t0;
    Sample s;
    s.dur_ns = dur > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(dur);
    s.gap_ns = gap > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(gap);
    b->ring.push(s);
    b->callbacks.fetch_add(1, std::memory_order_relaxed);
    if (b->period_ns_int && dur >= b->period_ns_int)
        b->deadline_misses.fetch_add(1, std::memory_order_relaxed);
}

static void on_state_changed(void* /*userdata*/, enum pw_stream_state old,
                             enum pw_stream_state state, const char* error) {
    std::fprintf(stderr, "[bench] state: %s -> %s%s%s\n",
                 pw_stream_state_as_string(old),
                 pw_stream_state_as_string(state),
                 error ? " error=" : "",
                 error ? error : "");
}

static const struct pw_stream_events stream_events = [] {
    pw_stream_events e = {};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = on_process;
    e.state_changed = on_state_changed;
    return e;
}();

// ---------------------------------------------------------------------------

static void waiter_thread(Bench* b) {
    while (b->running.load(std::memory_order_relaxed)) {
        switch (b->mode) {
        case Mode::None:
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            break;
        case Mode::Cv:  b->cv.WaitNext();  break;
        case Mode::Sem: b->sem.WaitNext(); break;
        }
        {
            std::lock_guard<std::mutex> lk(b->nrt_lock);
            volatile uint64_t sink = 0;
            for (int i = 0; i < 32; ++i) sink += i;
            (void)sink;
        }
        b->wakeups.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Statistics

static void print_stats(const char* label, const char* field,
                        std::vector<uint32_t> samples, double period_ns) {
    if (samples.empty()) { std::fprintf(stderr, "no samples\n"); return; }
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    auto pct = [&](double p) { return samples[size_t(p * (n - 1))]; };
    double sum = 0;
    for (uint32_t v : samples) sum += double(v);
    const double mean = sum / double(n);
    double sq = 0;
    for (uint32_t v : samples) { double d = double(v) - mean; sq += d * d; }
    const double stddev = std::sqrt(sq / double(n));
    const uint32_t p50   = pct(0.50);
    const uint32_t p90   = pct(0.90);
    const uint32_t p99   = pct(0.99);
    const uint32_t p999  = pct(0.999);
    const uint32_t p9999 = pct(0.9999);
    const uint32_t maxv  = samples.back();

    std::printf("\n=== %s [%s] ===\n", label, field);
    std::printf("  samples:   %zu\n", n);
    std::printf("  period:    %.1f us\n", period_ns / 1e3);
    std::printf("  mean:      %7.2f us   (%s ~ %5.2f %% of period)\n",
                mean / 1e3,
                std::strcmp(field, "dur") == 0 ? "cpu_load" : "gap/period",
                100.0 * mean / period_ns);
    std::printf("  stddev:    %7.2f us\n", stddev / 1e3);
    std::printf("  p50:       %7.2f us\n", p50 / 1e3);
    std::printf("  p90:       %7.2f us\n", p90 / 1e3);
    std::printf("  p99:       %7.2f us\n", p99 / 1e3);
    std::printf("  p99.9:     %7.2f us\n", p999 / 1e3);
    std::printf("  p99.99:    %7.2f us\n", p9999 / 1e3);
    std::printf("  max:       %7.2f us\n", maxv / 1e3);
    std::printf("  spike p99/p50:   %6.2fx\n", double(p99) / double(p50 ? p50 : 1));
    std::printf("  spike p999/p50:  %6.2fx\n", double(p999) / double(p50 ? p50 : 1));
    std::printf("  spike max/mean:  %6.2fx\n", double(maxv) / (mean > 0 ? mean : 1));

    static const uint32_t edges[] = {
        500, 1000, 2000, 5000, 10000, 20000, 50000,
        100000, 200000, 500000, 1000000, 2000000, 5000000,
    };
    const int ne = sizeof(edges) / sizeof(edges[0]);
    std::vector<size_t> buckets(ne + 1, 0);
    for (uint32_t v : samples) {
        int b = ne;
        for (int i = 0; i < ne; ++i) if (v < edges[i]) { b = i; break; }
        buckets[b]++;
    }
    std::printf("  histogram:\n");
    for (int i = 0; i < ne; ++i) {
        if (buckets[i] == 0) continue;
        double pct_ = 100.0 * double(buckets[i]) / double(n);
        std::printf("    < %8u ns: %8zu (%5.2f %%)\n", edges[i], buckets[i], pct_);
    }
    if (buckets[ne] > 0)
        std::printf("    >=%8u ns: %8zu (%5.2f %%)\n",
                    edges[ne - 1], buckets[ne],
                    100.0 * double(buckets[ne]) / double(n));
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    Mode mode = Mode::None;
    int seconds = 20;
    int num_outputs = 2;
    double load_us = 0.0;
    int explicit_iters = -1;
    std::string csv_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s requires arg\n", n); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--mode")    {
            std::string m = next("--mode");
            if      (m == "none") mode = Mode::None;
            else if (m == "cv")   mode = Mode::Cv;
            else if (m == "sem")  mode = Mode::Sem;
            else { std::fprintf(stderr, "unknown mode %s\n", m.c_str()); return 2; }
        }
        else if (a == "--seconds") seconds = std::atoi(next("--seconds"));
        else if (a == "--outputs") num_outputs = std::atoi(next("--outputs"));
        else if (a == "--load-us") load_us = std::atof(next("--load-us"));
        else if (a == "--iters")   explicit_iters = std::atoi(next("--iters"));
        else if (a == "--csv")     csv_path = next("--csv");
        else if (a == "-h" || a == "--help") {
            std::printf("usage: %s [--mode none|cv|sem] [--seconds N] "
                        "[--outputs N] [--csv path]\n", argv[0]);
            return 0;
        }
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    pw_init(&argc, &argv);

    Bench bench;
    bench.mode = mode;
    bench.num_outputs = num_outputs;

    std::fprintf(stderr, "[bench] creating thread loop\n");
    bench.loop = pw_thread_loop_new("sc_pw_bench", nullptr);
    if (!bench.loop) { std::fprintf(stderr, "thread_loop_new failed\n"); return 1; }

    pw_thread_loop_lock(bench.loop);
    std::fprintf(stderr, "[bench] creating stream\n");
    bench.stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(bench.loop),
        "sc_pw_bench",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE,     "Music",
            nullptr),
        &stream_events,
        &bench);
    if (!bench.stream) {
        std::fprintf(stderr, "pw_stream_new_simple failed\n");
        pw_thread_loop_unlock(bench.loop);
        return 1;
    }

    // Build audio format param.
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = uint32_t(num_outputs);
    info.rate = 0; // let pipewire pick
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    std::fprintf(stderr, "[bench] connecting stream\n");
    int conn = pw_stream_connect(
        bench.stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT
                               | PW_STREAM_FLAG_MAP_BUFFERS
                               | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (conn < 0) {
        std::fprintf(stderr, "pw_stream_connect failed: %d\n", conn);
        pw_thread_loop_unlock(bench.loop);
        return 1;
    }
    pw_thread_loop_unlock(bench.loop);

    std::fprintf(stderr, "[bench] starting thread loop\n");
    if (pw_thread_loop_start(bench.loop) < 0) {
        std::fprintf(stderr, "pw_thread_loop_start failed\n");
        return 1;
    }

    // Wait for STREAMING state.
    std::fprintf(stderr, "[bench] waiting for stream to start\n");
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (bench.callbacks.load() > 5) break;
    }
    const uint64_t preheat = bench.callbacks.load();
    std::fprintf(stderr, "[bench] %llu callbacks after preheat\n",
                 (unsigned long long)preheat);

    // Estimate period from preheat gap samples (median of what we have).
    {
        uint64_t h = bench.ring.head.load();
        std::vector<uint32_t> g;
        g.reserve(h);
        for (uint64_t i = 1; i < h; ++i)
            g.push_back(bench.ring.data[i & SampleRing::kMask].gap_ns);
        if (!g.empty()) {
            std::sort(g.begin(), g.end());
            bench.period_ns = double(g[g.size() / 2]);
            bench.period_ns_int = uint64_t(bench.period_ns);
            std::fprintf(stderr, "[bench] preheat period estimate: %.1f us\n",
                         bench.period_ns / 1e3);
        }
    }

    // Synthetic load: explicit --iters wins over --load-us.
    if (explicit_iters >= 0) {
        bench.load_iters = explicit_iters;
        std::fprintf(stderr, "[bench] explicit load iters=%d\n", bench.load_iters);
    } else if (load_us > 0.0) {
        bench.load_iters = bench.load.calibrate(load_us);
        std::fprintf(stderr, "[bench] load calibration: target=%.1fus iters=%d\n",
                     load_us, bench.load_iters);
    }

    // Reset ring and counters so only post-preheat samples are included.
    bench.ring.head.store(0);
    bench.callbacks.store(0);
    bench.deadline_misses.store(0);
    bench.last_callback_start_ns = 0;

    std::thread waiter(waiter_thread, &bench);

    const char* mode_name =
        mode == Mode::None ? "none" :
        mode == Mode::Cv   ? "cv_any" : "sem_t";
    std::printf("running: mode=%s seconds=%d outputs=%d\n",
                mode_name, seconds, num_outputs);

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::fprintf(stderr, "[bench] sleep done, tearing down\n");
    bench.running.store(false);
    switch (mode) {
    case Mode::None: break;
    case Mode::Cv:  bench.cv.Signal();  break;
    case Mode::Sem: bench.sem.Signal(); break;
    }
    waiter.join();

    const uint64_t head = bench.ring.head.load();
    std::vector<uint32_t> durs, gaps;
    durs.reserve(head);
    gaps.reserve(head > 1 ? head - 1 : 0);
    for (uint64_t i = 0; i < head; ++i) {
        Sample s = bench.ring.data[i & SampleRing::kMask];
        durs.push_back(s.dur_ns);
        if (i > 0) gaps.push_back(s.gap_ns);
    }
    if (!gaps.empty()) {
        std::vector<uint32_t> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        bench.period_ns = double(sorted[sorted.size() / 2]);
    }

    std::printf("\ncallbacks=%llu wakeups=%llu dropped=%llu deadline_misses=%llu load_iters=%d period=%.1fus\n",
                (unsigned long long)bench.callbacks.load(),
                (unsigned long long)bench.wakeups.load(),
                (unsigned long long)bench.ring.dropped.load(),
                (unsigned long long)bench.deadline_misses.load(),
                bench.load_iters,
                bench.period_ns / 1e3);

    print_stats(mode_name, "dur", durs, bench.period_ns);
    print_stats(mode_name, "gap", gaps, bench.period_ns);

    if (!csv_path.empty()) {
        std::ofstream f(csv_path);
        f << "dur_ns,gap_ns\n";
        for (uint64_t i = 0; i < head; ++i) {
            Sample s = bench.ring.data[i & SampleRing::kMask];
            f << s.dur_ns << ',' << s.gap_ns << '\n';
        }
        std::fprintf(stderr, "wrote %llu samples to %s\n",
                     (unsigned long long)head, csv_path.c_str());
    }

    pw_thread_loop_lock(bench.loop);
    pw_stream_disconnect(bench.stream);
    pw_stream_destroy(bench.stream);
    pw_thread_loop_unlock(bench.loop);
    pw_thread_loop_stop(bench.loop);
    pw_thread_loop_destroy(bench.loop);
    pw_deinit();
    return 0;
}
