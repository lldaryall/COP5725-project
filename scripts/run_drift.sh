#!/usr/bin/env bash
# Extension experiments: sweep the rate of distribution shift and record
# throughput over time (results/drift_series.csv) to separate graceful
# degradation from thrashing. Runs the baselines now; ALEX is added later.
set -euo pipefail
cd "$(dirname "$0")/.."
DS=${DS:-synthetic:lognormal}
INIT=${INIT:-1000000}
OPS=${OPS:-2000000}
INDEXES=${INDEXES:-"stdmap btree"}
SUM=${SUM:-results/drift.csv}
SER=${SER:-results/drift_series.csv}
common=(--dataset "$DS" --n $((INIT + OPS)) --init "$INIT" --ops "$OPS" --insert-frac 0.5
        --csv "$SUM" --series "$SER")

make -s build/bench
for idx in $INDEXES; do
  # Hot range: share of inserts hitting a 1%-wide slice ramps 0 -> 100%.
  for w in 0.1 0.01 0.001; do
    ./build/bench --index "$idx" "${common[@]}" --pattern hotrange --hot-width "$w" \
      --label "hot_w$w"
  done
  # Drift: insert centre moves at a controlled rate (quantile units / 1M inserts).
  for rate in 0 0.01 0.1 0.5 2; do
    ./build/bench --index "$idx" "${common[@]}" --pattern drift --drift-from 0.1 \
      --drift-rate "$rate" --label "drift_r$rate"
  done
  # Regime change halfway through: to a narrow band, and to appends past the max.
  ./build/bench --index "$idx" "${common[@]}" --pattern regime --regime-center 0.1 \
    --label regime_band
  ./build/bench --index "$idx" "${common[@]}" --pattern regime --regime-append 1 \
    --label regime_append
done
echo "results appended to $SUM and $SER"
