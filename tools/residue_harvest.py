#!/usr/bin/env python3
"""The residue harvester (RESIDUE_PLAN.md R0).

Re-runs the marker greps the plan's §2 harvest was built from, so
the numbers in the ledger are LIVE, not prose.  Three metrics:

  1. per-plan marker-line counts (residue / non-goal / deferred /
     follow-up / follow-on / future work), case-insensitive;
  2. TODO.md unchecked boxes (`- [ ]`);
  3. docs/status.md 🚧/🔶 rows.

The ledger's own files are excluded (RESIDUE_PLAN.md,
docs/residue_ledger.md, CHANGELOG.md): the ledger describing debt
must not count as debt.

Usage:
    tools/residue_harvest.py           # human-readable report
    tools/residue_harvest.py --machine # "name count" lines, sorted
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MARKER = re.compile(r"residue|non-goal|deferr|follow-up|follow-on|"
                    r"future work", re.I)
EXCLUDE = {"RESIDUE_PLAN.md"}


def plan_counts():
    out = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "*_PLAN.md"))):
        name = os.path.basename(path)
        if name in EXCLUDE:
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            out[name] = sum(1 for line in fh if MARKER.search(line))
    return out


def todo_unchecked():
    try:
        with open(os.path.join(ROOT, "TODO.md"), encoding="utf-8") as fh:
            return sum(1 for line in fh if line.startswith("- [ ]"))
    except OSError:
        return -1


def status_wip_rows():
    try:
        with open(os.path.join(ROOT, "docs", "status.md"),
                  encoding="utf-8") as fh:
            return sum(1 for line in fh if "\U0001F6A7" in line or
                       "\U0001F536" in line)
    except OSError:
        return -1


def main():
    machine = "--machine" in sys.argv
    counts = plan_counts()
    todo = todo_unchecked()
    wip = status_wip_rows()
    if machine:
        for name in sorted(counts):
            print(f"{name} {counts[name]}")
        print(f"TODO.md {todo}")
        print(f"status-wip {wip}")
        return 0
    print("residue harvest (live):")
    for name, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        if n:
            print(f"  {n:3d}  {name}")
    zero = [n for n, c in counts.items() if c == 0]
    print(f"  zero-marker plans: {len(zero)}")
    print(f"  TODO.md unchecked boxes: {todo}")
    print(f"  docs/status.md WIP rows: {wip}")
    total = sum(counts.values())
    print(f"  marker lines total: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
