# Local PipeWire backend for scsynth — status & handoff

This document exists so a future session (you or a future Claude instance)
can pick this work up without re-discovering context. It is deliberately
specific about file paths, commands, and what has and has not been tested.

## TL;DR (v1 current)

A **native PipeWire backend for scsynth** has been built as a local
experiment. It lives in `server/scsynth/SC_PipeWire.cpp`, is selected at
configure time with `-DAUDIOAPI=pipewire`, and is **full-duplex**: it
auto-connects both playback and capture streams to the default PipeWire
sink/source on this host, honours scsynth's `-H` flag for device
selection, discovers format via `param_changed`, parses latency params,
handles `-i 0` correctly (no input stream), and preserves v0's ~40% CPU
savings vs the libjack-on-PipeWire shim under load.

There is **no RFC, no upstream PR, no discussion with SC core**. This is a
private branch experiment. Do not ship anything from here without going
through that process first.

## What is new in v1 (this session)

- **Full duplex**: a second `pw_stream` with `PW_DIRECTION_INPUT` is
  created when `mWorld->mNumInputs > 0`. Both streams run on the same
  `pw_thread_loop` data thread, so their `process()` callbacks are
  serialized — the input callback writes the most recent capture buffer
  into a shared planar array (`mCaptureBuf`), and the output callback
  reads it during the DSP loop. Worst case is up to one quantum of
  input→output latency if the output callback runs before the input
  callback within a graph cycle.
- **Device selection via `-H`**: `mInDeviceName` is parsed as
  `"outTarget:inTarget"` (either half optional) and fed to
  `PW_KEY_TARGET_OBJECT` on each stream. Verified working by targeting
  `alsa_input.pci-0000_66_00.6.analog-stereo` for capture while leaving
  output on the default Yamaha sink.
- **Principled format discovery** via `param_changed(SPA_PARAM_Format)`
  on both streams, using `spa_format_audio_raw_parse`. No more fragile
  `pw_stream_get_time_n` fallback.
- **Latency reporting** via `param_changed(SPA_PARAM_Latency)` using
  `spa_latency_parse`. `mMaxOutputLatency` is now populated from the
  graph instead of hardcoded 0.
- **Quantum-change detection** (best effort): `RunOutput()` compares the
  current `nFrames` to `mCurrentQuantum` and, if they differ, logs a
  warning and drops the buffer. Mid-run quantum changes are rare in
  practice and mid-run `Reset(sr, bufsize)` is dangerous; this is the
  safer minimum.
- **Setup handshake is now condition-variable driven by
  `mOutFormatKnown && mInFormatKnown` plus the first process callback**
  rather than the v0 "grab rate from pw_stream_get_time_n" hack.
- **CPU accounting + xrun tag** match the SC_Jack.cpp measurement patch
  (instantaneous wall-clock ratio, EMA for `mAvgCPU`, running max for
  `mPeakCPU`, rate-limited `xrun` scprintf on `busy > period`). So the
  scsynth_sweep harness compares like-for-like.
- **Regression-tested**: `-i 0` (output only), `-i 2 -o 2` (small
  channel count), default `-i 8 -o 8`, and the full load sweep — all
  work, and the sweep shows no CPU regression vs v0.

## v1 sweep vs JACK (repeat of RESULTS.md Table 3 with the new backend)

6 s per cell, 50 Hz /status polling, matched wall-clock accounting on
both backends. `peak_max` is the worst-case single callback; `>100%` is
the count of /status samples where `peakCPU > 100%` (= real deadline
miss).

```
driver    synths   obs  avg_mean   avg_max  peak_mean  peak_max  peak_std     p99   >90%   >100%  xruns
--------------------------------------------------------------------------------------------------------
jack         100   100      7.21      8.24       9.45     10.34      0.52    10.34     0       0      0
pipewire     100   100      6.72      7.51       9.29     15.19      1.96    15.19     0       0      0
jack         500   500     34.30     36.05      36.89     40.28      2.01    40.28     0       0      0
pipewire     500   500     25.58     28.09      33.65     40.15      5.33    40.15     0       0      0
jack        1000  1000     67.44     72.14      72.59     82.89      5.46    82.89     0       0      0
pipewire    1000  1000     40.10     55.94      65.16     93.03     18.28    93.03    39       0      0
jack        2000  2000    124.56    165.47     138.74    178.22     22.44   178.22   227     227      6
pipewire    2000  2000     72.54     88.07      94.19    121.40     15.13   121.40   107     107      1
jack        3000  3000    194.69    242.13     215.44    267.53     33.14   267.53   145     145      4
pipewire    3000  3000    114.21    127.95     140.40    176.38     22.37   176.38   226     226      6
```

Headline is unchanged from v0: PW uses **~40% less CPU** at moderate
load, sustaining **~50% more voices** before deadline misses. PW remains
spikier than JACK at the 1000-synth load point (peak_std 18 vs 5) —
that's PipeWire's graph scheduling jitter, not the backend code.

Raw: `tools/jack_signal_bench/results/scsynth_sweep/v1_full.jsonl`.

## What works (v0, verified in this session)

1. **Build integration** — `cmake -DAUDIOAPI=pipewire .` configures cleanly.
   Both `scsynth` and `libscsynth` compile and link against
   `libpipewire-0.3.so.0`; no `libjack.so` dependency in the resulting
   scsynth binary (verified with `ldd`).
2. **Driver lifetime** — `SC_PipeWireDriver::DriverSetup` creates a
   `pw_thread_loop`, constructs a `pw_stream` with `Audio/Playback/DSP`
   media properties, connects with
   `PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS`,
   and **blocks on a condition variable until the first `process()`
   callback** observes the negotiated buffer size. This is the idiomatic way
   to make an async PipeWire negotiation look synchronous to scsynth's
   expectation of a fully-specified driver after `Setup()`.
3. **State tracking** — `state_changed` transitions log to scprintf.
   `ERROR`/`UNCONNECTED` after initial setup posts `mQuitProgram` so scsynth
   exits cleanly if the stream is lost. Observed sequence during boot:
   `unconnected -> connecting -> paused -> streaming`.
4. **RT process loop** — mirrors `SC_JackDriver::Run` line-for-line:
   `mDLL.Update` → `mFromEngine.Free` → `mToEngine.Perform` →
   `mOscPacketsToEngine.Perform` → per-block scheduler drain → `World_Run` →
   output copy → `mAudioSync.Signal`.
5. **Planar → interleaved output conversion** — scsynth's audio buses are
   planar (`outBuses[k*bufFrames + n]`); pw_stream's buffer is interleaved
   F32. The callback deinterleaves on the way out, respecting
   `outTouched[k] == bufCounter` for zero-fill of untouched channels, same
   semantics as SC_Jack.
6. **OSC round-trip** — `/status`, `/d_recv`, `/s_new`, `/n_free` all work.
   `/status.reply` reports realistic CPU usage (ugens=0 idle; peaks to
   ~0.155% under a single SinOsc). `numSynthDefs` increments with
   `/d_recv`.
7. **PipeWire graph integration** — `pw-link -l` shows ports named
   `SuperCollider:output_FL`, `output_FR`, … autoconnected to the default
   sink. Audio was audibly produced during the `/s_new default ... amp 0.2`
   test (a ~1.2s 440Hz tone through the Yamaha DM3).
8. **Supernova side** — `AUDIOAPI=pipewire` is accepted by
   `server/supernova/CMakeLists.txt` but transparently maps to the `jack`
   backend path (PipeWire's libjack shim) because supernova does not yet
   have its own pipewire driver and we did not want to break supernova
   builds in this experiment.

## What does NOT work / was not implemented in v0

v0 items that are now DONE in v1:

- ✅ Audio input (full duplex via second pw_stream)
- ✅ Device selection via `-H "outTarget:inTarget"`
- ✅ Latency reporting via `param_changed(SPA_PARAM_Latency)`
- ✅ Principled format discovery via `param_changed(SPA_PARAM_Format)`
- ✅ CPU hook + xrun counter (matched with SC_Jack.cpp local patch)
- ✅ `zeroAndQueue` helper extracted; `RunOutput()` is flatter than v0's `Run()`

Items that are still open as of v1 (ordered by practical importance):

1. **Quantum-change handling is best-effort, not complete.** If PipeWire
   renegotiates the buffer size mid-session, `RunOutput()` logs a warning
   and drops the buffer, but doesn't call `Reset(sampleRate, bufferSize)`
   to update `mNumSamplesPerCallback`. A proper fix would need to stop
   the streams, reset the driver, restart — all within the RT thread's
   context, which is tricky. Not verified in practice: I didn't force a
   quantum change in testing (would disrupt user's live PipeWire graph).
   Code path exists but is untested.
2. **Capture data flow verified only at the graph level.** `pw-link`
   confirms the input stream is connected and reaches `streaming`, and
   `RunInput()` dequeues buffers without crashing, but I have not
   confirmed end-to-end that captured samples actually reach a UGen via
   `SoundIn.ar`. That would need a binary `.scsyndef` with a `SoundIn`
   UGen or sclang to compile one. The pipeline is structurally correct
   but a real audio-in-to-audio-out passthrough test is pending.
3. **Cross-stream scheduling order is not enforced.** Input and output
   streams are independent graph nodes. Within a graph cycle PipeWire
   may call their `process()` in either order. If output runs first we
   get the previous cycle's input — up to one quantum of capture
   latency. For a 21 ms quantum that's imperceptible; for 64-sample
   (1.33 ms) setups it may matter. A real fix would either (a) couple
   the streams via an explicit graph link so PipeWire schedules them in
   order, or (b) switch to `pw_filter` with manual link creation. Not
   done.
4. **No format-param handling on capture latency.** Output latency is
   parsed but input latency is not — `mMaxOutputLatency` only reflects
   the output stream. Acceptable for now because mScheduled events are
   output-aligned.
5. **Supernova side not updated.** `AUDIOAPI=pipewire` still maps
   internally to `jack` for supernova, going through the libjack shim.
   Supernova has its own audio backend files; porting them is a
   separate project.
6. **No Windows / macOS build check.** Only compiled on Linux. The
   CMake branch is Linux-only in practice. macOS PipeWire exists but is
   not common; Windows has no PipeWire.
7. **No test suite integration.** No CI, no automated tests beyond the
   `scsynth_sweep.py` harness. Smoke-tested by hand.
8. **Code smells still present:**
   - `server_timeseed`, `oscTimeNow`, `initializeScheduler` are
     duplicated between `SC_PipeWire.cpp` and `SC_Jack.cpp`.
   - `RunOutput()` is still long — the DSP loop could be extracted to a
     helper shared with `SC_Jack.cpp`.
   - `mCaptureBuf` is a plain `std::vector<float>` resized once at
     setup. Fine as long as the quantum is stable. If quantum changes
     the capture buffer size becomes wrong.

## Usage reference

### `-H` / device selection syntax (v1)

The scsynth `-H` flag is parsed as `"outTarget:inTarget"`. Either half
may be empty:

- `-H "my-sink-name"` — target only the output sink, default input
- `-H ":my-source-name"` — default output, target only the input source
- `-H "sink-name:source-name"` — target both
- *(no `-H`)* — autoconnect both to system defaults

Targets are matched against PipeWire `node.name` or serial. Use
`pw-cli ls Node` or `wpctl status` to find the right names.

Example:

```sh
scsynth -u 57110 -H ":alsa_input.pci-0000_66_00.6.analog-stereo"
# → output = default sink, input = built-in analog mic
```

### Channel counts

`-i N` and `-o M` are respected. `-i 0` skips creating the input stream
entirely (no capture port will appear in pw-link). Default is 8/8.

## Real in-process sweep results (added in follow-up session)

With both backends using matched wall-clock CPU accounting (see "Local
patch to SC_Jack.cpp" below), measuring N voices of the default synthdef
at 6 s/cell, 50 Hz /status polling:

| load    | JACK avg % | PW avg % | JACK peak_max | PW peak_max | JACK >100% | PW >100% |
|--------:|-----------:|---------:|--------------:|------------:|-----------:|---------:|
|  100    |      7.1   |    6.1   |        11.1   |       9.8   |      0/300 |    0/300 |
|  500    |     34.4   |   26.1   |        43.8   |      36.2   |      0/300 |    0/300 |
| 1000    |     66.7   |   37.1   |        96.5   |      88.1   |      0/300 |    0/300 |
| 2000    |    124.2   |   72.4   |       189.3   |     157.8   |  229/229   | 113/229  |
| 3000    |    197.7   |  113.7   |       271.1   |     233.2   |  143/143   | 218/218  |

**Headline:** native PipeWire uses ~30-45% less CPU for the same DSP work,
raising the sustainable voice count from ~1500 (JACK) to ~2500 (PW) on
this hardware. But PW is *spikier* at moderate load (peak stddev 17 vs 6
at 1000 synths), so the practical safety margin is closer to 30-40% more
voices than a naive 50% reading of the mean-CPU numbers.

Raw results: `tools/jack_signal_bench/results/scsynth_sweep/all.jsonl`.
Harness: `scsynth_sweep.py`. Table: `tabulate_sweep.py`.

## Local patch to SC_Jack.cpp (follow-up session)

For the real in-process comparison, `SC_Jack.cpp::Run()` was locally
modified to measure callback wall time via `clock_gettime(CLOCK_MONOTONIC)`
and compute `cpuUsage = 100 * busyNs / periodNs` itself, replacing the
`jack_cpu_load(mClient)` call. The reason: on the PipeWire libjack shim,
`jack_cpu_load` returns a smoothed/capped value that cannot be compared
apples-to-apples with SC_PipeWire.cpp's direct wall-clock measurement. The
same change also adds an `xrun` scprintf tag (rate-limited by
`mPeakCounter`) when `busyNs > periodNs`, matching SC_PipeWire's xrun tag
so the sweep harness counts overruns identically on both backends.

**This is a measurement hack, not a correctness fix.** Revert before any
real JACK work if you rebuild stock scsynth on develop. The specific
changes in SC_Jack.cpp are bracketed by comments beginning
`Local experimental change (not upstream)`.

## Files touched in this session

Scsynth backend:
- `server/scsynth/SC_PipeWire.cpp` — **new**, the backend.
  Added `xrun` scprintf tag + instantaneous CPU measurement in the
  follow-up session so the harness can compare backends fairly.
- `server/scsynth/SC_Jack.cpp` — **local measurement patch** (see
  "Local patch to SC_Jack.cpp" section above). Replaces `jack_cpu_load`
  with wall-clock measurement and adds the matching `xrun` tag. Revert
  before real JACK work if you rebuild stock scsynth.
- `server/scsynth/SC_CoreAudio.h` — added `SC_AUDIO_API_PIPEWIRE 7`
- `server/scsynth/CMakeLists.txt` — added pipewire branch (regex,
  pkg_check_modules, source list, link libs)
- `server/supernova/CMakeLists.txt` — accepts pipewire, maps internally to
  jack path (supernova out of scope for this experiment)
- `lang/CMakeLists.txt` — accepts pipewire in the regex; no real change
  otherwise (sclang only uses the flag for device listing)
- `CMakeLists.txt` (top level) — updated the AUDIOAPI cache string help

Signaling fix (separate from backend, compatible, landed the same session):
- `common/SC_SyncCondition.h` — new `sem_t`-backed path behind
  `#if defined(__linux__) && !defined(__COBALT__)`; all other platforms
  keep the original `condition_variable_any` implementation. Rationale:
  `condition_variable_any::notify_one` on libstdc++ takes an internal
  mutex, which is RT-hostile. Verified by benchmark: 30% lower stddev and
  2× lower max in the low-load regime; no perf regression.

Benchmark harness (supporting work, not shipped):
- `tools/jack_signal_bench/jack_signal_bench.cpp` — standalone libjack
  client that isolates the `cv_any` vs `sem_t` signaling cost
- `tools/jack_signal_bench/pw_signal_bench.cpp` — pw_stream twin
- `tools/jack_signal_bench/extract.py` — stats extractor
- `tools/jack_signal_bench/results/*` — raw text and CSV from all runs
- `tools/jack_signal_bench/scsynth_sweep.py` — real in-process scsynth
  load sweep harness (added in follow-up session). Launches a given
  scsynth binary, loads default.scsyndef, spawns N voices, polls /status
  at 50 Hz, counts xrun tags, emits JSON summary.
- `tools/jack_signal_bench/tabulate_sweep.py` — renders sweep JSONL as a
  table
- `tools/jack_signal_bench/scsynth_jack`, `scsynth_pw` — the two
  side-by-side binaries built from the current tree

## How to build & run

**Switch to the pipewire backend** (from `build/` directory, existing
CMake cache):

```sh
cd build
cmake -DAUDIOAPI=pipewire .
cmake --build . --target scsynth -- -j4
```

Verify the resulting binary links against libpipewire, not libjack:

```sh
ldd build/server/scsynth/scsynth | grep -E 'pipewire|jack'
# expected: libpipewire-0.3.so.0  (no libjack line)
```

**Smoke test** — launch and check boot:

```sh
./build/server/scsynth/scsynth -u 57113 &
# expected output (truncated):
#   PipeWireDriver: state unconnected -> connecting
#   PipeWireDriver: state connecting -> paused
#   PipeWireDriver: state paused -> streaming
#   PipeWireDriver: negotiated 1024 samples @ 48000.0 Hz, N output channel(s)
#   SC_AudioDriver: sample rate = 48000.000000, driver's block size = 1024
#   SuperCollider 3 server ready.
kill %1
```

**Full round-trip test** — play a note via raw OSC:

```sh
# Tested scripts live in /tmp (not committed):
#   /tmp/osc_status.py   — sends /status, parses /status.reply
#   /tmp/play_note.py    — loads default.scsyndef, plays + frees a note
./build/server/scsynth/scsynth -u 57113 > /tmp/scsynth.log 2>&1 &
sleep 2
python3 /tmp/play_note.py 57113
pw-link -l | grep SuperCollider   # verify autoconnection
kill %1
```

The `default.scsyndef` file lives at
`testsuite/server/supernova/default.scsyndef` — 612 bytes, a basic
envelope-gated SinOsc.

**Switch back to jack**:

```sh
cmake -DAUDIOAPI=jack .
cmake --build . --target scsynth -- -j4
```

Both builds work from the same build directory; only the AUDIOAPI cache
var differs.

## Known good state for this session

- Commit: `462f0f1aa` (the `develop` tip when work started)
- Kernel: `Linux 6.17.0-111019-tuxedo`
- PipeWire: `1.0.5-1ubuntu3.2`
- libpipewire-0.3-dev: installed in this session
- Hardware: AMD Ryzen AI 9 365, Yamaha DM3 Pro USB audio interface
- Backend tested with 2 output channels at 1024-sample / 48 kHz quantum
  (PipeWire's default for this setup)

## Suggested next steps, in order

1. **Add input.** This is the biggest functional gap and is mechanical.
   Symmetric to output in all the obvious ways.
2. **Fix quantum change / sample-rate discovery** via `param_changed` on
   `SPA_PARAM_Format`. The current approach is fragile enough that it
   should be cleaned before any real work happens on top.
3. **Refactor `Run()`** to extract `zeroAndQueue()` helper and flatten the
   early-exit pyramid.
4. **Run the same load sweep we did with the benchmark harness** against
   the real in-process backend — i.e. scsynth with AUDIOAPI=pipewire vs
   AUDIOAPI=jack, running a CPU-bound test patch (lots of SinOsc /
   PinkNoise instances), compare `/status.reply` avgCPU/peakCPU under
   identical load. This is the real answer to "does a native backend fix
   scsynth's earlier-than-Pd dropout symptom". The standalone benchmark
   suggested yes, by about 50%; confirming on the real server would close
   the loop.
5. **If you want to go further**: investigate `PW_KEY_NODE_FORCE_QUANTUM`
   to pin the buffer size the way JACK does by default. Might reduce the
   gap/jitter noise we saw in the benchmark.
6. **Only then** consider taking this upstream as an RFC. Until then, keep
   it local and reversible.
