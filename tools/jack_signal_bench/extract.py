#!/usr/bin/env python3
"""Extract per-run stats from benchmark text output into a single table."""
import re
import sys
from pathlib import Path

def pull(text, pattern):
    m = re.search(pattern, text)
    return m.group(1) if m else None

def parse(path: Path):
    t = path.read_text()
    # Restrict dur stats to the "[dur]" section, gap stats to "[gap]".
    dur_m = re.search(r'=== .*? \[dur\] ===(.+?)(?:===|$)', t, re.S)
    if not dur_m:
        return None
    dur = dur_m.group(1)
    r = {
        'file':       path.stem,
        'callbacks':  pull(t, r'callbacks=(\d+)'),
        'misses':     pull(t, r'deadline_misses=(\d+)'),
        'iters':      pull(t, r'load_iters=(\d+)'),
        'mean_us':    pull(dur, r'mean:\s+([0-9.]+)\s+us'),
        'stddev_us':  pull(dur, r'stddev:\s+([0-9.]+)\s+us'),
        'p50_us':     pull(dur, r'p50:\s+([0-9.]+)\s+us'),
        'p99_us':     pull(dur, r'p99:\s+([0-9.]+)\s+us'),
        'p999_us':    pull(dur, r'p99\.9:\s+([0-9.]+)\s+us'),
        'max_us':     pull(dur, r'max:\s+([0-9.]+)\s+us'),
        'load_pct':   pull(dur, r'cpu_load ~\s+([0-9.]+)\s+%'),
    }
    return r

def main():
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('results/iters')
    rows = []
    for p in sorted(d.glob('*.txt')):
        r = parse(p)
        if r: rows.append(r)
    # Order: path, mode, iters
    def key(r):
        f = r['file']
        path = 0 if f.startswith('shim') else 1
        mode = 0 if '_cv_' in f else 1
        iters = int(r['iters']) if r['iters'] else 0
        return (path, iters, mode)
    rows.sort(key=key)
    hdr = f"{'file':<22} {'iters':>8} {'load%':>7} {'mean':>9} {'stddev':>8} {'p50':>9} {'p99':>9} {'p99.9':>9} {'max':>9} {'miss/cb':>12}"
    print(hdr)
    print('-' * len(hdr))
    for r in rows:
        miss_ratio = f"{r['misses']}/{r['callbacks']}"
        print(f"{r['file']:<22} {r['iters']:>8} {r['load_pct']:>7}% {r['mean_us']:>9} {r['stddev_us']:>8} {r['p50_us']:>9} {r['p99_us']:>9} {r['p999_us']:>9} {r['max_us']:>9} {miss_ratio:>12}")

if __name__ == '__main__':
    main()
