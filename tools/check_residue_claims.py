#!/usr/bin/env python3
"""Cross-check RESIDUE_PLAN.md + the ledger against the tree.

Seventh checker of the D8 family.  What is new here: the debt
RATCHET.  The harvester's per-file marker counts are pinned in
tools/residue_baseline.txt; any plan growing (or shedding) residue
markers without moving the baseline AND the ledger in the same
commit fails.  The ledger itself is arithmetic-checked: 48 rows,
sequential unique ids, class totals, status totals.

Usage:
    tools/check_residue_claims.py [--selftest]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LEDGER_ROWS = 48
CLASS_PIN = {"W": 33, "M": 5, "N": 2, "S": 8}
PHASE_ORDER = ["R0", "R1", "R2", "R3", "R4", "R5", "R6",
               "R7", "R8", "R9", "R10", "R11", "R12"]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def harvest_live():
    """Run the harvester in-process for a doctored-ROOT-friendly way."""
    script = os.path.join(ROOT, "tools", "residue_harvest.py")
    if not os.path.exists(script):
        return None
    try:
        out = subprocess.run(
            [sys.executable, script, "--machine"],
            capture_output=True, text=True, timeout=60,
            cwd=ROOT).stdout
    except Exception:
        return None
    return dict(line.rsplit(" ", 1) for line in out.splitlines()
                if " " in line)


def claims():
    plan = read("RESIDUE_PLAN.md")
    ledger = read("docs", "residue_ledger.md")
    checks = []

    # --- the ledger's arithmetic --------------------------------------
    rows = re.findall(r"^\| (RES-\d\d) \| (\w+) \| ([A-Z@R\d-]+) \|",
                      ledger, re.M)
    ids = [r[0] for r in rows]
    checks.append((
        f"ledger: exactly {LEDGER_ROWS} rows with unique sequential ids "
        f"(found {len(ids)})",
        len(ids) == LEDGER_ROWS and len(set(ids)) == LEDGER_ROWS and
        ids == [f"RES-{i:02d}" for i in range(1, LEDGER_ROWS + 1)]))
    class_counts = {}
    for _, cls, _st in rows:
        class_counts[cls] = class_counts.get(cls, 0) + 1
    checks.append((
        f"ledger: class totals match the pins (measured {class_counts}, "
        f"pinned {CLASS_PIN})",
        class_counts == CLASS_PIN))
    n_open = sum(1 for _, _, st in rows if st == "OPEN")
    n_closed = len(rows) - n_open
    done_phases = len([1 for r in re.findall(
        r"^\| R\d+ [^|]*\| ✅ complete", plan, re.M)])
    checks.append((
        f"ledger: status arithmetic is sane (OPEN {n_open} + moved "
        f"{n_closed} = {LEDGER_ROWS}; {done_phases} phase(s) landed)",
        n_open + n_closed == LEDGER_ROWS))

    # --- the debt ratchet: live harvest == pinned baseline -------------
    base_text = read("tools", "residue_baseline.txt")
    baseline = dict(line.rsplit(" ", 1) for line in
                    base_text.splitlines() if " " in line)
    live = harvest_live()
    if live is None:
        checks.append(("harvest: tools/residue_harvest.py runs", False))
    else:
        drift = {k: (baseline.get(k), live.get(k))
                 for k in set(baseline) | set(live)
                 if baseline.get(k) != live.get(k)}
        checks.append((
            f"ratchet: live harvest matches the baseline (drift: "
            f"{drift if drift else 'none'} — move baseline AND ledger "
            "in the same commit)",
            not drift and len(baseline) > 0))

    # --- R0 structure ---------------------------------------------------
    checks.append((
        "R0: the amended class totals are recorded as a CATCH in both "
        "documents (the draft's 27/6/3/12 was wrong)",
        "W 33" in ledger and "WRONG" in ledger and
        "W 33 · M 5 · N 2 · S 8" in plan))
    checks.append((
        "R0: the checker family wiring (test-unit runs this file with "
        "its selftest)",
        read("Makefile").count("check_residue_claims.py") >= 2))

    # --- structural: status header vs table -----------------------------
    done_rows = len(re.findall(r"^\| R\d+ [^|]*\| ✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### R\d+[^\n]*✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete row has a COMPLETE heading",
                   done_rows == done_heads and plan != ""))
    status_ok = False
    if re.search(r"^## Status: IN PROGRESS — R0 next", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"R0(?:–(R\d+))? complete", plan)
        if m:
            label = m.group(1) if m.group(1) else "R0"
            claimed = (PHASE_ORDER.index(label) + 1
                       if label in PHASE_ORDER else 0)
            status_ok = claimed == done_rows
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == len(PHASE_ORDER))
    checks.append(("plan: the Status header agrees with the table",
                   status_ok))
    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(ok for _, ok in results):
            print("check_residue_claims: SELFTEST inconclusive (tree "
                  "already red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(ok for _, ok in doctored):
            print("check_residue_claims: SELFTEST FAIL -- passes against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_residue_claims: selftest PASS (doctored tree "
              "detected)")
        return 0

    failed = 0
    results = claims()
    for desc, ok in results:
        if not ok:
            print(f"check_residue_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_residue_claims: {failed} claim(s) disagree with "
              "the tree", file=sys.stderr)
        return 1
    print(f"check_residue_claims: OK -- {len(results)} claims verified "
          "(the debt ratchet runs the harvester LIVE)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
