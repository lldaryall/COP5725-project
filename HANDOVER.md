# Handover: moving to the Linux machine

State as of 2026-10-09 (commit after `f276648`). Read this first, then `README.md`
for methodology and results.

## 1. Set up (Ubuntu/Debian)

```bash
sudo apt-get install -y build-essential clang libclang-rt-18-dev cmake zstd curl python3 git \
     linux-tools-common linux-tools-$(uname -r)    # linux-tools only for `perf`, optional
git clone https://github.com/lldaryall/COP5725-project.git && cd COP5725-project
make -j && make test          # build + unit tests
make check                    # tests under ASan + UBSan
```

Everything should pass. CI runs the same checks on every push:
- gcc and clang builds with warnings as errors, plus the tests and ASan/UBSan
- the CMake build and macOS arm64
- validation against the authors' ALEX

See the Actions tab.

## 2. First thing to do on Linux: perf counters

The hardware-counter code (`include/measure.h`, `perf_event_open`) has **never
produced numbers yet**. GitHub's runners and Docker on a Mac don't expose the
CPU's performance counters, so the `cycles_per_op`, `llc_miss_per_op` and
related CSV columns are empty in every result so far. On bare-metal Linux:

```bash
sudo sysctl -w kernel.perf_event_paranoid=1
./build/bench --index alex --dataset synthetic:lognormal --n 1000000 --init 1000000 \
    --ops 2000000 --mix readonly
# expect a "per op ... cycles ... instr ... LLC miss ... br miss" line
```

To sanity-check the numbers, compare with
`perf stat -e cycles,instructions,cache-misses,branch-misses ./build/bench ...`.
The bench only counts the timed loop, so its numbers should be somewhat lower
than `perf stat`'s whole-process totals. The proposal lists perf counters as an
evaluation tool, so this is worth confirming.

## 3. Where things stand

| Milestone | Status |
|---|---|
| Wk 1 literature survey | done (PDF, not in repo) |
| Wk 2 harness, datasets, drift workloads, std::map + B+ tree baselines | done |
| Wk 3–4 RMI baseline; ALEX bulk load + lookup | done, validated against the authors' code |
| Wk 5–6 midpoint read-only results | done: laptop scale and full scale on real data |
| **Wk 7–8 ALEX inserts, expansion, splits, cost-model adaptation** | **next, not started** |
| Wk 9–10 hot-range / drift / regime runs on ALEX; ablation with adaptation off | after that |

**Headline results** (README, "Full-scale results"; raw data in
`results/full_scale/`). These are from 200M-key read-only runs on SOSD books,
fb, osm and wiki plus synthetic sets, on GitHub's x86 runners, with 0 errors in
126 runs:

- **ALEX vs. the B+ tree:** ALEX is faster on all 7 datasets, 1.23–6.24×.
- **ALEX vs. the RMI:** ALEX wins on 6 of 7. osm is the exception, where the RMI
  is 1.8× faster.
- **Index size:** ALEX's index is up to about 2,500× smaller than the B+ tree's.
- **Hard datasets:** on osm and fb, ALEX's advantage mostly disappears.

## 4. Where to find things

| Path | What |
|---|---|
| `include/alex.h` | ALEX: data nodes (Gapped Array), model nodes, cost model, fanout-tree bulk load |
| `include/rmi.h`, `include/linear_model.h` | RMI baseline; linear models + least-squares fits |
| `include/indexes.h` | common index interface + wrappers (`StdMapIndex`, `BTreeIndex`, `RMIIndex`, `AlexIndex`) |
| `include/workloads.h` | workload generator: paper mixes + `hotrange` / `drift` / `regime` insert patterns |
| `include/datasets.h` | SOSD loader, synthetic keys |
| `include/measure.h` | timing, percentiles, perf counters |
| `src/bench.cpp` | benchmark driver (`./build/bench --help`) |
| `src/alex_reference.h` | wrapper for the authors' ALEX (`--index alexref`, `make reference`) |
| `tests/test_main.cpp` | unit tests, including ALEX structural invariants |
| `scripts/run_read_path.sh` + `analyze_read_path.py` | read-only experiment and analysis |
| `scripts/validate_alex.sh` | structure + throughput comparison with the reference |
| `scripts/run_drift.sh` | Wk 9–10 experiment (currently baselines only) |
| `scripts/download_sosd.sh` | real datasets (about 6.4 GB on disk) |
| `.github/workflows/benchmarks.yml` | full-scale runs on GitHub (`gh workflow run benchmarks.yml`) |

## 5. Reproducing on this machine

```bash
scripts/download_sosd.sh                        # books fb osm wiki -> data/
scripts/fetch_alex_reference.sh && make reference   # authors' ALEX, native on x86
scripts/validate_alex.sh                        # must end "exact fit: structure matches..."
make NATIVE=1
SIZES=all DATASETS="data/books_200M_uint64" INDEXES="alex btree" \
  RMI_MODELS="262144 1048576 4194304 16777216" OUT=results/linux_read_path.csv \
  scripts/run_read_path.sh
python3 scripts/analyze_read_path.py results/linux_read_path.csv
```

**Memory.** A 200M-key run peaks at about 7.8 GB, and std::map at 200M needs
more than 12 GB. Write new runs to a **new CSV file**: the bench refuses to
append to a CSV with a different header, on purpose.

Bare-metal Linux numbers with perf counters would be the best final results,
because the GitHub runners had different CPUs per job and no counters.

## 6. Next: Weeks 7–8, ALEX inserts

Mirror the authors' code in `third_party/ALEX/src/core/` (after running
`fetch_alex_reference.sh`). Mapping to our code:

1. **Data node insert** (`AlexDataNode::insert`, `find_insert_position`,
   `closest_gap`, `insert_using_shifts`, `insert_element_at` in `alex_nodes.h`):
   predict the position, find the insert slot, and shift toward the closest gap.
   Keep the gap fill (gaps hold the key to their right) and the bitmap correct.
   `DataNode` in `include/alex.h` already has `bitmap`, `num_keys`, `capacity`
   and the `expected_avg_*` fields.
2. **Runtime cost tracking** on each data node: lookups, exponential-search
   iterations, inserts and shifts, compared against `expected_avg_*`.
   `significant_cost_deviation()` and `catastrophic_cost()` decide when to adapt.
3. **Expansion** when density reaches `kMaxDensity` (0.8). Expand to
   `kMinDensity` (0.6), either scaling the model or retraining it, as the
   reference's `resize` does. The reference counts these as
   `num_expand_and_scales` and `num_expand_and_retrains`.
4. **Splits** (`Alex::insert` in `alex.h`): sideways (into the parent's
   duplicate pointers, or expanding the parent model node) or downward (a new
   model node with 2 children). Use the fanout tree (`find_best_fanout_existing_node`).
5. **Out-of-domain inserts** (keys beyond the root's range): the reference
   expands the root (`expand_root`, superroot). This is exactly what the
   `--regime-append 1` pattern stresses.
6. **Wiring and tests:**
   - Set `AlexIndex::supports_inserts()` to true and expose counters (expansions,
     splits by type, retrains) through `counters()`. That's what Wk 9–10 plots.
   - Validate against `--index alexref`, which already supports inserts and
     reports `expand_and_scales`, `downward_splits` and the rest. Compare
     structure and counters on write-heavy workloads, as `validate_alex.sh`
     does for bulk load.
   - Extend `tests/test_main.cpp`: insert against the `std::map` oracle and
     re-check `check_alex_structure` after every N inserts.

**Weeks 9–10.** `scripts/run_drift.sh` already sweeps hot-range widths, drift
rates and regime changes, and records throughput over time (`--series`).
Remaining work:
- Add `alex` (and `alexref`) to `INDEXES`.
- Add a `Params` flag that disables adaptation, for the ablation.
- Plot throughput windows alongside the adaptation counters to tell thrashing
  apart from graceful degradation.

## 7. Gotchas (all learned the hard way)

- **Keys are seen as doubles by models.** Near 2^63, doubles are 2048 apart.
  Keys that collapse to one double can't be split, so they become one oversized
  data node (`can_split` in `alex.h`).
- **`UINT64_MAX` is reserved** (the gap sentinel). The bench drops it from
  datasets, and SOSD fb contains it once.
- **`Params::data_node_cost_bytes = 208`** is the reference's struct size.
  Using our real 88 bytes makes the cost model build about 5× more data nodes.
- **`--alex-ref-fit 1`** reproduces the reference's model-fit arithmetic and
  makes structure comparisons bit-exact. The default fit is more precise and
  differs by up to 0.1% of nodes on osm and wiki.
- **The reference ALEX needs x86** (POPCNT/LZCNT/BMI). It's native on this
  Linux box; on Apple Silicon it ran under Rosetta.
- **wiki deduplicates to 90.4M keys**; use `--init all` rather than a count.
- **Bench stderr is logged** to `<out>.log` by the scripts. Never send it to
  `/dev/null` again: that once hid an fb failure.
- **Workflows use `shell: bash`** (pipefail). Without it, `cmd | tee` once
  reported a failed job as green.
- **The B+ tree uses tlx's default 256-byte nodes**, untuned. Worth stating, or
  adding a larger-node variant, in the final report.
- **Laptop-era notes in the README** (Rosetta, `caffeinate`, the ~42 ns Apple
  clock) don't apply on Linux.

## 8. Docs and results to keep in sync

- **README:** the status table, the midpoint section, and the full-scale section.
- **`results/`:** `baselines.csv` and `drift*.csv` are laptop smoke tests;
  `read_path.csv` is the laptop midpoint; `full_scale/` holds the GitHub-runner
  results.
- **Course PDFs** (proposal, literature survey) live outside the repo, in the
  course folder on the Mac. Copy them over if you need them.
