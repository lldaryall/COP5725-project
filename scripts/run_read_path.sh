#!/usr/bin/env bash
# Midpoint experiment (weeks 5-6): read-only comparison of ALEX against the
# B+ tree, the two-stage RMI and std::map, as in the ALEX paper's read-only
# results. The RMI is swept over second-stage model counts and the best is
# reported, since the paper tuned it per dataset. Each configuration runs
# with 3 seeds; scripts/analyze_read_path.py reports medians.
#
#   SIZES="1000000 10000000" scripts/run_read_path.sh
#   SIZES=all DATASETS=data/fb_200M_uint64 scripts/run_read_path.sh   # whole dataset
#   python3 scripts/analyze_read_path.py results/read_path.csv
set -euo pipefail
cd "$(dirname "$0")/.."
SIZES=${SIZES:-"1000000 10000000"}
OPS=${OPS:-10000000}
SEEDS=${SEEDS:-"1 2 3"}
OUT=${OUT:-results/read_path.csv}
RMI_MODELS=${RMI_MODELS:-"1024 4096 16384 65536 262144 1048576"}
INDEXES=${INDEXES:-"alex btree stdmap"}  # plus the RMI sweep

# DATASETS overrides the list, e.g. DATASETS="data/fb_200M_uint64".
if [[ -n ${DATASETS:-} ]]; then
  read -r -a datasets <<< "$DATASETS"
else
  datasets=(synthetic:lognormal synthetic:uniform synthetic:normal)
  for f in data/*_uint32 data/*_uint64; do [[ -f $f ]] && datasets+=("$f"); done
fi

make -s build/bench
for ds in "${datasets[@]}"; do
  for n in $SIZES; do
    for seed in $SEEDS; do
      if [[ $n == all ]]; then
        common=(--dataset "$ds" --n 0 --init all)
      else
        common=(--dataset "$ds" --n "$n" --init "$n")
      fi
      common+=(--ops "$OPS" --mix readonly --seed "$seed" --csv "$OUT")
      for idx in $INDEXES; do
        ./build/bench --index "$idx" "${common[@]}" --label "$idx" 2>/dev/null
      done
      for m in $RMI_MODELS; do
        [[ $n == all ]] || (( m * 2 <= n )) || continue
        ./build/bench --index rmi --rmi-models "$m" "${common[@]}" --label "rmi_$m" 2>/dev/null
      done
    done
  done
done
echo "results appended to $OUT"
