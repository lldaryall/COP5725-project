#!/usr/bin/env python3
"""Print a summary table from a bench CSV (stdlib only).

    python3 scripts/summarize.py results/baselines.csv
"""
import csv
import sys

COLS = [
    ("label", "label", str),
    ("index", "index", str),
    ("dataset", "dataset", str),
    ("pattern", "pattern", str),
    ("throughput_mops", "Mops/s", lambda v: f"{float(v):.2f}"),
    ("p50_ns", "p50 ns", str),
    ("p99_ns", "p99 ns", str),
    ("index_bytes", "index MB", lambda v: f"{int(v) / 1e6:.2f}"),
    ("data_bytes", "data MB", lambda v: f"{int(v) / 1e6:.2f}"),
    ("errors", "err", str),
]


def main(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    table = [[title for _, title, _ in COLS]]
    for r in rows:
        table.append([fmt(r[key]) if r[key] != "" else "-" for key, _, fmt in COLS])
    widths = [max(len(row[i]) for row in table) for i in range(len(COLS))]
    for i, row in enumerate(table):
        print("  ".join(cell.ljust(w) for cell, w in zip(row, widths)))
        if i == 0:
            print("  ".join("-" * w for w in widths))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
