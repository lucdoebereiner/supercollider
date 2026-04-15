# Benchmark results

Three tables from this investigation, cleaned up for copy-paste. Raw source
data under `results/`.

Hardware: AMD Ryzen AI 9 365, PipeWire 1.0.5 (libjack shim), Yamaha DM3.
Quantum: 1024 samples @ 48 kHz → 21333 µs period.
All times µs unless noted.

---

## Table 1 — Standalone signaling benchmark, shim vs native, three modes

Same callback body (DC-fill of 2 output channels), only the tail signaling
call differs:

- `none`  = no signal (pure DSP baseline, not scsynth behavior)
- `cv_any` = `condition_variable_any::notify_one()` — **what scsynth does today**
- `sem_t`  = `sem_post()` — **the proposed SC_SyncCondition fix**

60 s/cell, ~2813 callbacks each. Raw: `results/shim_*.txt`, `results/pw_*.txt`.

| metric (µs)     | shim/none | shim/cv_any | shim/sem_t | pw/none | pw/cv_any | pw/sem_t |
|-----------------|----------:|------------:|-----------:|--------:|----------:|---------:|
| period          |    21 333 |      21 333 |     21 333 |  21 007 |    21 019 |   21 050 |
| samples         |     2 813 |       2 813 |      2 813 |   2 813 |     2 812 |    2 812 |
| mean            |      1.08 |       15.39 |      14.54 |    1.04 |      6.54 |     8.67 |
| stddev          |      0.55 |        7.31 |       4.91 |    0.75 |      4.40 |     5.03 |
| p50             |      0.99 |       16.48 |      16.12 |    0.77 |      5.31 |     8.03 |
| p90             |      1.70 |       22.06 |      18.36 |    1.72 |     11.42 |    12.19 |
| p99             |      3.26 |       27.13 |      20.91 |    3.30 |     19.72 |    23.40 |
| p99.9           |      5.93 |       62.58 |      58.27 |    9.02 |     42.77 |    63.70 |
| p99.99          |      6.24 |       63.67 |      58.80 |    9.43 |     57.73 |    74.18 |
| max             |      6.90 |       64.21 |      61.42 |   10.16 |     58.92 |   100.21 |
| spike p99/p50   |     3.28× |       1.65× |      1.30× |   4.27× |     3.71× |    2.92× |
| spike max/mean  |     6.37× |       4.17× |      4.23× |   9.82× |     9.01× |   11.56× |

Scheduling jitter (gap between callback starts):

| metric (µs)  | shim/none | shim/cv | shim/sem | pw/none | pw/cv | pw/sem |
|--------------|----------:|--------:|---------:|--------:|------:|-------:|
| mean         |    21 333 |  21 333 |   21 333 |  21 333 | 21 333 | 21 333 |
| stddev       |      9.83 |  238.48 |   249.24 |  474.86 | 474.82 | 486.79 |
| p99          |    21 363 |  21 947 |   21 941 |  22 055 | 22 119 | 22 236 |
| max          |    21 438 |  22 344 |   22 272 |  22 479 | 22 609 | 22 633 |

---

## Table 2 — Standalone load sweep with fixed iteration counts

Four loads × 2 modes × 2 paths. `--iters N` with explicit counts (not
`--load-us` calibration, which was unreliable across runs due to turbo
boost state variation). 30 s/cell. Raw: `results/iters/`.

| cell                | iters  | load % |  mean  | stddev |   p99  |  p99.9  |   max   | misses |
|---------------------|-------:|-------:|-------:|-------:|-------:|--------:|--------:|-------:|
| shim / cv           | 150000 |  23.7% |  5 057 |    391 |  7 222 |   8 246 |   8 451 |  0/1407 |
| shim / sem          | 150000 |  23.4% |  4 999 |    272 |  5 889 |   7 464 |   8 165 |  0/1406 |
| shim / cv           | 400000 |  45.4% |  9 678 |    629 | 12 733 |  15 422 |  16 189 |  0/1406 |
| shim / sem          | 400000 |  45.8% |  9 768 |    591 | 12 724 |  14 513 |  16 369 |  0/1406 |
| shim / cv           | 650000 |  72.3% | 15 433 |    750 | 20 216 |  21 748 |  24 476 |  3/1406 |
| shim / sem          | 650000 |  71.8% | 15 312 |    845 | 20 537 |  23 102 |  23 994 |  4/1406 |
| shim / cv           | 850000 |  93.7% | 19 983 |  1 029 | 25 639 |  31 643 |  33 797 | 30/1404 |
| shim / sem          | 850000 |  92.5% | 19 735 |  1 120 | 26 853 |  31 183 |  33 764 | 30/1404 |
| pw   / cv           | 150000 |  16.6% |  3 494 |    510 |  5 283 |   5 958 |   7 216 |  0/1407 |
| pw   / sem          | 150000 |  16.4% |  3 458 |    543 |  5 141 |   6 739 |   7 010 |  0/1406 |
| pw   / cv           | 400000 |  31.5% |  6 625 |  1 107 | 12 227 |  13 686 |  13 978 |  0/1406 |
| pw   / sem          | 400000 |  31.0% |  6 532 |    951 | 10 873 |  13 007 |  14 139 |  0/1405 |
| pw   / cv           | 650000 |  48.6% | 10 242 |  1 360 | 16 854 |  20 918 |  21 568 |  0/1403 |
| pw   / sem          | 650000 |  48.5% | 10 207 |  1 322 | 15 394 |  21 085 |  23 162 |  1/1404 |
| pw   / cv           | 850000 |  62.3% | 13 133 |  1 365 | 18 960 |  23 259 |  25 239 |  8/1399 |
| pw   / sem          | 850000 |  61.7% | 13 003 |  1 267 | 18 157 |  24 417 |  26 196 |  5/1401 |

Shim vs native, same iter count, cv_any mode (headline ratio):

| iters  | shim mean | native mean | shim penalty |
|-------:|----------:|------------:|-------------:|
| 150k   |   5 057   |     3 494   |      +45 %   |
| 400k   |   9 678   |     6 625   |      +46 %   |
| 650k   |  15 433   |    10 242   |      +51 %   |
| 850k   |  19 983   |    13 133   |      +52 %   |

---

## Table 3 — Real in-process scsynth sweep

Two scsynth binaries, same tree, only `AUDIOAPI` differs. Identical
wall-clock CPU accounting in both (SC_Jack.cpp locally patched to match
SC_PipeWire.cpp — see STATUS.md). N default-synthdef voices, 6 s/cell,
50 Hz /status polling. `xruns` is the rate-limited xrun tag count (one
per ~second of sustained overrun).

Raw: `results/scsynth_sweep/all.jsonl`.

```
driver    synths   obs  avg_mean   avg_max  peak_mean  peak_max  peak_std     p99   >90%   >100%  xruns
--------------------------------------------------------------------------------------------------------
jack         100   100      7.08      8.48       9.36     11.09      1.21    11.09     0       0      0
pipewire     100   100      6.07      6.85       8.55      9.75      1.10     9.75     0       0      0
jack         500   500     34.42     35.62      38.65     43.84      2.65    43.84     0       0      0
pipewire     500   500     26.12     28.86      33.56     36.18      3.04    36.18     0       0      0
jack        1000  1000     66.65     72.09      74.55     96.47      6.19    96.47     4       0      0
pipewire    1000  1000     37.14     44.64      60.73     88.11     16.89    88.11     0       0      0
jack        2000  2000    124.15    171.35     135.06    189.32     24.46   189.32   229     229      6
pipewire    2000  2000     72.40     82.46     104.44    157.80     27.84   157.80   159     113      1
jack        3000  3000    197.69    243.11     221.93    271.13     30.36   271.13   143     143      4
pipewire    3000  3000    113.73    134.72     147.76    233.16     35.40   209.34   218     218      6
jack        4000  4000    286.12    327.16     335.09    373.31     44.28   373.31    98      98      3
pipewire    4000  4000    181.16    189.59     213.50    247.99     23.66   247.99   147     147      4
jack        5000  5000    351.05    379.51     412.18    459.74     58.28   459.74   290     290      2
pipewire    5000  5000    241.72    257.37     327.02    348.02     32.29   348.02   294     294      3
```

Key per-load comparison:

| synths | JACK avg % | PW avg % | PW savings | JACK peak max | PW peak max | JACK `>100%` | PW `>100%` |
|-------:|-----------:|---------:|-----------:|--------------:|------------:|-------------:|-----------:|
|    100 |       7.1  |     6.1  |     −14 %  |          11.1 |         9.8 |            0 |          0 |
|    500 |      34.4  |    26.1  |     −24 %  |          43.8 |        36.2 |            0 |          0 |
|   1000 |      66.7  |    37.1  |     −44 %  |          96.5 |        88.1 |            0 |          0 |
|   2000 |     124.2  |    72.4  |     −42 %  |         189.3 |       157.8 |      229/229 |    113/229 |
|   3000 |     197.7  |   113.7  |     −42 %  |         271.1 |       233.2 |      143/143 |    218/218 |

First dropout: both backends cross the 100 % line somewhere between 1000
and 2000 synths, but at 2000 synths JACK is 100 % saturated
(229/229 samples over deadline) while PW is 49 % saturated (113/229).
Sustainable voice count: **~1500 (JACK) vs ~2500 (PW)**, i.e. native PW
gives roughly **50 % more headroom** on this hardware.
