#!/usr/bin/env bash
# Reproduction grid for the baselines: every index x dataset x ALEX-paper mix.
# Sizes default to laptop scale; the paper bulk loads 50M+ keys:
#   INIT=50000000 OPS=50000000 scripts/run_baselines.sh
set -euo pipefail
cd "$(dirname "$0")/.."
INIT=${INIT:-1000000}
OPS=${OPS:-2000000}
OUT=${OUT:-results/baselines.csv}
INDEXES=${INDEXES:-"stdmap btree rmi alex"}

# DATASETS overrides the list, e.g. DATASETS="data/fb_200M_uint64".
if [[ -n ${DATASETS:-} ]]; then
  read -r -a datasets <<< "$DATASETS"
else
  datasets=(synthetic:lognormal synthetic:uniform)
  for f in data/*_uint32 data/*_uint64; do [[ -f $f ]] && datasets+=("$f"); done
fi

make -s build/bench
for ds in "${datasets[@]}"; do
  for mix in readonly readheavy writeheavy writeonly; do
    for idx in $INDEXES; do
      # RMI is read-only; ALEX inserts arrive in weeks 7-8.
      if [[ $mix != readonly && ( $idx == rmi || $idx == alex ) ]]; then continue; fi
      ./build/bench --index "$idx" --dataset "$ds" --n $((INIT + OPS)) --init "$INIT" \
        --ops "$OPS" --mix "$mix" --label "$mix" --csv "$OUT"
    done
  done
done
echo "results appended to $OUT"
