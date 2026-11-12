#!/usr/bin/env python3
"""
End-to-end tests for the local PipeWire backend of scsynth (AUDIOAPI=pipewire),
plus a smoke test of supernova (which uses JACK via pipewire-jack).

Everything is routed through temporary null sinks created with pactl, so no
sound reaches real hardware and other running servers are not touched (the
tests use UDP ports 57190+). Requires: a running PipeWire (+ pipewire-pulse),
pactl, pw-play, pw-record, pw-dump, numpy.

Usage:
    pw_backend_test.py [--build BUILD_DIR] [--only PATTERN] [--stress N] [-v]

Each test prints PASS/FAIL with a short reason; exit status is the number of
failed tests (0 == all good).

Tests:
  boot_matrix     scsynth boots/quits cleanly for many -i/-o combinations,
                  including 0 inputs, 0 outputs and 0/0, and engine time
                  advances at real-time rate (a 1 s Line frees after ~1 s).
  graph_nodes     the PipeWire graph shows the expected nodes/ports for each
                  configuration (no playback node with -o 0, an unlinked
                  1-port clock node with -i 0 -o 0, client name from -H).
  output_signal   4 sines on 4 outputs arrive on the right channels at the
                  right frequency/level, with no gaps.
  duplex_signal   a stereo test tone played into a null sink is captured by
                  scsynth, passed through, and arrives at the output intact
                  and gap-free (checks the capture ring).
  input_only      with -o 0 the capture stream drives the engine; captured
                  level is right and gap-free.
  load            ~800 sines on one output while a reference sine on another
                  must stay clean; no xruns reported.
  sched_timing    OSC bundles scheduled ahead are executed on time.
  stress_reboot   N fast boot/quit cycles across configs: no hangs/crashes.
  supernova       supernova boots/quits for the same channel combinations and
                  produces output.
"""

import argparse, math, os, re, shutil, socket, struct, subprocess, sys, tempfile, time, wave, json

import numpy as np

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir, os.pardir))

# ----------------------------------------------------------------------------
# OSC


def _pad4(b):
    return b + b"\x00" * ((-len(b)) % 4)


def _ostr(s):
    return _pad4(s.encode() + b"\x00")


def osc_msg(addr, *args):
    types, data = ",", b""
    for a in args:
        if isinstance(a, bool):
            a = int(a)
        if isinstance(a, int):
            types += "i"
            data += struct.pack(">i", a)
        elif isinstance(a, float):
            types += "f"
            data += struct.pack(">f", a)
        elif isinstance(a, str):
            types += "s"
            data += _ostr(a)
        elif isinstance(a, (bytes, bytearray)):
            types += "b"
            data += _pad4(struct.pack(">i", len(a)) + bytes(a))
        else:
            raise TypeError(a)
    return _ostr(addr) + _ostr(types) + data


def osc_bundle(timetag, *msgs):
    out = _ostr("#bundle") + struct.pack(">Q", timetag)
    for m in msgs:
        out += struct.pack(">i", len(m)) + m
    return out


def osc_parse(data):
    i = data.index(b"\x00")
    addr = data[:i].decode(errors="replace")
    p = (i // 4 + 1) * 4
    if p >= len(data):
        return addr, []
    j = data.index(b"\x00", p)
    types = data[p + 1 : j].decode()
    p = (j // 4 + 1) * 4
    args = []
    for t in types:
        if t == "i":
            args.append(struct.unpack_from(">i", data, p)[0]); p += 4
        elif t == "f":
            args.append(struct.unpack_from(">f", data, p)[0]); p += 4
        elif t == "d":
            args.append(struct.unpack_from(">d", data, p)[0]); p += 8
        elif t == "s":
            k = data.index(b"\x00", p)
            args.append(data[p:k].decode(errors="replace")); p = (k // 4 + 1) * 4
        elif t == "b":
            n = struct.unpack_from(">i", data, p)[0]; p += 4
            args.append(data[p : p + n]); p += (n + 3) // 4 * 4
    return addr, args


NTP_EPOCH = 2208988800


def osc_timetag(t_unix):
    secs = t_unix + NTP_EPOCH
    return (int(secs) << 32) | int((secs % 1) * (1 << 32))


# ----------------------------------------------------------------------------
# Minimal SynthDef (SCgf v2) writer
#
# A def is described as a list of ugens; inputs refer to ("c", value) for
# constants, ("p", name) for control params, or (ugen_index, output_index).


class Def:
    def __init__(self, name, params):
        self.name = name
        self.params = list(params)  # [(name, default)]
        self.consts = []
        self.ugens = []  # (cls, rate, inputs, nouts, special)
        if self.params:
            self.ugens.append(("Control", 1, [], len(self.params), 0))

    def const(self, v):
        v = float(v)
        if v not in self.consts:
            self.consts.append(v)
        return ("c", v)

    def p(self, name):
        return (0, [n for n, _ in self.params].index(name))

    def u(self, cls, rate, inputs, nouts=1, special=0):
        ins = [self.const(x) if isinstance(x, (int, float)) else x for x in inputs]
        self.ugens.append((cls, rate, ins, nouts, special))
        return len(self.ugens) - 1

    def o(self, ug, k=0):
        return (ug, k)

    def encode(self):
        b = bytes([len(self.name)]) + self.name.encode()
        b += struct.pack(">i", len(self.consts)) + b"".join(struct.pack(">f", c) for c in self.consts)
        b += struct.pack(">i", len(self.params)) + b"".join(struct.pack(">f", float(d)) for _, d in self.params)
        b += struct.pack(">i", len(self.params))
        for i, (n, _) in enumerate(self.params):
            b += bytes([len(n)]) + n.encode() + struct.pack(">i", i)
        b += struct.pack(">i", len(self.ugens))
        for cls, rate, ins, nouts, special in self.ugens:
            b += bytes([len(cls)]) + cls.encode() + struct.pack(">b", rate)
            b += struct.pack(">iih", len(ins), nouts, special)
            for x in ins:
                if x[0] == "c":
                    b += struct.pack(">ii", -1, self.consts.index(x[1]))
                else:
                    b += struct.pack(">ii", x[0], x[1])
            b += bytes([rate]) * nouts
        b += struct.pack(">h", 0)
        return b


def scgf(*defs):
    return b"SCgf" + struct.pack(">ih", 2, len(defs)) + b"".join(d.encode() for d in defs)


AR, KR, IR = 2, 1, 0
MUL = 2  # BinaryOpUGen special index for '*'


def make_defs():
    defs = []
    # sine: Out.ar(out, SinOsc.ar(freq) * amp)
    d = Def("t_sine", [("out", 0), ("freq", 440), ("amp", 0.2)])
    s = d.u("SinOsc", AR, [d.p("freq"), 0])
    m = d.u("BinaryOpUGen", AR, [(s, 0), d.p("amp")], special=MUL)
    d.u("Out", AR, [d.p("out"), (m, 0)], nouts=0)
    defs.append(d)
    # thru: Out.ar(out, In.ar(in))
    d = Def("t_thru", [("in", 0), ("out", 0)])
    i = d.u("In", AR, [d.p("in")])
    d.u("Out", AR, [d.p("out"), (i, 0)], nouts=0)
    defs.append(d)
    # meter: amp follower on an audio bus -> control buses
    #   kbus   : current amplitude
    #   kbus+1 : running min of the amplitude since the last t_reset (gap detector)
    #   kbus+2 : running max
    d = Def("t_meter", [("in", 0), ("kbus", 0), ("reset", 0)])
    i = d.u("In", AR, [d.p("in")])
    # audio-rate follower (Amplitude.kr would only look at the first sample
    # of each block and alias against the test tone)
    aa = d.u("Amplitude", AR, [(i, 0), 0.0005, 0.05])  # 60 dB release: 50 ms
    loa = d.u("RunningMin", AR, [(aa, 0), d.p("reset")])
    hia = d.u("RunningMax", AR, [(aa, 0), d.p("reset")])
    a = d.u("A2K", KR, [(aa, 0)])
    lo = d.u("A2K", KR, [(loa, 0)])
    hi = d.u("A2K", KR, [(hia, 0)])
    k1 = d.u("BinaryOpUGen", KR, [d.p("kbus"), 1], special=0)  # '+'
    k2 = d.u("BinaryOpUGen", KR, [d.p("kbus"), 2], special=0)
    d.u("Out", KR, [d.p("kbus"), (a, 0)], nouts=0)
    d.u("Out", KR, [(k1, 0), (lo, 0)], nouts=0)
    d.u("Out", KR, [(k2, 0), (hi, 0)], nouts=0)
    defs.append(d)
    # line: frees itself after dur seconds (engine-time probe)
    d = Def("t_line", [("dur", 1.0)])
    d.u("Line", KR, [0, 1, d.p("dur"), 2])
    defs.append(d)
    return scgf(*defs)


# ----------------------------------------------------------------------------
# Server process wrapper


_ALL_SERVERS = []


class Server:
    def __init__(self, binary, port, args, plugins, verbose=False, supernova=False):
        self.port = port
        self.verbose = verbose
        self.log = []
        cmd = [binary, "-u", str(port)]
        cmd += ["-U", plugins] + args
        self.cmd = cmd
        env = dict(os.environ)
        # don't load the user's synthdef dir (it may contain defs needing
        # plugins we don't load)
        env["SC_SYNTHDEF_PATH"] = tempfile.mkdtemp(prefix="pwtest_defs_")
        self._defdir = env["SC_SYNTHDEF_PATH"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                                     text=True, bufsize=1)
        _ALL_SERVERS.append(self.proc)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        import threading
        self._ready = threading.Event()
        self._t = threading.Thread(target=self._reader, daemon=True)
        self._t.start()

    def _reader(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            self.log.append(line)
            if self.verbose:
                print("    | " + line)
            if "server ready" in line or "Supernova ready" in line:
                self._ready.set()

    def wait_ready(self, timeout=10.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self._ready.wait(0.05):
                return True
            if self.proc.poll() is not None:
                return False
        return False

    def send(self, data):
        self.sock.sendto(data, ("127.0.0.1", self.port))

    def recv_until(self, addr, timeout=2.0, match=None):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                data, _ = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            a, args = osc_parse(data)
            if a == addr and (match is None or match(args)):
                return args
        return None

    def sync(self, timeout=3.0):
        self.send(osc_msg("/sync", 4242))
        return self.recv_until("/synced", timeout, lambda a: a and a[0] == 4242) is not None

    def status(self):
        self.send(osc_msg("/status"))
        return self.recv_until("/status.reply", 2.0)

    def load_defs(self):
        self.send(osc_msg("/d_recv", make_defs()))
        return self.recv_until("/done", 3.0) is not None

    def cget(self, *buses):
        self.send(osc_msg("/c_get", *buses))
        r = self.recv_until("/c_set", 2.0)
        if r is None:
            return None
        return [r[i] for i in range(1, len(r), 2)]

    def quit(self, timeout=5.0):
        if self.proc.poll() is None:
            try:
                self.send(osc_msg("/quit"))
            except OSError:
                pass
        try:
            rc = self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            rc = "HANG"
        self._t.join(1.0)
        self.sock.close()
        shutil.rmtree(self._defdir, ignore_errors=True)
        return rc

    def logtext(self):
        return "\n".join(self.log)


BAD_LOG = re.compile(r"terminate called|Segmentation|exception in real time|core dumped|Aborted|"
                     r"unworkable|timed out waiting", re.I)


# ----------------------------------------------------------------------------
# PipeWire helpers


class NullSink:
    """Temporary pactl null sink with AUX channel positions."""

    def __init__(self, name, channels):
        self.name = name
        cmap = ",".join(f"aux{i}" for i in range(channels))
        out = subprocess.check_output(
            ["pactl", "load-module", "module-null-sink", f"sink_name={name}", f"channels={channels}",
             f"channel_map={cmap}", f"sink_properties=device.description={name}"], text=True)
        self.module = out.strip()
        self.channels = channels
        time.sleep(0.3)

    def close(self):
        subprocess.call(["pactl", "unload-module", self.module])


class VirtualSource:
    """pw-loopback pair: audio played into `<name>_sink` comes out of the
    Audio/Source node `<name>`, which a capture stream can target directly
    (capture streams that target a plain sink fall back to the default
    source unless stream.capture.sink is set)."""

    def __init__(self, name, channels):
        self.name = name
        self.sink = name + "_sink"
        cmap = "[" + " ".join(f"AUX{i}" for i in range(channels)) + "]"
        self.proc = subprocess.Popen(
            ["pw-loopback", "-n", name + "_lb", "--channels", str(channels), "--channel-map", cmap,
             f"--capture-props=media.class=Audio/Sink node.name={self.sink} node.description={self.sink}",
             f"--playback-props=media.class=Audio/Source node.name={name} node.description={name}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.8)

    def close(self):
        self.proc.terminate()
        self.proc.wait(3)


def pw_nodes():
    d = json.loads(subprocess.check_output(["pw-dump"], text=True))
    nodes, ports, links = {}, {}, []
    for o in d:
        t = o.get("type", "")
        info = o.get("info") or {}
        props = info.get("props") or {}
        if t.endswith(":Node"):
            nodes[o["id"]] = props
        elif t.endswith(":Port"):
            ports[o["id"]] = props
        elif t.endswith(":Link"):
            links.append(info)
    return nodes, ports, links


def nodes_named(prefix):
    nodes, ports, links = pw_nodes()
    out = {}
    for nid, p in nodes.items():
        nm = p.get("node.name", "")
        if nm.startswith(prefix):
            nports = [pid for pid, pp in ports.items()  # real I/O ports, not monitor_*
                      if pp.get("node.id") == nid and not pp.get("port.monitor")]
            nlinks = [l for l in links if l.get("output-node-id") == nid or l.get("input-node-id") == nid]
            out[nm] = {"props": p, "ports": len(nports), "links": len(nlinks)}
    return out


def write_tone_wav(path, freqs, seconds, rate=48000, amp=0.3):
    t = np.arange(int(seconds * rate)) / rate
    data = np.stack([amp * np.sin(2 * np.pi * f * t) for f in freqs], axis=1)
    pcm = (np.clip(data, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(len(freqs))
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def record_sink(sink, channels, seconds, path, rate=48000):
    """Record a sink's monitor for `seconds` (raw f32; libsndfile WAV refuses
    >2 AUX channels). Returns (rate, float array [n, ch])."""
    p = subprocess.Popen(["pw-record", "--raw", "--target", sink, "-P", "{ stream.capture.sink=true }",
                          "--channels", str(channels),
                          "--channel-map", ",".join(f"AUX{i}" for i in range(channels)),
                          "--rate", str(rate), "--format", "f32", path],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(seconds)
    p.terminate()
    _, err = p.communicate(timeout=3)
    a = np.fromfile(path, dtype="<f4").astype(np.float64)
    if a.size < channels * rate // 4:
        raise RuntimeError(f"pw-record captured {a.size} samples: {err.strip()}")
    a = a[: a.size // channels * channels].reshape(-1, channels)
    return rate, a


def analyze_channel(x, sr, skip=0.3):
    """Return (dominant freq, rms, worst gap ratio) for one channel."""
    x = x[int(skip * sr):]
    if len(x) < sr // 4:
        return 0.0, 0.0, 0.0, -1
    w = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * w))
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    # parabolic interpolation around the peak
    k = int(np.argmax(spec[1:]) + 1)
    if 1 <= k < len(spec) - 1:
        a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
        k = k + 0.5 * (a - c) / (a - 2 * b + c)
    freq = k * sr / len(x)
    rms = float(np.sqrt(np.mean(x ** 2)))
    # gap detector: RMS over 2 ms windows; a dropout/torn block shows as a
    # window far below the overall RMS
    win = max(1, int(0.002 * sr))
    nwin = len(x) // win
    wr = np.sqrt(np.mean(x[: nwin * win].reshape(nwin, win) ** 2, axis=1))
    gap = float(wr.min() / rms) if rms > 0 else 0.0
    # discontinuity detector: the 2nd difference of a clean sine is small and
    # bounded by (2*pi*f/sr)^2 * amp; a splice/torn block spikes it
    d2 = np.abs(np.diff(x, 2))
    bound = (2 * np.pi * freq / sr) ** 2 * (rms * math.sqrt(2)) + 1e-4
    clicks = int(np.sum(d2 > 3 * bound))
    return freq, rms, gap, clicks


# ----------------------------------------------------------------------------
# Test runner


class Ctx:
    def __init__(self, args):
        self.build = os.path.abspath(args.build)
        self.scsynth = os.path.join(self.build, "server/scsynth/scsynth")
        self.supernova = os.path.join(self.build, "server/supernova/supernova")
        self.plugins = os.path.join(self.build, "server/plugins")
        self.verbose = args.verbose
        self.stress = args.stress
        self.load = args.load
        self.port = 57190
        self.tmp = tempfile.mkdtemp(prefix="pwtest_")
        self.results = []

    def next_port(self):
        self.port += 1
        if self.port > 57240:
            self.port = 57191
        return self.port

    def server(self, args, supernova=False):
        b = self.supernova if supernova else self.scsynth
        return Server(b, self.next_port(), args, self.plugins, self.verbose, supernova)

    def check(self, name, ok, detail=""):
        self.results.append((name, ok, detail))
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))
        sys.stdout.flush()
        return ok


def engine_time_check(srv, dur=1.0):
    """Start a self-freeing Line and measure wall time until /n_end."""
    srv.send(osc_msg("/notify", 1))
    srv.recv_until("/done", 2.0)
    nid = 5000 + int(time.time() * 10) % 1000
    t0 = time.time()
    srv.send(osc_msg("/s_new", "t_line", nid, 0, 0, "dur", float(dur)))
    r = srv.recv_until("/n_end", dur + 3.0, lambda a: a and a[0] == nid)
    if r is None:
        return None
    return time.time() - t0


def boot_and_probe(ctx, label, args, supernova=False, timecheck=True):
    srv = ctx.server(args, supernova)
    ok = srv.wait_ready(10)
    detail = ""
    if ok:
        st = srv.status()
        ok = st is not None
        if not ok:
            detail = "no /status.reply"
        elif timecheck:
            ok = srv.load_defs()
            if not ok:
                detail = "/d_recv failed"
            else:
                el = engine_time_check(srv, 1.0)
                if el is None:
                    ok, detail = False, "engine not advancing (no /n_end)"
                elif not (0.8 < el < 1.4):
                    ok, detail = False, f"1 s Line took {el:.2f} s"
                else:
                    detail = f"sr={st[7]:.0f} line={el:.2f}s"
    else:
        detail = "did not boot: " + " | ".join(srv.log[-3:])
    rc = srv.quit()
    log = srv.logtext()
    if rc != 0 and ok:
        ok, detail = False, f"exit status {rc}"
    m = BAD_LOG.search(log)
    if m and ok:
        ok, detail = False, f"log: {m.group(0)}"
    ctx.check(label, ok, detail)
    if not ok and not ctx.verbose:
        print("      " + "\n      ".join(srv.log[-12:]))
    return ok


IO_MATRIX = [(2, 2), (0, 2), (2, 0), (0, 0), (1, 1), (8, 8), (0, 16), (16, 0), (32, 32)]


def t_boot_matrix(ctx):
    for i, o in IO_MATRIX:
        boot_and_probe(ctx, f"scsynth -i {i} -o {o}", ["-H", "pwtest", "-i", str(i), "-o", str(o)])
    # non-default block size / hardware buffer
    boot_and_probe(ctx, "scsynth -i 2 -o 2 -z 32", ["-H", "pwtest", "-i", "2", "-o", "2", "-z", "32"])
    boot_and_probe(ctx, "scsynth -i 0 -o 0 -Z 256", ["-H", "pwtest", "-i", "0", "-o", "0", "-Z", "256"])
    boot_and_probe(ctx, "scsynth -i 2 -o 0 -Z 512", ["-H", "pwtest", "-i", "2", "-o", "0", "-Z", "512"])


def t_graph_nodes(ctx):
    cases = [
        (2, 2, {"pwtnodes playback": 2, "pwtnodes capture": 2}),
        (0, 2, {"pwtnodes playback": 2}),
        (2, 0, {"pwtnodes capture": 2}),
        (0, 0, {"pwtnodes playback": 1}),
    ]
    for i, o, expect in cases:
        srv = ctx.server(["-H", "pwtnodes", "-i", str(i), "-o", str(o)])
        if not srv.wait_ready(10):
            ctx.check(f"graph -i {i} -o {o}", False, "did not boot")
            srv.quit()
            continue
        time.sleep(0.5)
        found = nodes_named("pwtnodes")
        got = {k: v["ports"] for k, v in found.items()}
        ok = got == expect
        detail = f"nodes={got}"
        if ok and i == 0 and o == 0:
            n = found["pwtnodes playback"]
            if n["links"] != 0:
                ok, detail = False, f"clock-only node has {n['links']} link(s)"
            else:
                detail += " (clock node unlinked)"
        srv.quit()
        ctx.check(f"graph -i {i} -o {o}", ok, detail if ok else f"expected {expect}, {detail}")


def t_output_signal(ctx):
    sink = NullSink("pwtest_out4", 4)
    try:
        srv = ctx.server(["-H", ":pwtest_out4", "-i", "0", "-o", "4"])
        if not srv.wait_ready(10):
            ctx.check("output 4ch", False, "did not boot")
            srv.quit()
            return
        srv.load_defs()
        freqs = [300.0, 500.0, 700.0, 1100.0]
        for k, f in enumerate(freqs):
            srv.send(osc_msg("/s_new", "t_sine", 1000 + k, 0, 0, "out", k, "freq", f, "amp", 0.25))
        srv.sync()
        time.sleep(0.5)
        sr, a = record_sink("pwtest_out4", 4, 2.5, os.path.join(ctx.tmp, "out4.raw"))
        srv.quit()
        for k, f in enumerate(freqs):
            fr, rms, gap, clicks = analyze_channel(a[:, k], sr)
            ok = abs(fr - f) < 2.0 and abs(rms - 0.25 / math.sqrt(2)) < 0.02 and gap > 0.5 and clicks == 0
            ctx.check(f"output ch{k} {f:.0f} Hz", ok,
                      f"freq={fr:.1f} rms={rms:.3f} (exp {0.25 / math.sqrt(2):.3f}) gap={gap:.2f} clicks={clicks}")
    finally:
        sink.close()


def t_load(ctx):
    """Heavy DSP load on ch0, a clean reference sine on ch1: the reference must
    stay click/gap-free and no xruns may be reported."""
    sink = NullSink("pwtest_load", 2)
    try:
        srv = ctx.server(["-H", ":pwtest_load", "-i", "2", "-o", "2", "-m", "65536", "-n", "16384"])
        if not srv.wait_ready(10):
            ctx.check("load", False, "did not boot")
            srv.quit()
            return
        srv.load_defs()
        rng = np.random.default_rng(1)
        n = ctx.load
        for k in range(n):
            srv.send(osc_msg("/s_new", "t_sine", 2000 + k, 0, 0, "out", 0,
                             "freq", float(rng.uniform(100, 4000)), "amp", 0.5 / n))
            if k % 100 == 99:
                srv.sync()
        srv.send(osc_msg("/s_new", "t_sine", 1999, 0, 0, "out", 1, "freq", 1000.0, "amp", 0.25))
        srv.sync()
        time.sleep(0.5)
        st0 = srv.status()
        sr, a = record_sink("pwtest_load", 2, 3.0, os.path.join(ctx.tmp, "load.raw"))
        st = srv.status()
        srv.quit()
        xruns = sum(1 for l in srv.log if "xrun" in l)
        fr, rms, gap, clicks = analyze_channel(a[:, 1], sr)
        ok = st is not None and st[2] >= n and abs(fr - 1000) < 2 and gap > 0.5 and clicks == 0 and xruns == 0
        ctx.check(f"load {n} synths", ok,
                  f"avgCPU={st[5] if st else -1:.1f}% peak={st[6] if st else -1:.1f}% synths={st[2] if st else -1} "
                  f"ref freq={fr:.1f} gap={gap:.2f} clicks={clicks} xruns={xruns}")
    finally:
        sink.close()


def t_duplex_signal(ctx):
    src = VirtualSource("pwtest_src", 2)
    dst = NullSink("pwtest_dst", 2)
    play = None
    try:
        wav = os.path.join(ctx.tmp, "tone.wav")
        write_tone_wav(wav, [440.0, 660.0], 12.0)
        play = subprocess.Popen(["pw-play", "--target", src.sink, wav], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        srv = ctx.server(["-H", "pwtduplex:pwtest_src", "pwtduplex:pwtest_dst", "-i", "2", "-o", "2"])
        if not srv.wait_ready(10):
            ctx.check("duplex", False, "did not boot")
            srv.quit()
            return
        srv.load_defs()
        # input buses start at numOutputs (2)
        for k in range(2):
            srv.send(osc_msg("/s_new", "t_thru", 1100 + k, 0, 0, "in", 2 + k, "out", k))
            srv.send(osc_msg("/s_new", "t_meter", 1200 + k, 1, 0, "in", 2 + k, "kbus", 10 * k))
        srv.sync()
        time.sleep(1.0)
        for k in range(2):
            srv.send(osc_msg("/n_set", 1200 + k, "reset", 1.0))
        srv.sync()
        time.sleep(0.05)
        for k in range(2):
            srv.send(osc_msg("/n_set", 1200 + k, "reset", 0.0))
        sr, a = record_sink("pwtest_dst", 2, 3.0, os.path.join(ctx.tmp, "duplex.raw"))
        meters = [srv.cget(10 * k, 10 * k + 1, 10 * k + 2) for k in range(2)]
        srv.quit()
        for k, f in enumerate([440.0, 660.0]):
            fr, rms, gap, clicks = analyze_channel(a[:, k], sr)
            ok = abs(fr - f) < 2.0 and abs(rms - 0.3 / math.sqrt(2)) < 0.03 and gap > 0.5 and clicks == 0
            ctx.check(f"duplex in{k}->out{k} {f:.0f} Hz", ok,
                      f"freq={fr:.1f} rms={rms:.3f} gap={gap:.2f} clicks={clicks}")
            m = meters[k]
            if m is None:
                ctx.check(f"duplex meter in{k}", False, "no /c_set")
            else:
                cur, lo, hi = m
                ok = 0.25 < cur < 0.33 and lo > 0.25 and hi < 0.33
                ctx.check(f"duplex meter in{k}", ok, f"amp={cur:.3f} min={lo:.3f} max={hi:.3f}")
    finally:
        if play:
            play.terminate()
            play.wait()
        src.close()
        dst.close()


def t_input_only(ctx):
    src = VirtualSource("pwtest_src1", 2)
    play = None
    try:
        wav = os.path.join(ctx.tmp, "tone1.wav")
        write_tone_wav(wav, [440.0, 660.0], 10.0)
        play = subprocess.Popen(["pw-play", "--target", src.sink, wav], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        for zz in ([], ["-Z", "256"]):
            label = "input-only -i 2 -o 0" + (" " + " ".join(zz) if zz else "")
            srv = ctx.server(["-H", "pwtin:pwtest_src1", "-i", "2", "-o", "0"] + zz)
            if not srv.wait_ready(10):
                ctx.check(label, False, "did not boot: " + " | ".join(srv.log[-3:]))
                srv.quit()
                continue
            srv.load_defs()
            for k in range(2):  # 0 outputs -> input buses start at 0
                srv.send(osc_msg("/s_new", "t_meter", 1200 + k, 0, 0, "in", k, "kbus", 10 * k))
            srv.sync()
            time.sleep(1.0)
            for k in range(2):
                srv.send(osc_msg("/n_set", 1200 + k, "reset", 1.0))
            srv.sync()
            time.sleep(0.05)
            for k in range(2):
                srv.send(osc_msg("/n_set", 1200 + k, "reset", 0.0))
            time.sleep(3.0)
            meters = [srv.cget(10 * k, 10 * k + 1, 10 * k + 2) for k in range(2)]
            rc = srv.quit()
            for k in range(2):
                m = meters[k]
                if m is None:
                    ctx.check(f"{label} ch{k}", False, "no /c_set")
                    continue
                cur, lo, hi = m
                ok = 0.25 < cur < 0.33 and lo > 0.25 and hi < 0.33 and rc == 0
                ctx.check(f"{label} ch{k}", ok, f"amp={cur:.3f} min={lo:.3f} max={hi:.3f} rc={rc}")
    finally:
        if play:
            play.terminate()
            play.wait()
        src.close()


def t_sched_timing(ctx):
    """Schedule /s_new bundles in the future and check they fire on time."""
    for i, o in [(2, 2), (2, 0), (0, 0)]:
        srv = ctx.server(["-H", "pwtsched", "-i", str(i), "-o", str(o)])
        if not srv.wait_ready(10):
            ctx.check(f"sched -i {i} -o {o}", False, "did not boot")
            srv.quit()
            continue
        srv.load_defs()
        srv.send(osc_msg("/notify", 1))
        srv.recv_until("/done", 2.0)
        errs = []
        for n in range(5):
            when = time.time() + 0.3
            nid = 7000 + n
            srv.send(osc_bundle(osc_timetag(when), osc_msg("/s_new", "t_line", nid, 0, 0, "dur", 0.01)))
            r = srv.recv_until("/n_go", 2.0, lambda a, nid=nid: a and a[0] == nid)
            if r is None:
                errs.append(None)
            else:
                errs.append(time.time() - when)
        srv.quit()
        if None in errs:
            ctx.check(f"sched -i {i} -o {o}", False, f"missing /n_go: {errs}")
        else:
            # /n_go is sent from the NRT side after the event runs; allow one
            # hardware buffer (~21 ms at 1024/48k, more at 96k/rate) + jitter
            # Events run early by the reported output latency (so they *sound*
            # on time, as with SC_Jack); /n_go is sent after the block runs.
            # So check: within [-100, +60] ms, and consistent (jitter).
            spread = max(errs[1:]) - min(errs[1:])
            ok = all(-0.1 < e < 0.06 for e in errs) and spread < 0.03
            ctx.check(f"sched -i {i} -o {o}", ok, "offset ms: " + ", ".join(f"{e * 1e3:.1f}" for e in errs)
                      + f" (spread {spread * 1e3:.1f})")


def t_stress_reboot(ctx):
    configs = [(2, 2), (0, 2), (2, 0), (0, 0)]
    fails = []
    t0 = time.time()
    for n in range(ctx.stress):
        i, o = configs[n % len(configs)]
        srv = ctx.server(["-H", "pwtstress", "-i", str(i), "-o", str(o)])
        ok = srv.wait_ready(10) and srv.status() is not None
        rc = srv.quit(5)
        if not ok or rc != 0 or BAD_LOG.search(srv.logtext()):
            fails.append((n, i, o, rc, srv.log[-2:]))
    ctx.check(f"stress {ctx.stress} boot/quit cycles", not fails,
              f"{time.time() - t0:.1f}s" + (f", failures: {fails[:3]}" if fails else ""))


def t_supernova(ctx):
    if not os.path.exists(ctx.supernova):
        ctx.check("supernova", False, f"not built: {ctx.supernova}")
        return
    for i, o in [(2, 2), (0, 2), (2, 0), (0, 0)]:
        boot_and_probe(ctx, f"supernova -i {i} -o {o}", ["-i", str(i), "-o", str(o)], supernova=True)
    # Output signal through supernova. Under JACK, -H is the JACK *server*
    # name and supernova autoconnects to the physical outputs, so: boot,
    # unlink it from the hardware *before* making any sound, link it to a
    # null sink instead, then play.
    sink = NullSink("pwtest_sn", 2)
    try:
        srv = ctx.server(["-i", "0", "-o", "2"], supernova=True)
        if not srv.wait_ready(10):
            ctx.check("supernova output", False, "did not boot: " + " | ".join(srv.log[-3:]))
            srv.quit()
            return
        time.sleep(0.3)
        links = pw_link_map()
        outs = sorted(p for p in links if p.startswith("supernova:output"))
        for port in outs:
            for dst in links[port]:
                subprocess.call(["pw-link", "-d", port, dst], stderr=subprocess.DEVNULL)
        for k, port in enumerate(outs[:2]):
            subprocess.call(["pw-link", port, f"pwtest_sn:playback_AUX{k}"], stderr=subprocess.DEVNULL)
        time.sleep(0.2)
        links = pw_link_map()
        routed = {p: links.get(p, []) for p in outs}
        if any(not d or any(not x.startswith("pwtest_sn:") for x in d) for d in routed.values()):
            ctx.check("supernova output", False, f"could not route ports safely: {routed}")
            srv.quit()
            return
        srv.load_defs()
        srv.send(osc_msg("/s_new", "t_sine", 1000, 0, 0, "out", 0, "freq", 500.0, "amp", 0.25))
        srv.send(osc_msg("/s_new", "t_sine", 1001, 0, 0, "out", 1, "freq", 700.0, "amp", 0.25))
        srv.sync()
        time.sleep(0.3)
        sr, a = record_sink("pwtest_sn", 2, 2.0, os.path.join(ctx.tmp, "sn.raw"))
        srv.quit()
        for k, f in enumerate([500.0, 700.0]):
            fr, rms, gap, clicks = analyze_channel(a[:, k], sr)
            ok = abs(fr - f) < 2.0 and abs(rms - 0.25 / math.sqrt(2)) < 0.02 and gap > 0.5 and clicks == 0
            ctx.check(f"supernova output ch{k} {f:.0f} Hz", ok,
                      f"freq={fr:.1f} rms={rms:.3f} gap={gap:.2f} clicks={clicks}")
    finally:
        sink.close()


def pw_link_map():
    """{output port: [linked input ports]} from `pw-link -l -o`."""
    out, cur = {}, None
    for line in subprocess.check_output(["pw-link", "-l", "-o"], text=True).split("\n"):
        if not line.strip():
            continue
        if line.startswith(" "):
            if cur and "|->" in line:
                out[cur].append(line.split("|->", 1)[1].strip())
        else:
            cur = line.strip()
            out.setdefault(cur, [])
    return out


TESTS = [
    ("boot_matrix", t_boot_matrix),
    ("graph_nodes", t_graph_nodes),
    ("output_signal", t_output_signal),
    ("load", t_load),
    ("duplex_signal", t_duplex_signal),
    ("input_only", t_input_only),
    ("sched_timing", t_sched_timing),
    ("stress_reboot", t_stress_reboot),
    ("supernova", t_supernova),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(REPO, "build"))
    ap.add_argument("--only", default="")
    ap.add_argument("--stress", type=int, default=20)
    ap.add_argument("--load", type=int, default=800, help="synth count for the load test")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    ctx = Ctx(args)
    if not os.path.exists(ctx.scsynth):
        sys.exit(f"no scsynth at {ctx.scsynth}")
    default_sink = subprocess.check_output(["pactl", "get-default-sink"], text=True).strip()
    try:
        for name, fn in TESTS:
            if args.only and not re.search(args.only, name):
                continue
            print(f"[{name}]")
            fn(ctx)
    finally:
        for p in _ALL_SERVERS:  # never leave a test server behind
            if p.poll() is None:
                p.kill()
        # null sinks must never become the default
        if subprocess.check_output(["pactl", "get-default-sink"], text=True).strip() != default_sink:
            subprocess.call(["pactl", "set-default-sink", default_sink])
        shutil.rmtree(ctx.tmp, ignore_errors=True)
    fails = [r for r in ctx.results if not r[1]]
    print(f"\n{len(ctx.results) - len(fails)}/{len(ctx.results)} passed")
    for n, _, d in fails:
        print(f"  FAILED: {n}  {d}")
    sys.exit(len(fails))


if __name__ == "__main__":
    main()
