#!/usr/bin/env bash
# Reproducibility benchmark for the experimental PipeWire audio backend
# for scsynth. Compares AUDIOAPI=jack and AUDIOAPI=pipewire builds at
# matched DSP load:
#
#   - default kernel scheduling (unpinned)
#   - both backends pinned to the same CPUs (taskset)
#   - optionally `perf stat` for clock frequency / instruction counts
#
# Output: a single markdown report you can paste into a forum thread.
#
# Requirements (all in the SC source tree):
#   - tools/jack_signal_bench/scsynth_sweep.py  (the harness)
#   - tools/jack_signal_bench/scsynth_jack      (built with -DAUDIOAPI=jack)
#   - tools/jack_signal_bench/scsynth_pw        (built with -DAUDIOAPI=pipewire)
#
# Usage:
#   ./forum_bench.sh                     # default settings
#   ./forum_bench.sh --voices 500,1500   # custom load points
#   ./forum_bench.sh --pin 0-3           # which CPUs to taskset to
#   ./forum_bench.sh --no-pin            # skip the pinned half
#   ./forum_bench.sh --with-perf         # also collect perf stat
#   ./forum_bench.sh --seconds 12        # per-cell measurement length
#
# Total runtime at defaults: ~3-5 minutes.

set -eu
set -o pipefail

VOICES="500,1000,2000"
PIN="0-3"
DO_PIN=1
DO_PERF=0
SECONDS_PER_CELL=8

while [[ $# -gt 0 ]]; do
    case "$1" in
        --voices)    VOICES="$2"; shift 2 ;;
        --pin)       PIN="$2"; shift 2 ;;
        --no-pin)    DO_PIN=0; shift ;;
        --with-perf) DO_PERF=1; shift ;;
        --seconds)   SECONDS_PER_CELL="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "unknown flag: $1" >&2; exit 2 ;;
    esac
done

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
HARNESS="$HERE/scsynth_sweep.py"
JACK_BIN="$HERE/scsynth_jack"
PW_BIN="$HERE/scsynth_pw"

for f in "$HARNESS" "$JACK_BIN" "$PW_BIN"; do
    if [[ ! -e "$f" ]]; then
        echo "missing: $f" >&2
        echo >&2
        echo "Build both backends from the same source tree, then copy:" >&2
        echo "  cmake -DAUDIOAPI=jack -DSUPERNOVA=OFF -B build-jack ../ && make -C build-jack scsynth -j" >&2
        echo "  cp build-jack/server/scsynth/scsynth $JACK_BIN" >&2
        echo "  cmake -DAUDIOAPI=pipewire -DSUPERNOVA=OFF -B build-pw ../ && make -C build-pw scsynth -j" >&2
        echo "  cp build-pw/server/scsynth/scsynth $PW_BIN" >&2
        exit 1
    fi
done

OUTDIR="$HERE/results/forum_run_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"
RESULTS="$OUTDIR/all.jsonl"
REPORT="$HERE/forum_bench_results.md"
: > "$RESULTS"

IFS=',' read -ra VOICE_ARR <<< "$VOICES"

run_cell() {
    local bin="$1" n="$2" label="$3" pincmd="$4"
    local port=$((59000 + RANDOM % 1000))
    echo "  $label" >&2
    if [[ -n "$pincmd" ]]; then
        $pincmd python3 "$HARNESS" --binary "$bin" --synths "$n" \
            --seconds "$SECONDS_PER_CELL" --inputs 0 --outputs 2 \
            --label "$label" --port "$port" \
            >> "$RESULTS" 2>> "$OUTDIR/stderr.log"
    else
        python3 "$HARNESS" --binary "$bin" --synths "$n" \
            --seconds "$SECONDS_PER_CELL" --inputs 0 --outputs 2 \
            --label "$label" --port "$port" \
            >> "$RESULTS" 2>> "$OUTDIR/stderr.log"
    fi
    sleep 2
}

echo "[1/3] unpinned sweep" >&2
for bin_path in "$JACK_BIN" "$PW_BIN"; do
    bin_name=$(basename "$bin_path")
    for n in "${VOICE_ARR[@]}"; do
        run_cell "$bin_path" "$n" "${bin_name}_unpinned_n${n}" ""
    done
done

if [[ $DO_PIN -eq 1 ]]; then
    echo "[2/3] pinned to CPUs $PIN" >&2
    for bin_path in "$JACK_BIN" "$PW_BIN"; do
        bin_name=$(basename "$bin_path")
        for n in "${VOICE_ARR[@]}"; do
            run_cell "$bin_path" "$n" "${bin_name}_pinned_n${n}" "taskset -c $PIN"
        done
    done
fi

PERF_OK=0
if [[ $DO_PERF -eq 1 ]]; then
    paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)
    if (( paranoid > 1 )); then
        echo "[3/3] perf stat: skipped (kernel.perf_event_paranoid=$paranoid)" >&2
        echo "      enable with: sudo sysctl kernel.perf_event_paranoid=1" >&2
    else
        echo "[3/3] perf stat (mid-load cell only)" >&2
        PERF_OK=1
        n_mid="${VOICE_ARR[$(( ${#VOICE_ARR[@]} / 2 ))]}"
        PIN_CMD=""
        [[ $DO_PIN -eq 1 ]] && PIN_CMD="taskset -c $PIN"
        for bin_path in "$JACK_BIN" "$PW_BIN"; do
            bin_name=$(basename "$bin_path")
            port=$((59500 + RANDOM % 100))
            $PIN_CMD python3 "$HARNESS" --binary "$bin_path" --synths "$n_mid" \
                --seconds 18 --inputs 0 --outputs 2 \
                --label "${bin_name}_perf_n${n_mid}" --port "$port" \
                > "$OUTDIR/sweep_perf_${bin_name}.log" 2>&1 &
            SWEEP_PID=$!
            sleep 6
            SC_PID=$(pgrep -P $SWEEP_PID -f "$bin_name" | head -1 || true)
            if [[ -n "${SC_PID:-}" ]]; then
                echo "  perf stat on $bin_name (pid $SC_PID)" >&2
                perf stat -p "$SC_PID" \
                    -e task-clock,cycles,instructions,cache-misses \
                    -- sleep 8 > "$OUTDIR/perf_${bin_name}.txt" 2>&1 || true
            fi
            wait $SWEEP_PID 2>/dev/null || true
            sleep 1
        done
    fi
fi

# Generate the markdown report.
{
    echo "## PipeWire vs JACK-shim — scsynth benchmark"
    echo
    echo "- **Host:** $(hostname)"
    echo "- **CPU:** $(lscpu | awk -F: '/Model name/{sub(/^ +/,"",$2); print $2; exit}')"
    echo "- **Logical CPUs:** $(nproc)"
    echo "- **Kernel:** $(uname -r)"
    if command -v pipewire >/dev/null 2>&1; then
        echo "- **PipeWire:** $(pipewire --version 2>&1 | head -1)"
    fi
    echo "- **Load points:** $VOICES voices of the default synthdef"
    echo "- **Per-cell duration:** ${SECONDS_PER_CELL} s, /status polled at 50 Hz"
    echo "- **Channels:** -i 0 -o 2"
    echo
    if [[ $DO_PIN -eq 1 ]]; then
        echo "- **Pinned sweep:** \`taskset -c $PIN\` applied to both backends"
    fi
    echo
    python3 - "$RESULTS" "$DO_PIN" "$PIN" <<'PY'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
do_pin = int(sys.argv[2])
pin_arg = sys.argv[3]

bins_seen = sorted(set(r['binary'] for r in rows))
jack_bin = next((b for b in bins_seen if 'jack' in b.lower()), None)
pw_bin   = next((b for b in bins_seen if 'pw' in b.lower() or 'pipewire' in b.lower()), None)
counts   = sorted(set(r['synths_requested'] for r in rows))

def get(bin_name, n, kind):
    label = f"{bin_name}_{kind}_n{n}"
    for r in rows:
        if r.get('label') == label:
            return r
    return None

def table(kind, title):
    print(f"### {title}")
    print()
    print("| voices | PW avg | PW peak max | PW xruns | JACK avg | JACK peak max | JACK xruns | JACK − PW |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|")
    for n in counts:
        pw = get(pw_bin, n, kind)
        jk = get(jack_bin, n, kind)
        if not pw or not jk:
            continue
        gap = ((jk['avg_mean'] - pw['avg_mean']) / jk['avg_mean'] * 100) if jk['avg_mean'] > 0 else 0
        print(f"| {n} "
              f"| {pw['avg_mean']:.1f}% | {pw['peak_max']:.1f}% | {pw['xruns']} "
              f"| {jk['avg_mean']:.1f}% | {jk['peak_max']:.1f}% | {jk['xruns']} "
              f"| {gap:+.0f}% |")
    print()

table("unpinned", "Default scheduling (unpinned)")
if do_pin:
    table("pinned", f"Pinned to CPUs {pin_arg} (taskset)")
PY

    if [[ $PERF_OK -eq 1 ]]; then
        echo "### Clock frequency and instructions (\`perf stat\`)"
        echo
        for f in "$OUTDIR"/perf_*.txt; do
            bn=$(basename "$f" .txt | sed 's/^perf_//')
            echo "**$bn**"
            echo
            echo '```'
            grep -E "task-clock|cycles|instructions|GHz|insn per cycle" "$f" | sed 's/^ */  /' | head -8
            echo '```'
            echo
        done
        echo "If \`cycles\` differs significantly while \`instructions\` are similar,"
        echo "the gap is mostly CPU frequency (which core the audio thread ran on),"
        echo "not extra work."
        echo
    fi

    echo "### How this was run"
    echo
    cmd="$(basename "$0") --voices $VOICES --seconds $SECONDS_PER_CELL"
    if [[ $DO_PIN -eq 1 ]]; then cmd="$cmd --pin $PIN"; else cmd="$cmd --no-pin"; fi
    if [[ $DO_PERF -eq 1 ]]; then cmd="$cmd --with-perf"; fi
    echo '```sh'
    echo "$cmd"
    echo '```'
} > "$REPORT"

echo >&2
echo "=== DONE ===" >&2
echo "report:   $REPORT" >&2
echo "raw data: $OUTDIR/" >&2
echo >&2
cat "$REPORT"
