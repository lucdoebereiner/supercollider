// pw_filter_autolink_test.cpp
//
// One-shot test: create a pw_filter with 2 input + 2 output DSP ports,
// connect it, print every state transition, run for a few seconds, then
// exit. The goal is to determine whether wireplumber auto-links a DSP
// filter to default source/sink on this system — if yes, the real
// SC_PipeWire backend rewrite can use pw_filter cleanly.
//
// Build:
//   g++ -O2 -std=c++17 -pthread pw_filter_autolink_test.cpp \
//       $(pkg-config --cflags libpipewire-0.3) \
//       $(pkg-config --libs libpipewire-0.3) -o pw_filter_autolink_test

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <chrono>
#include <cstdio>
#include <thread>

struct Ctx {
    pw_filter* filter = nullptr;
    int process_calls = 0;
};

static void on_state_changed(void* data, enum pw_filter_state old, enum pw_filter_state state,
                             const char* error) {
    (void)data;
    std::fprintf(stderr, "[filter] state: %s -> %s%s%s\n", pw_filter_state_as_string(old),
                 pw_filter_state_as_string(state), error ? " err=" : "", error ? error : "");
}

static void on_process(void* data, spa_io_position* pos) {
    auto* c = static_cast<Ctx*>(data);
    c->process_calls++;
    if (c->process_calls == 1) {
        std::fprintf(stderr, "[filter] first process() — quantum=%u rate=%u/%u\n",
                     pos ? pos->clock.duration : 0,
                     pos ? pos->clock.rate.num : 0,
                     pos ? pos->clock.rate.denom : 0);
    }
}

static const pw_filter_events kEvents = [] {
    pw_filter_events e = {};
    e.version = PW_VERSION_FILTER_EVENTS;
    e.state_changed = on_state_changed;
    e.process = on_process;
    return e;
}();

int main(int argc, char** argv) {
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    pw_init(&argc, &argv);

    Ctx ctx;

    pw_thread_loop* loop = pw_thread_loop_new("autolink_test", nullptr);
    pw_thread_loop_lock(loop);

    ctx.filter = pw_filter_new_simple(
        pw_thread_loop_get_loop(loop), "sc_autolink_test",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,       "Audio",
            PW_KEY_MEDIA_CATEGORY,   "Duplex",    // we are both capture + playback
            PW_KEY_MEDIA_ROLE,       "DSP",
            PW_KEY_MEDIA_CLASS,      "Stream/Output/Audio",
            PW_KEY_NODE_AUTOCONNECT, "true",
            nullptr),
        &kEvents, &ctx);

    auto add = [&](enum pw_direction dir, const char* name) {
        pw_filter_add_port(
            ctx.filter, dir,
            (enum pw_filter_port_flags)PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(int),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, name,
                              PW_KEY_AUDIO_CHANNEL,
                              (dir == PW_DIRECTION_INPUT ? "FL" : "FL"),
                              nullptr),
            nullptr, 0);
    };

    add(PW_DIRECTION_INPUT, "in_1");
    add(PW_DIRECTION_INPUT, "in_2");
    add(PW_DIRECTION_OUTPUT, "out_1");
    add(PW_DIRECTION_OUTPUT, "out_2");

    int r = pw_filter_connect(ctx.filter,
                              (enum pw_filter_flags)(PW_FILTER_FLAG_RT_PROCESS),
                              nullptr, 0);
    std::fprintf(stderr, "[filter] pw_filter_connect returned %d\n", r);

    pw_thread_loop_unlock(loop);

    pw_thread_loop_start(loop);

    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::fprintf(stderr, "[filter] t+%ds process_calls=%d\n", i + 1, ctx.process_calls);
    }

    pw_thread_loop_lock(loop);
    pw_filter_disconnect(ctx.filter);
    pw_filter_destroy(ctx.filter);
    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
    pw_thread_loop_destroy(loop);
    pw_deinit();
    return 0;
}
