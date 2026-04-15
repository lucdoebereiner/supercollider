// jack_signal_bench.cpp
//
// Isolates the cost of SC's SC_SyncCondition::Signal() on the JACK rt callback.
// Opens a JACK client, does trivial DC-fill "DSP", signals an NRT waiter using
// one of three modes (none / cv_any / sem_t), records per-callback wall-clock
// durations with CLOCK_MONOTONIC, then prints statistics. Build:
//
//   g++ -O2 -std=c++17 -pthread jack_signal_bench.cpp -ljack -o jack_signal_bench
//
// Usage:
//   ./jack_signal_bench --mode cv   --seconds 20
//   ./jack_signal_bench --mode sem  --seconds 20
//   ./jack_signal_bench --mode none --seconds 20
//
// Optional:
//   --csv out.csv     dump every sample's nanosecond duration
//   --outputs N       number of output ports (default 2)

#include <jack/jack.h>

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
// Signalers

// A copy of SC_SyncCondition as of develop, minus the WaitOnce variant we
// don't exercise here. This is intentionally line-for-line equivalent so the
// benchmark faithfully measures what scsynth does today.
class CvAnySignaler {
public:
    void Signal() {
        ++write;
        available.notify_one();
    }
    void WaitNext() {
        std::unique_lock<std::mutex> lock(mutex);
        read = write;
        while (read == write)
            available.wait(lock);
    }

private:
    std::condition_variable_any available;
    std::mutex mutex;
    int read = 0, write = 0;
};

// sem_t-backed equivalent. Signal is a pure futex-wake; wait drains then
// blocks for a fresh post (mirroring WaitNext semantics).
class SemSignaler {
public:
    SemSignaler() { sem_init(&sem, 0, 0); }
    ~SemSignaler() { sem_destroy(&sem); }
    void Signal() { sem_post(&sem); }
    void WaitNext() {
        while (sem_trywait(&sem) == 0) { } // drain any accumulated posts
        while (sem_wait(&sem) != 0 && errno == EINTR) { }
    }

private:
    sem_t sem;
};

// ---------------------------------------------------------------------------
// Synthetic DSP load.
//
// A fixed-shape busy loop over a small float scratch array, calibrated at
// startup so that `iters_per_callback` takes approximately `target_us`
// microseconds of wall-clock time on this CPU. Same code lives in
// pw_signal_bench.cpp — keep them byte-for-byte identical so shim vs native
// compare like-for-like.

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

    // Calibrate iters_per_callback so that do_work(iters_per_callback)
    // takes approximately target_us microseconds. Warms the cache first,
    // then takes the median of a few timing runs.
    int calibrate(double target_us) {
        if (target_us <= 0.0) return 0;
        // warm
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
        // best is ns for `probe` iters.
        double ns_per_iter = double(best) / double(probe);
        double iters = (target_us * 1000.0) / ns_per_iter;
        return iters > 1.0 ? int(iters) : 1;
    }

private:
    float scratch[kScratchN];
};

// ---------------------------------------------------------------------------
// Lock-free SPSC ring of uint32 nanosecond samples.
// Power-of-two capacity. Dropping if full (we oversize to avoid loss).

struct Sample {
    uint32_t dur_ns; // time spent inside the callback
    uint32_t gap_ns; // interval from previous callback-start (scheduling jitter proxy)
};

struct SampleRing {
    static constexpr size_t kCap = 1u << 20; // 1M samples
    static constexpr size_t kMask = kCap - 1;
    std::vector<Sample> data{ kCap, Sample{ 0, 0 } };
    std::atomic<uint64_t> head{ 0 };      // writer index (rt thread only)
    std::atomic<uint64_t> dropped{ 0 };

    void push(Sample s) {
        uint64_t h = head.load(std::memory_order_relaxed);
        if (h >= kCap) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        data[h & kMask] = s;
        head.store(h + 1, std::memory_order_release);
    }
};

// ---------------------------------------------------------------------------
// Shared state

enum class Mode { None, Cv, Sem };

struct Bench {
    Mode mode = Mode::None;
    int num_outputs = 2;
    std::vector<jack_port_t*> out_ports;

    SampleRing ring;

    CvAnySignaler cv;
    SemSignaler sem;

    // Simulated NRT lock, drained by the waiter thread. Held briefly to
    // create realistic contention with the RT signaler (this is what
    // actually triggers the cv_any wait-mutex path).
    std::mutex nrt_lock;

    std::atomic<bool> running{ true };
    std::atomic<uint64_t> callbacks{ 0 };
    std::atomic<uint64_t> wakeups{ 0 };
    std::atomic<uint64_t> deadline_misses{ 0 };
    uint64_t last_callback_start_ns = 0; // rt-thread-only, no synchronization needed
    uint64_t period_ns = 0;              // set at init, read in callback

    LoadWork load;
    int load_iters = 0;
};

static Bench* g_bench = nullptr;

// ---------------------------------------------------------------------------
// JACK callback

static inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

static int process_cb(jack_nframes_t nframes, void* arg) {
    Bench* b = static_cast<Bench*>(arg);
    const uint64_t t0 = now_ns();

    // Fixed, trivial DSP: DC fill every output port. This is the user's
    // exact "constant DC" repro.
    for (jack_port_t* p : b->out_ports) {
        float* buf = static_cast<float*>(jack_port_get_buffer(p, nframes));
        for (jack_nframes_t i = 0; i < nframes; ++i)
            buf[i] = 0.25f;
    }

    // Synthetic load: fixed number of float ops, calibrated to a target us.
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
    if (b->period_ns && dur >= b->period_ns)
        b->deadline_misses.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// ---------------------------------------------------------------------------
// NRT waiter thread — mirrors SC_AudioDriver::RunThread

static void waiter_thread(Bench* b) {
    while (b->running.load(std::memory_order_relaxed)) {
        switch (b->mode) {
        case Mode::None: {
            // No signal path to exercise; sleep lightly so this thread
            // doesn't spin and skew measurements.
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            break;
        }
        case Mode::Cv:  b->cv.WaitNext();  break;
        case Mode::Sem: b->sem.WaitNext(); break;
        }
        // Briefly take the "NRT lock" to simulate fifo draining. This is the
        // window during which cv_any notify_one blocks on its internal mutex
        // against the waiter's own brief holds of that same mutex.
        {
            std::lock_guard<std::mutex> lk(b->nrt_lock);
            // Simulate a handful of cache-line-warm memory accesses.
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
    if (samples.empty()) {
        std::fprintf(stderr, "no samples\n");
        return;
    }
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    auto pct = [&](double p) {
        size_t idx = size_t(p * (n - 1));
        return samples[idx];
    };

    double sum = 0;
    for (uint32_t v : samples) sum += double(v);
    const double mean = sum / double(n);
    double sq = 0;
    for (uint32_t v : samples) {
        double d = double(v) - mean;
        sq += d * d;
    }
    const double stddev = std::sqrt(sq / double(n));

    const uint32_t p50 = pct(0.50);
    const uint32_t p90 = pct(0.90);
    const uint32_t p99 = pct(0.99);
    const uint32_t p999 = pct(0.999);
    const uint32_t p9999 = pct(0.9999);
    const uint32_t maxv = samples.back();

    std::printf("\n=== %s [%s] ===\n", label, field);
    std::printf("  samples:   %zu\n", n);
    std::printf("  period:    %.1f us (jack buffer period)\n", period_ns / 1e3);
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

    // Log-spaced histogram buckets in ns.
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
    std::printf("  histogram (count >= bucket):\n");
    for (int i = 0; i < ne; ++i) {
        if (buckets[i] == 0) continue;
        double pct = 100.0 * double(buckets[i]) / double(n);
        std::printf("    < %8u ns: %8zu (%5.2f %%)\n", edges[i], buckets[i], pct);
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
    std::fprintf(stderr, "[bench] entered main\n");
    Mode mode = Mode::None;
    int seconds = 20;
    int num_outputs = 2;
    double load_us = 0.0;
    int explicit_iters = -1; // if >=0, skip calibration
    std::string csv_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--mode") {
            std::string m = next("--mode");
            if      (m == "none") mode = Mode::None;
            else if (m == "cv")   mode = Mode::Cv;
            else if (m == "sem")  mode = Mode::Sem;
            else { std::fprintf(stderr, "unknown mode %s\n", m.c_str()); return 2; }
        } else if (a == "--seconds") {
            seconds = std::atoi(next("--seconds"));
        } else if (a == "--outputs") {
            num_outputs = std::atoi(next("--outputs"));
        } else if (a == "--load-us") {
            load_us = std::atof(next("--load-us"));
        } else if (a == "--iters") {
            explicit_iters = std::atoi(next("--iters"));
        } else if (a == "--csv") {
            csv_path = next("--csv");
        } else if (a == "-h" || a == "--help") {
            std::printf("usage: %s [--mode none|cv|sem] [--seconds N] "
                        "[--outputs N] [--csv path]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg %s\n", a.c_str());
            return 2;
        }
    }

    Bench bench;
    bench.mode = mode;
    bench.num_outputs = num_outputs;
    g_bench = &bench;

    std::fprintf(stderr, "[bench] opening jack\n");
    jack_status_t st;
    jack_client_t* client = jack_client_open("sc_signal_bench", JackNoStartServer, &st);
    if (!client) {
        std::fprintf(stderr, "jack_client_open failed (status=0x%x)\n", st);
        return 1;
    }
    const jack_nframes_t bufsize = jack_get_buffer_size(client);
    const jack_nframes_t srate = jack_get_sample_rate(client);
    const double period_ns = 1e9 * double(bufsize) / double(srate);
    bench.period_ns = uint64_t(period_ns);

    // Synthetic load configuration: explicit --iters wins over --load-us.
    if (explicit_iters >= 0) {
        bench.load_iters = explicit_iters;
        std::fprintf(stderr, "[bench] explicit load iters=%d\n", bench.load_iters);
    } else if (load_us > 0.0) {
        bench.load_iters = bench.load.calibrate(load_us);
        std::fprintf(stderr, "[bench] load calibration: target=%.1fus iters=%d\n",
                     load_us, bench.load_iters);
    }

    for (int i = 0; i < num_outputs; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "out_%d", i + 1);
        jack_port_t* p = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE,
                                            JackPortIsOutput, 0);
        if (!p) { std::fprintf(stderr, "port register failed\n"); return 1; }
        bench.out_ports.push_back(p);
    }

    jack_set_process_callback(client, process_cb, &bench);

    std::fprintf(stderr, "[bench] activating jack\n");
    if (jack_activate(client)) {
        std::fprintf(stderr, "jack_activate failed\n"); return 1;
    }

    std::fprintf(stderr, "[bench] starting waiter thread\n");
    std::thread waiter(waiter_thread, &bench);

    const char* mode_name =
        mode == Mode::None ? "none" :
        mode == Mode::Cv   ? "cv_any" : "sem_t";
    std::printf("running: mode=%s seconds=%d outputs=%d bufsize=%u srate=%u period=%.1fus\n",
                mode_name, seconds, num_outputs, bufsize, srate, period_ns / 1e3);

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::fprintf(stderr, "[bench] sleep done, tearing down\n");

    bench.running.store(false);
    // Wake the waiter so it can exit.
    switch (mode) {
    case Mode::None: break;
    case Mode::Cv:  bench.cv.Signal();  break;
    case Mode::Sem: bench.sem.Signal(); break;
    }
    std::fprintf(stderr, "[bench] joining waiter\n");
    waiter.join();

    // Drain ring.
    const uint64_t head = bench.ring.head.load();
    std::vector<uint32_t> durs, gaps;
    durs.reserve(head);
    gaps.reserve(head > 1 ? head - 1 : 0);
    for (uint64_t i = 0; i < head; ++i) {
        Sample s = bench.ring.data[i & SampleRing::kMask];
        durs.push_back(s.dur_ns);
        if (i > 0) gaps.push_back(s.gap_ns); // first sample has no gap
    }

    std::printf("\ncallbacks=%llu wakeups=%llu dropped=%llu deadline_misses=%llu load_iters=%d\n",
                (unsigned long long)bench.callbacks.load(),
                (unsigned long long)bench.wakeups.load(),
                (unsigned long long)bench.ring.dropped.load(),
                (unsigned long long)bench.deadline_misses.load(),
                bench.load_iters);

    print_stats(mode_name, "dur", durs, period_ns);
    print_stats(mode_name, "gap", gaps, period_ns);

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
    // Skip jack_client_close: the pipewire-jack shim (pipewire 1.0.5 on this
    // system) segfaults inside client cleanup. Since we're done measuring,
    // _exit bypasses global destructors and terminates cleanly. Harmless for
    // a single-shot benchmark.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
