#!/usr/bin/env python3
"""Tabulate scsynth_sweep results/scsynth_sweep/all.jsonl into a pretty table."""
import json, sys, pathlib

path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "results/scsynth_sweep/all.jsonl")
rows = [json.loads(l) for l in path.read_text().splitlines() if l.strip()]
rows.sort(key=lambda r: (r.get("synths_requested", 0), r.get("driver", "")))

hdr = (
    f"{'driver':<8} {'synths':>7} {'obs':>5} "
    f"{'avg_mean':>9} {'avg_max':>9} "
    f"{'peak_mean':>10} {'peak_max':>9} {'peak_std':>9} {'p99':>8} "
    f"{'>90%':>6} {'>100%':>7} {'xruns':>6}"
)
print(hdr)
print("-" * len(hdr))
for r in rows:
    if "error" in r:
        print(f"{r.get('driver','?'):<8} {r.get('synths_requested',0):>7}  ERROR: {r['error']}")
        continue
    print(
        f"{r['driver']:<8} {r['synths_requested']:>7} {r['synths_observed']:>5} "
        f"{r['avg_mean']:>9.2f} {r['avg_max']:>9.2f} "
        f"{r['peak_mean']:>10.2f} {r['peak_max']:>9.2f} {r['peak_std']:>9.2f} {r['peak_p99']:>8.2f} "
        f"{r['over_90_pct_count']:>6} {r['over_100_pct_count']:>7} {r.get('xruns', 0):>6}"
    )
