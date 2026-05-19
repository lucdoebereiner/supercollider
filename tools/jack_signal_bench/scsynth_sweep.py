#!/usr/bin/env python3
"""
Real in-process scsynth load sweep: compare AUDIOAPI=jack vs AUDIOAPI=pipewire
builds at various voice counts, measuring avg/peak CPU and dropout-like
events from /status.reply polls and stderr.

Usage:
    scsynth_sweep.py --binary ./scsynth_pw --synths 500 --seconds 8

Prints one summary line (JSON) on stdout. Intended to be driven by a shell
loop; the loop aggregates into a table.

Method:
1. Launch scsynth binary with -u <port> as a subprocess.
2. Wait for "server ready" in its stderr.
3. /d_recv the default synthdef (612 bytes, ~SinOsc+envelope).
4. /s_new a given number of synths at distributed frequencies, amp scaled
   so the overall sum stays in a sane range.
5. Poll /status at ~50 Hz for `seconds` seconds, recording each reply's
   (avgCPU, peakCPU, actualSampleRate).
6. Count "late" lines in scsynth stderr as dropout-like events.
7. Free all synths, wait for scsynth to become idle again, quit.
8. Emit summary JSON.
"""

import argparse, json, os, socket, struct, subprocess, sys, time

# Default synthdef path: testsuite/server/supernova/default.scsyndef relative
# to the repo root (this script lives in tools/jack_signal_bench/). Override
# with --synthdef if running outside the SC tree.
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir, os.pardir))
SYNTHDEF = os.path.join(_REPO_ROOT, "testsuite", "server", "supernova", "default.scsyndef")


def pad4(b):
    return b + b"\x00" * ((-len(b)) % 4)


def osc_string(s):
    return pad4(s.encode() + b"\x00")


def osc_blob(b):
    return pad4(struct.pack(">i", len(b)) + b)


def osc_msg(addr, types, *args):
    out = osc_string(addr) + osc_string("," + types)
    ai = 0
    for t in types:
        v = args[ai]
        ai += 1
        if t == "i":
            out += struct.pack(">i", v)
        elif t == "f":
            out += struct.pack(">f", v)
        elif t == "s":
            out += osc_string(v)
        elif t == "b":
            out += osc_blob(v)
    return out


def parse_status_reply(data):
    """Return dict of fields from a /status.reply, or None if not one."""
    addr_nul = data.index(b"\x00")
    addr = data[:addr_nul].decode(errors="replace")
    if addr != "/status.reply":
        return None
    addr_end = ((addr_nul // 4) + 1) * 4
    types_nul = data.index(b"\x00", addr_end)
    types_end = ((types_nul // 4) + 1) * 4
    body = data[types_end:]
    # ,iiiiiffdd: unused, ugens, synths, groups, defs, avgCPU, peakCPU, sr, asr
    unused, ugens, synths, groups, defs = struct.unpack_from(">5i", body, 0)
    avgCPU, peakCPU = struct.unpack_from(">2f", body, 20)
    sr, asr = struct.unpack_from(">2d", body, 28)
    return {
        "ugens": ugens,
        "synths": synths,
        "groups": groups,
        "defs": defs,
        "avg": avgCPU,
        "peak": peakCPU,
        "sr": sr,
        "asr": asr,
    }


def drain_scsynth_output(proc, fd_accum):
    """Non-blocking read of scsynth stdout (scprintf goes there) into accum."""
    stream = proc.stdout
    if stream is None:
        return
    try:
        chunk = stream.read()
        if chunk:
            fd_accum.append(chunk)
    except BlockingIOError:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True, help="scsynth binary to test")
    ap.add_argument("--synths", type=int, required=True)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--port", type=int, default=57113)
    ap.add_argument("--label", default=None, help="label to include in output")
    ap.add_argument("--synthdef", default=SYNTHDEF,
                    help="path to a binary .scsyndef (default: testsuite/.../default.scsyndef in the SC tree)")
    ap.add_argument("--inputs", type=int, default=None, help="scsynth -i (input channel count)")
    ap.add_argument("--outputs", type=int, default=None, help="scsynth -o (output channel count)")
    args = ap.parse_args()

    env = os.environ.copy()
    # Use a unique PipeWire client name per run so they don't collide if a
    # previous run hasn't fully quit yet.
    env["PIPEWIRE_NODE_NAME"] = f"sc_sweep_{args.port}"

    # Launch scsynth. Capture stdout (scprintf goes there) so we can count
    # xrun tags. -n (nodes), -m (RT mem KB), -w (wire bufs), -a (audio
    # buses) are all bumped so the sweep can actually instantiate many
    # voices instead of silently capping at scsynth defaults.
    scsynth_argv = [
        args.binary,
        "-u", str(args.port),
        "-n", "16384",     # max nodes
        "-m", "131072",    # RT memory KB (128 MB)
        "-w", "8192",      # wire buffers
        "-a", "4096",      # audio bus channels
    ]
    if args.inputs is not None:
        scsynth_argv += ["-i", str(args.inputs)]
    if args.outputs is not None:
        scsynth_argv += ["-o", str(args.outputs)]
    proc = subprocess.Popen(
        scsynth_argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env=env,
    )

    # Wait for "server ready" to appear in stdout.
    import fcntl

    assert proc.stdout is not None
    fl = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, fl | os.O_NONBLOCK)

    ready = False
    driver_type = "?"
    deadline = time.time() + 10.0
    stderr_accum = []
    while time.time() < deadline:
        try:
            chunk = proc.stdout.read()
        except BlockingIOError:
            chunk = None
        if chunk:
            stderr_accum.append(chunk)
            joined = b"".join(stderr_accum).decode(errors="replace")
            if "PipeWireDriver" in joined:
                driver_type = "pipewire"
            elif "JackDriver" in joined:
                driver_type = "jack"
            if "SuperCollider 3 server ready" in joined:
                ready = True
                break
        time.sleep(0.05)
    if not ready:
        proc.terminate()
        proc.wait(timeout=5)
        print(
            json.dumps(
                {
                    "label": args.label,
                    "binary": args.binary,
                    "synths": args.synths,
                    "error": "server did not reach ready",
                    "stderr_tail": b"".join(stderr_accum).decode(errors="replace")[-500:],
                }
            )
        )
        return 2

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.1)
    sock.bind(("127.0.0.1", 0))
    target = ("127.0.0.1", args.port)

    # Load synthdef.
    with open(args.synthdef, "rb") as f:
        sdef_blob = f.read()
    sock.sendto(osc_msg("/d_recv", "b", sdef_blob), target)
    time.sleep(0.1)
    drain_scsynth_output(proc, stderr_accum)

    # Spawn N voices. Frequencies distributed across 100..1000 Hz so the
    # sum is non-periodic but still audible. amp scaled by 1/sqrt(N) so total
    # level is roughly constant (we're not listening, just loading).
    import math

    base_id = 10000
    amp = 0.05 / math.sqrt(max(1, args.synths))
    for i in range(args.synths):
        freq = 100.0 + (900.0 * i / max(1, args.synths - 1)) if args.synths > 1 else 440.0
        msg = osc_msg(
            "/s_new", "siiisfsf", "default", base_id + i, 0, 0, "freq", float(freq), "amp", float(amp)
        )
        sock.sendto(msg, target)
        # Pace spawning so we don't overflow scsynth's OSC input fifo.
        if i % 64 == 0:
            time.sleep(0.005)
    # Let voices settle and the /status peak counter stabilize.
    time.sleep(0.5)
    drain_scsynth_output(proc, stderr_accum)

    # Poll /status at ~50 Hz for `seconds`.
    samples = []
    poll_interval = 0.020  # 50 Hz
    t_end = time.time() + args.seconds
    next_tick = time.time()
    while time.time() < t_end:
        sock.sendto(osc_msg("/status", ""), target)
        # Drain replies; only keep /status.reply. Other messages (/done,
        # /n_end, etc.) get ignored.
        try:
            while True:
                data, _ = sock.recvfrom(4096)
                parsed = parse_status_reply(data)
                if parsed:
                    samples.append(parsed)
                    break
        except socket.timeout:
            pass
        next_tick += poll_interval
        sleep_for = next_tick - time.time()
        if sleep_for > 0:
            time.sleep(sleep_for)
    drain_scsynth_output(proc, stderr_accum)

    # Free synths.
    for i in range(args.synths):
        sock.sendto(osc_msg("/n_free", "i", base_id + i), target)
        if i % 128 == 0:
            time.sleep(0.002)
    time.sleep(0.3)
    drain_scsynth_output(proc, stderr_accum)

    # Kill scsynth.
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    drain_scsynth_output(proc, stderr_accum)

    # Summarize. xrun tag is emitted by both backends (JACK via its
    # jack_set_xrun_callback, PipeWire via our dur > period check).
    stderr_text = b"".join(stderr_accum).decode(errors="replace")
    xrun_count = stderr_text.count(": xrun")
    late_count = stderr_text.count("late ")  # scheduled OSC bundle late, distinct
    failure_count = stderr_text.count("FAILURE IN SERVER")

    if not samples:
        print(
            json.dumps(
                {
                    "label": args.label,
                    "binary": args.binary,
                    "driver": driver_type,
                    "synths": args.synths,
                    "error": "no /status samples",
                    "stderr_tail": stderr_text[-500:],
                }
            )
        )
        return 3

    avgs = [s["avg"] for s in samples]
    peaks = [s["peak"] for s in samples]
    avgs_sorted = sorted(avgs)
    peaks_sorted = sorted(peaks)

    def pct(lst, p):
        return lst[max(0, min(len(lst) - 1, int(p * (len(lst) - 1))))]

    def stats(lst):
        n = len(lst)
        mean = sum(lst) / n
        var = sum((x - mean) ** 2 for x in lst) / n
        return mean, var**0.5

    avg_mean, avg_std = stats(avgs)
    peak_mean, peak_std = stats(peaks)
    over90 = sum(1 for p in peaks if p > 90.0)
    over100 = sum(1 for p in peaks if p > 100.0)
    active_synths = samples[len(samples) // 2]["synths"] if samples else -1

    out = {
        "label": args.label,
        "binary": os.path.basename(args.binary),
        "driver": driver_type,
        "synths_requested": args.synths,
        "synths_observed": active_synths,
        "samples": len(samples),
        "seconds": args.seconds,
        "avg_mean": round(avg_mean, 3),
        "avg_std": round(avg_std, 3),
        "avg_max": round(max(avgs), 3),
        "peak_mean": round(peak_mean, 3),
        "peak_std": round(peak_std, 3),
        "peak_p50": round(pct(peaks_sorted, 0.50), 3),
        "peak_p90": round(pct(peaks_sorted, 0.90), 3),
        "peak_p99": round(pct(peaks_sorted, 0.99), 3),
        "peak_max": round(max(peaks), 3),
        "over_90_pct_count": over90,
        "over_100_pct_count": over100,
        "xruns": xrun_count,
        "late_msgs": late_count,
        "failures": failure_count,
    }
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
