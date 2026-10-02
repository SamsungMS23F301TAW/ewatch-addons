#!/usr/bin/env python3
"""Print golden ARTHASH lines for test/test_art/golden.inc.

Only run this when introducing a NEW algorithm version. The ALGO_VERSION 1
table is frozen: if its hashes change, people's recorded days would re-render
differently, so a change there is a bug to fix, not a table to refresh.

    tools/preview/build.sh && python3 tools/preview/make_golden.py > test/test_art/golden.inc
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = [
    # Two weeks cover every family twice; then special days and step extremes.
    ("2026-10-05", 0), ("2026-10-06", 1500), ("2026-10-07", 3000), ("2026-10-08", 4500),
    ("2026-10-09", 6000), ("2026-10-10", 7500), ("2026-10-11", 9000), ("2026-10-12", 10500),
    ("2026-10-13", 12000), ("2026-10-14", 13500), ("2026-10-15", 15000), ("2026-10-16", 16500),
    ("2026-10-17", 20000), ("2026-10-18", 40000),
    ("2027-01-01", 5000), ("2026-06-21", 8000), ("2026-12-21", 8000), ("2026-03-20", 8000),
    ("2026-09-23", 2000), ("2025-02-14", 7777), ("2030-07-04", 11111),
]

for date, steps in CASES:
    out = subprocess.run([os.path.join(HERE, "preview"), "hash", date, str(steps)],
                         check=True, capture_output=True, text=True).stdout.split()
    y, m, d = (int(v) for v in date.split("-"))
    print("  { %d, %d, %d, %u, 0x%sU }," % (y, m, d, steps, out[-1]))
