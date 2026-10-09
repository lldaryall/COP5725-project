#!/usr/bin/env python3
"""Summarise the read-only experiment against the ALEX paper's claims.

    python3 scripts/analyze_read_path.py results/read_path.csv
"""
import csv
import statistics
import sys
from collections import defaultdict


def main(path):
    runs = defaultdict(list)  # (dataset, n, label) -> rows
    for r in csv.DictReader(open(path, newline="")):
        runs[(r["dataset"], int(r["init_keys"]), r["label"])].append(r)

    def med(rows, col):
        return statistics.median(float(r[col]) for r in rows)

    errors = sum(int(r["errors"]) for rows in runs.values() for r in rows)
    cases = sorted({(d, n) for d, n, _ in runs})
    print("Read-only throughput (Mops/s, median of seeds) and index size (index_bytes)\n")
    hdr = f"{'dataset':22}{'keys':>10}  {'ALEX':>7}{'B+tree':>8}{'RMI*':>7}{'map':>7}" \
          f"  {'ALEX/B+':>8}{'ALEX/RMI':>9}  {'B+ idx/ALEX idx':>16}{'RMI idx/ALEX idx':>17}  RMI* models"
    print(hdr)
    print("-" * len(hdr))
    for d, n in cases:
        get = lambda label: runs.get((d, n, label))
        alex, bt, sm = get("alex"), get("btree"), get("stdmap")
        rmis = [(lbl, rows) for (dd, nn, lbl), rows in runs.items()
                if dd == d and nn == n and lbl.startswith("rmi_")]
        if not (alex and bt and rmis):
            continue
        best_lbl, best = max(rmis, key=lambda x: med(x[1], "throughput_mops"))
        a, b, r = (med(x, "throughput_mops") for x in (alex, bt, best))
        m = f"{med(sm, 'throughput_mops'):7.2f}" if sm else f"{'-':>7}"
        ai, bi, ri = (med(x, "index_bytes") for x in (alex, bt, best))
        print(f"{d:22}{n:>10}  {a:7.2f}{b:8.2f}{r:7.2f}{m}  {a / b:7.2f}x{a / r:8.2f}x"
              f"  {bi / ai:15.1f}x{ri / ai:16.1f}x  {best_lbl[4:]}")
    print("\nmap = std::map (not run at the largest sizes, for memory).")
    print("RMI* = best of the swept second-stage model counts (the paper tuned RMI per dataset).")
    print("Index size counts navigation structure only (B+ tree inner nodes, RMI models,")
    print("ALEX model nodes + data node metadata), matching the paper's definition.")
    print(f"\nlookup/verification errors across all runs: {errors}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
