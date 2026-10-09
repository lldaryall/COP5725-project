# Reproducing ALEX: An Updatable Adaptive Learned Index

[![CI](https://github.com/lldaryall/COP5725-project/actions/workflows/ci.yml/badge.svg)](https://github.com/lldaryall/COP5725-project/actions/workflows/ci.yml)

COP 5725 implementation-flavor project. Darya Pylypenko.

Reproduces Ding et al., *ALEX: An Updatable Adaptive Learned Index* (SIGMOD 2020),
and extends it by measuring where ALEX's cost-model adaptation stops keeping up
when the insert distribution shifts at a controlled rate: hot-range inserts,
gradual drift, and abrupt regime change.

## Status

| Milestone (from proposal) | Status |
|---|---|
| Wk 1: literature review (RMI, ALEX, PGM, FITing-Tree, LIPP, SOSD, robustness studies) | done, see Literature Survey |
| Wk 2: dataset loading, drift workload generators, perf counters, std::map and B+ tree baseline runs | done |
| Wk 3–4: two-stage RMI baseline; ALEX bulk load and lookup | done; ALEX validated against the authors' code |
| Wk 5–6: midpoint, read-path results vs. the paper | done: [laptop midpoint](#midpoint-results-read-only), [full scale on real data](#full-scale-results-real-datasets-200m-keys) |
| Wk 7–8: Gapped Array inserts, node expansion and splits, cost models | next (cost model already in place) |
| Wk 9–10: hot-range, drift, and regime runs with adaptation tracking; ablation with adaptation off | |

## Layout

```
include/datasets.h    SOSD loader + synthetic keys (uniform, lognormal, normal, sequential)
include/workloads.h   pre-generated op streams: ALEX-paper mixes + hotrange/drift/regime
include/indexes.h     common index interface: std::map, tlx B+ tree, RMI, ALEX
include/alex.h        ALEX: Gapped Array data nodes, model nodes, cost model, fanout tree
include/rmi.h         two-stage linear RMI (read-only "Learned Index" baseline)
include/linear_model.h  linear models shared by RMI and ALEX
include/measure.h     timing, latency percentiles, Linux perf_event counters
src/bench.cpp         benchmark driver (CSV output)
src/alex_reference.h  wrapper for the authors' ALEX, used only for validation
tests/test_main.cpp   unit tests (workload invariants, indexes vs. an oracle, ALEX invariants)
scripts/              SOSD download, experiment runners, validation, analysis
docker/Dockerfile     Linux build/benchmark environment (gcc + clang)
.github/workflows/    CI on every push; full-scale benchmarks on demand
third_party/tlx/      vendored tlx B+ tree (Boost license)
```

## Build and test

```
make            # or: cmake -B build && cmake --build build
make test
make check      # tests under AddressSanitizer + UndefinedBehaviorSanitizer
```

C++17, no dependencies beyond the vendored tlx headers. Use `make NATIVE=1` for
CPU-tuned runs, and `CXX=` / `CXXFLAGS=` to change the compiler or flags.

**Linux via Docker.** `scripts/docker_run.sh` builds `docker/Dockerfile`
(Ubuntu 24.04) and runs the full check inside it: gcc and clang builds with the
tests, both sanitizer runs, the CMake build, a smoke run of every index and
pattern, and a perf-counter probe.

**CI.** Every push runs on GitHub Actions:
- Linux builds with gcc and clang (warnings as errors), the tests, and ASan + UBSan
- the CMake build and a macOS arm64 build
- validation against the authors' ALEX, built natively on x86

## Running

```
./build/bench --index btree --dataset synthetic:lognormal --n 3000000 \
    --init 1000000 --ops 2000000 --mix writeheavy --csv results/runs.csv
./build/bench --help                         # all options

scripts/download_sosd.sh [books fb osm wiki] # real datasets (needs curl + zstd)
scripts/run_baselines.sh                     # index x dataset x mix grid
scripts/run_read_path.sh                     # midpoint read-only comparison
python3 scripts/analyze_read_path.py results/read_path.csv
scripts/run_drift.sh                         # extension sweep, throughput over time
python3 scripts/summarize.py results/baselines.csv
```

`INIT=50000000 OPS=50000000 scripts/run_baselines.sh` approaches the paper's
scale and needs a few GB of RAM per run. `--init all` bulk loads every unique key
of a dataset.

### Real datasets

`scripts/download_sosd.sh` fetches the four SOSD datasets (Marcus et al., VLDB
'20) from the SOSD Harvard Dataverse (doi:10.7910/DVN/JGVF9A). Each is a
200M-key uint64 file of 1.6 GB, checked against the Dataverse MD5 before it is
decompressed:

| Name | File | Contents |
|---|---|---|
| books | `books_200M_uint64` | Amazon book popularity |
| fb | `fb_200M_uint64` | Facebook user IDs |
| osm | `osm_cellids_200M_uint64` | OpenStreetMap cell IDs |
| wiki | `wiki_ts_200M_uint64` | Wikipedia edit timestamps; 90.4M after deduplication |

Every experiment script picks up whatever is in `data/`, or takes
`DATASETS="data/<file> ..."`.

### Full-scale runs on GitHub Actions

All four real datasets at full size, plus 200M-key synthetic ones, don't fit
this project's 8 GB development laptop. The **Benchmarks** workflow
(`.github/workflows/benchmarks.yml`) runs them on GitHub's x86 Linux runners
(4 vCPUs, 16 GB), one runner per dataset. Each job:

- downloads and caches the dataset
- enables perf counters
- runs the read-only comparison (ALEX, B+ tree, RMI sweep; 3 seeds)
- validates ALEX against the reference on that dataset

A final job merges the CSVs into one summary.

```
gh workflow run benchmarks.yml               # or the Actions tab -> Benchmarks -> Run
gh run download <run-id> -n results-merged   # read_path_full.csv, machine and validation logs
```

## Methodology

Follows the ALEX paper (Sec. 6.1):

- **Data.** The dataset is deduplicated and shuffled. The first `--init` keys are
  bulk loaded, and the remainder is the pool inserts are drawn from.
- **Mixes.** readonly (0% inserts), readheavy (5%), writeheavy (50%), writeonly (100%).
- **Lookups.** Lookups pick existing keys, Zipfian (θ = 0.99) by default, or
  uniform with `--lookup uniform`. Every lookup is checked for a hit.
- **Workload generation.** All operations are generated before timing starts.
- **Throughput.** Throughput is measured in `--windows` slices, so the series CSV
  shows throughput over the course of the run. That is how thrashing is told
  apart from steady degradation.
- **Latency.** Every 16th operation is timed individually to get p50, p99 and p99.9.
- **Memory.** `index_bytes` is the navigation structure, as the paper defines
  index size: B+ tree inner nodes, map node overhead, RMI models, and ALEX model
  nodes plus data-node metadata. `data_bytes` is key and payload storage: B+ tree
  leaves, the RMI's sorted array, and ALEX slots including gaps and bitmaps. The
  B+ tree and std::map are measured exactly with a counting allocator. RMI and
  ALEX sizes are computed from their allocations, as the reference's
  `model_size()` and `data_size()` do.
- **Hardware counters.** On Linux, cycles, instructions, LLC misses, branch
  misses and L1D misses per op come from `perf_event_open`. Lower
  `/proc/sys/kernel/perf_event_paranoid` to 2 or less if the columns come out empty.
- **Verification.** After each run, every bulk-loaded and inserted key is looked
  up and its payload checked. The process exits non-zero on any error.

### Distribution-shift patterns (extension)

Hot-range, drift and regime patterns are positioned in **quantile space** of the
bulk-loaded keys. That keeps a setting like "drift from q=0.1 at rate r"
comparable across datasets with very different raw key ranges. Quantiles above 1
or below 0 extrapolate past the largest or smallest key.

| Pattern | Controlled variables |
|---|---|
| `hotrange` | `--hot-center`, `--hot-width`; the hot share ramps `--hot-frac-start` → `--hot-frac-end` |
| `drift` | inserts ~ N(centre, `--drift-sigma`); centre = `--drift-from` + `--drift-rate` × (inserts since `--drift-start`) / 1M |
| `regime` | at `--regime-at`, switch to N(`--regime-center`, `--regime-sigma`), or to appends past the max with `--regime-append 1` |

If a target range has no unused integer keys left, the draw falls back to the
dataset pool. The fallback count is reported, so such runs are not mislabelled.

## ALEX implementation and validation

`include/alex.h` implements ALEX's read path and bulk loading as described in the
paper (Sec. 3–4) and the authors' code:

- **Data nodes** are Gapped Arrays at 70% initial density. Keys are placed at their
  model-predicted slot, gaps hold the next key to their right, and lookups run an
  exponential search from the prediction.
- **Model nodes** route keys with a linear model over a power-of-two pointer array.
  A child can own 2^k aligned duplicate pointers.
- **The cost model** estimates exponential-search iterations and shifts per
  insert, using the reference's weights (20, 0.5, 20, 5e-7).
- **Bulk loading** picks each model node's fanout with the bottom-up fanout tree
  and merges cheap siblings.

**Validation against the authors' code.** `scripts/fetch_alex_reference.sh &&
scripts/validate_alex.sh` bulk loads both implementations with identical keys and
parameters. It then checks that the resulting trees have exactly the same number
of model nodes and data nodes and the same data bytes. That holds for all 24
combinations of 4 distributions × 3 sizes (100K–4M) × 2 max node sizes (16 MB,
64 KB), with 3 seeds each.

The reference uses x86 intrinsics, so on Apple Silicon `make reference` builds
for x86_64 and runs under Rosetta. Measured in the same process (to remove
allocation and translation noise), lookup throughput matches the reference within
a few percent on lognormal, normal and uniform keys. On sequential keys with deep
trees it's within about 10–20%. Two lessons from validating:

- **Model-size term.** It must charge the reference's data-node footprint
  (208 bytes, `Params::data_node_cost_bytes`), not our smaller struct (88 bytes).
  Otherwise the cost model builds about 5× more data nodes than ALEX does.
- **Float-to-int clamp.** It runs on every node visit, and clamping in `double`
  first cost 10–15% of throughput on easy datasets. `LinearModel::predict_clamped`
  uses the CPU's saturating conversion, which is well-defined for every input,
  unlike the reference's plain cast.

**Deliberate deviations from the reference**, all on edge cases:

- **Model fitting.** Models are fitted by least squares on keys offset by the
  first key, computed exactly in integers. The reference accumulates raw sums in
  `long double`, which is only a `double` on ARM.
- **Keys that collapse as doubles.** Models see keys as doubles, and near 2^63
  consecutive doubles are 2048 apart. Keys that round to the same double can't be
  routed apart, so such a node becomes one oversized data node. Splitting it would
  recurse forever, which our tests caught.
- **`UINT64_MAX` is reserved.** It's the gap sentinel, so lookups of it return
  "not found" instead of matching a gap.

## Midpoint results (read-only)

`scripts/run_read_path.sh` reproduces the paper's read-only comparison. Each
index is bulk loaded and then serves 10M Zipfian lookups. Every key is verified
after each run, and there were 0 errors across all 168 runs. The RMI is swept
over second-stage model counts and the fastest is reported, as the paper tuned
it. Throughput is the median of 3 seeds (one seed at 50M). The full table is
from `python3 scripts/analyze_read_path.py results/read_path.csv`.

```
dataset                     keys     ALEX  B+tree   RMI*    map   ALEX/B+ ALEX/RMI   B+ idx/ALEX idx RMI idx/ALEX idx  RMI* models
----------------------------------------------------------------------------------------------------------------------------------
synthetic:lognormal      1000000    23.64   12.38  22.83   7.29     1.91x    1.04x              0.4x             2.3x  262144
synthetic:lognormal     10000000    14.81    5.78  11.02   2.91     2.56x    1.34x              2.5x             5.8x  1048576
synthetic:lognormal     50000000     8.29    3.46   7.08      -     2.39x    1.17x              3.1x             6.0x  4194304
synthetic:normal         1000000    31.03   12.70  31.65   7.33     2.44x    0.98x             51.0x            75.5x  65536
synthetic:normal        10000000    19.91    6.25  12.45   2.86     3.19x    1.60x            290.9x           688.9x  1048576
synthetic:normal        50000000    14.43    4.72   7.05      -     3.06x    2.05x            807.5x            95.6x  262144
synthetic:uniform        1000000    32.55   12.45  36.87   6.88     2.61x    0.88x           4429.1x           409.7x  4096
synthetic:uniform       10000000    18.14    6.31  16.05   2.81     2.87x    1.13x           6708.1x           993.0x  65536
synthetic:uniform       50000000    10.38    4.74  11.03      -     2.19x    0.94x           2494.7x          1181.7x  1048576
```

Index size ratios are B+ tree (or RMI) index bytes divided by ALEX index bytes.

How this compares with the paper's claims, as summarized in the literature
survey:

| Paper claim | Here |
|---|---|
| ALEX beats the B+ tree (up to 4.1× across workloads) and never loses | Reproduced. ALEX is 1.9–3.2× faster in all 9 configurations and never slower. |
| ALEX's index is orders of magnitude smaller than a B+ tree's (up to ~2000×) | Reproduced at scale: 2.5× (lognormal) to 6,700× (uniform) smaller at 10M–50M keys. Not at 1M lognormal keys (0.4×), see the caveats. |
| ALEX beats the read-only Learned Index (RMI) by up to 2.2× with a smaller index | Mostly reproduced. 0.88–2.05× (best 2.05× at 50M normal keys). The tuned RMI is 6–12% ahead on uniform keys at 1M and 50M, and level within noise on normal keys at 1M. ALEX's index is 2–1,200× smaller than the fastest RMI's. |

**Caveats:**

- These numbers come from synthetic datasets on a laptop at up to 50M keys,
  versus the paper's 200M real and synthetic keys on a server.
- ALEX's advantages grow with key count because the cost model's size penalty
  scales with the total number of keys. With few keys it favours many small data
  nodes, so at 1M lognormal keys ALEX's index is larger than the B+ tree's.
- SOSD datasets are picked up automatically once downloaded
  (`scripts/download_sosd.sh`). They need more RAM than this 8 GB laptop
  comfortably has at full size.

## Full-scale results (real datasets, 200M keys)

These are the paper-scale runs from the **Benchmarks** workflow on GitHub's x86
Linux runners: all four SOSD datasets with every unique key bulk loaded, plus
200M-key synthetic sets. Each index serves 10M Zipfian lookups, and throughput
is the median of 3 seeds. All 126 runs verified every key with 0 errors, and
peak memory was 7.8 GB per process. Raw data is in `results/full_scale/`, as
`read_path.csv` plus per-job `machine-*.txt` and `validate-*.txt` files.

```
dataset                     keys     ALEX  B+tree   RMI*    map   ALEX/B+ ALEX/RMI   B+ idx/ALEX idx RMI idx/ALEX idx  RMI* models
----------------------------------------------------------------------------------------------------------------------------------
books_200M_uint64      200000000     7.45    1.72   5.99      -     4.34x    1.24x           1441.5x          2731.4x  16777216
fb_200M_uint64         199999999     2.41    1.96   1.76      -     1.23x    1.37x              7.2x             0.2x  262144
osm_cellids_200M_uint64 200000000     2.47    1.63   4.38      -     1.52x    0.56x              4.9x             9.3x  16777216
synthetic:lognormal    200000000     9.27    1.82   7.47      -     5.09x    1.24x             12.6x            23.8x  16777216
synthetic:normal       200000000    10.02    1.76   6.93      -     5.70x    1.45x           1850.8x          3506.9x  16777216
synthetic:uniform      200000000    12.46    2.07   9.81      -     6.01x    1.27x           2523.0x          4780.7x  16777216
wiki_ts_200M_uint64     90437011    11.02    1.77   7.85      -     6.24x    1.40x             51.5x           215.7x  16777216
```

Each dataset ran on one runner, so its indexes are compared on the same CPU.
GitHub assigned different AMD EPYC models across jobs, so absolute Mops/s are
not comparable *between* datasets:

| CPU | Datasets |
|---|---|
| AMD EPYC 7763 | lognormal, normal, osm |
| AMD EPYC 9V45 | fb, uniform |
| AMD EPYC 9V74 | books, wiki |

**Against the paper's claims:**

| Paper claim | Full scale |
|---|---|
| ALEX beats the B+ tree (up to 4.1×) and never loses | Reproduced, all 7 datasets: 1.23× (fb) to 6.24× (wiki). Above 4× on books, wiki and every synthetic set. |
| Index up to ~2000× smaller than a B+ tree's | Reproduced: 1,441× (books), 1,851× (normal), 2,523× (uniform). Only 4.9–51× on the hard real datasets (osm, fb, wiki). |
| Up to 2.2× faster than the Learned Index (RMI), with a smaller index | Mostly reproduced: ALEX is faster on 6 of 7 datasets (1.24–1.45×), and on 5 of those its index is 24–4,800× smaller. **Not on osm**: the tuned RMI is 1.8× faster there, though ALEX's index is still 9× smaller. On fb, ALEX is faster but its index is 5× larger. |

**What the hard datasets show.** osm and fb are the datasets SOSD and the
updatable-index studies flag as hard to learn. On osm, cell IDs have no smooth
local structure. On fb, a few huge outlier IDs make linear models fit the bulk
of the keys badly.

On both, ALEX's margin over the B+ tree shrinks to 1.2–1.5×. It needs thousands
of model nodes (osm: about 6,900 at 10M keys, against 1–31 on books and wiki)
and loses its index-size advantage. That matches Wongkham et al.'s finding,
summarized in the literature survey, that ALEX's throughput drops sharply from
easy to hard data. It also motivates the drift extension: hard regions of the
key space are where adaptation has to work.

**Validation on real data.** In reference-exact mode, ALEX's structure matches
the authors' implementation exactly on all four real datasets (1M and 10M keys,
3 seeds), and lookup throughput is within about 5% of theirs. With our default,
more precise model fit, structure is identical on books and fb. On wiki and osm
it differs by up to 0.1% of nodes: tiny floating-point differences flip
near-ties in the cost model on hard data.

**Not measured here:**
- **Hardware counters.** GitHub's hosted runners are VMs without access to the
  CPU's performance counters, and Docker Desktop on Apple Silicon has none
  either. Those columns stay empty, and they fill in on bare-metal Linux.
- **The B+ tree baseline** uses tlx's default 256-byte nodes, so it is not tuned.

## Notes on the preliminary numbers in `results/`

`baselines.csv` and `drift*.csv` were run at laptop scale (1M bulk-loaded keys,
2M ops) on an Apple M-series Mac, so treat them as smoke tests, not the
reproduction:

- Apple Silicon's clock ticks every ~42 ns, which makes per-op latencies coarse.
- perf counters are Linux-only.

The real runs will be on the Linux machine at the paper's scale.
