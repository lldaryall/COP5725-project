#!/usr/bin/env bash
# Validates this ALEX against the authors' reference implementation: both are
# bulk loaded with the same keys and parameters, and the resulting structure
# (model nodes, data nodes, data bytes) must match exactly.
#
# The exact check runs ours with --alex-ref-fit 1, which fits models with the
# reference's arithmetic. Our default fit (offset keys, more precise) can
# flip a near-tie in the cost model, so its node counts are shown alongside
# for information. Throughput is the median of 3 runs. Needs
# scripts/fetch_alex_reference.sh first.
set -euo pipefail
cd "$(dirname "$0")/.."
make -s reference
OUT=$(mktemp)
LOG=$(mktemp)
trap 'rm -f "$OUT" "$LOG"' EXIT
# Any failed run aborts with its error message (set -e), never silently.
bench_ref() {
  ./build/bench_ref "$@" >> "$LOG" 2>&1 || { echo "FAILED: bench_ref $*" >&2; { grep "^error:" "$LOG" | tail -1 || tail -3 "$LOG"; } >&2; exit 1; }
}

# DATASETS overrides the list, e.g. DATASETS="data/fb_200M_uint64".
if [[ -n ${DATASETS:-} ]]; then
  read -r -a datasets <<< "$DATASETS"
else
  datasets=(synthetic:lognormal synthetic:normal synthetic:uniform synthetic:sequential)
  for f in data/*_uint32 data/*_uint64; do [[ -f $f ]] && datasets+=("$f"); done
fi
SIZES=${SIZES:-"100000 1000000 4000000"}
NODE_BYTES=${NODE_BYTES:-"16777216 65536"}

for ds in "${datasets[@]}"; do
  for n in $SIZES; do
    for nb in $NODE_BYTES; do
      for rep in 1 2 3; do
        run=(--dataset "$ds" --n "$n" --init "$n" --ops 1000000 --mix readonly
             --alex-max-node-bytes "$nb" --seed "$rep" --csv "$OUT")
        bench_ref --index alexref "${run[@]}" --label "$(basename "$ds")|$n|$nb|$rep|ref"
        bench_ref --index alex --alex-ref-fit 1 "${run[@]}" --label "$(basename "$ds")|$n|$nb|$rep|exact"
        bench_ref --index alex "${run[@]}" --label "$(basename "$ds")|$n|$nb|$rep|default"
      done
    done
  done
done

python3 - "$OUT" <<'PY'
import csv, statistics, sys
from collections import defaultdict
runs = defaultdict(dict)
mops = defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    ds, n, nb, rep, mode = r["label"].split("|")
    c = dict(kv.split("=") for kv in r["counters"].split(";") if kv)
    runs[(ds, n, nb, rep)][mode] = (int(c["model_nodes"]), int(c["data_nodes"]), int(r["data_bytes"]))
    mops[(ds, n, nb, mode)].append(float(r["throughput_mops"]))
fails = default_diffs = 0
print(f"{'dataset':24}{'n':>9}{'node B':>10}  {'model/data nodes: ref | exact-fit | default-fit':48}"
      f"{'Mops ref':>9}{'ours':>7}  exact")
for key in sorted({k[:3] for k in runs}, key=lambda k: (k[0], int(k[1]), -int(k[2]))):
    ok = True
    for rep in "123":
        r = runs[key + (rep,)]
        # The reference counts the empty root data node its constructor makes.
        ref = (r["ref"][0], r["ref"][1] - 1, r["ref"][2])
        ok &= r["exact"] == ref
        default_diffs += r["default"] != ref
    r = runs[key + ("1",)]
    ref = (r["ref"][0], r["ref"][1] - 1)
    cell = f"{ref[0]}/{ref[1]} | {r['exact'][0]}/{r['exact'][1]} | {r['default'][0]}/{r['default'][1]}"
    fails += not ok
    a = statistics.median(mops[key + ("ref",)])
    b = statistics.median(mops[key + ("default",)])
    print(f"{key[0]:24}{key[1]:>9}{key[2]:>10}  {cell:48}{a:9.2f}{b:7.2f}  {'yes' if ok else 'NO'}")
total = 3 * len({k[:3] for k in runs})
print(f"\ndefault fit: structure identical to the reference in {total - default_diffs}/{total} runs")
print("exact fit: structure matches the reference in all cases" if not fails else f"exact fit: {fails} case(s) differ")
sys.exit(1 if fails else 0)
PY
