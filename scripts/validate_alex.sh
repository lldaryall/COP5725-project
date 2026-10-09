#!/usr/bin/env bash
# Validates this ALEX against the authors' reference implementation: both are
# bulk loaded with the same keys and parameters, and the resulting structure
# (model nodes, data nodes, data bytes) must match exactly. Throughput is
# reported for information (median of 3 runs). Needs
# scripts/fetch_alex_reference.sh first.
set -euo pipefail
cd "$(dirname "$0")/.."
make -s reference
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

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
      for idx in alex alexref; do
        for rep in 1 2 3; do
          ./build/bench_ref --index "$idx" --dataset "$ds" --n "$n" --init "$n" --ops 1000000 \
            --mix readonly --alex-max-node-bytes "$nb" --seed "$rep" \
            --label "$(basename "$ds")|$n|$nb|$rep" --csv "$OUT" 2>/dev/null
        done
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
    ds, n, nb, rep = r["label"].split("|")
    c = dict(kv.split("=") for kv in r["counters"].split(";") if kv)
    runs[(ds, n, nb, rep)][r["index"]] = (int(c["model_nodes"]), int(c["data_nodes"]), int(r["data_bytes"]))
    mops[(ds, n, nb, r["index"])].append(float(r["throughput_mops"]))
fails = 0
print(f"{'dataset':22}{'n':>9}{'node B':>10}  {'model/data nodes (ours | ref)':32}{'Mops ours':>10}{'ref':>7}  ok")
for key in sorted({k[:3] for k in runs}, key=lambda k: (k[0], int(k[1]), -int(k[2]))):
    ok = True
    for rep in "123":
        ours, ref = runs[key + (rep,)]["alex"], runs[key + (rep,)]["alexref"]
        # The reference counts the empty root data node its constructor makes.
        ok &= ours == (ref[0], ref[1] - 1, ref[2])
    ours, ref = runs[key + ("1",)]["alex"], runs[key + ("1",)]["alexref"]
    fails += not ok
    a = statistics.median(mops[key + ("alex",)])
    b = statistics.median(mops[key + ("alexref",)])
    print(f"{key[0]:22}{key[1]:>9}{key[2]:>10}  {f'{ours[0]}/{ours[1]} | {ref[0]}/{ref[1]-1}':32}{a:10.2f}{b:7.2f}  {'yes' if ok else 'NO'}")
print("\nstructure matches the reference in all cases" if not fails else f"\n{fails} case(s) differ")
sys.exit(1 if fails else 0)
PY
