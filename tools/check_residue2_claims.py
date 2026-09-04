#!/usr/bin/env python3
"""Cross-check RESIDUE2_PLAN.md against TODO.md and the residue ledger.

RESIDUE2 T0: the residue sequel cannot drift.  The plan's coverage
contract is pinned here as live checks — every TODO box carries the
`(RESIDUE2 T#)` tag of the phase that closes it, every tagged phase
exists, every OPEN ledger row is named in the plan, and the plan's
Status header agrees with its phase table — when a later phase moves
one, the pin moves in the same commit or CI is red.

Usage:
    tools/check_residue2_claims.py
    tools/check_residue2_claims.py --selftest
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASE_ORDER = ["T0", "T1", "T2", "T3", "T4",
               "T5", "T6", "T7", "T8", "T9"]

# Phases whose contract is a ledger row rather than a TODO box are
# exempt from the "tagged by at least one box" half of the coverage
# claim (their named rows must appear in the plan instead).
BOX_OPTIONAL = {"T0"}

PENDING_USER_ROWS = ["RES-30", "RES-32", "RES-33", "RES-48"]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def todo_boxes(todo):
    """The unchecked boxes as LOGICAL entries: the `- [ ]` line plus its
    wrapped continuation lines.  Returns a list of (line_no, text)."""
    entries = []
    lines = todo.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("- [ ]"):
            j = i + 1
            while j < len(lines) and re.match(r"^\s+\S", lines[j]):
                j += 1
            entries.append((i + 1, " ".join(
                ln.strip() for ln in lines[i:j])))
            i = j
        else:
            i += 1
    return entries


def phase_done(plan, phase):
    return bool(re.search(
        r"^### %s[^\n]*\n+(?:\*\*)?Status: ✅ COMPLETE" % phase,
        plan, re.M))


def phase_section_names_ledger(plan, phase):
    r"""True iff the phase's section names at least one RES-\d\d row."""
    m = re.search(r"^### %s —.*?(?=^### |\Z)" % phase, plan,
                  re.M | re.S)
    return bool(m and re.search(r"RES-\d\d", m.group(0)))


def claims():
    plan = read("docs", "plans", "RESIDUE2_PLAN.md")
    todo = read("TODO.md")
    ledger = read("docs", "residue_ledger.md")
    checks = []

    # --- the plan exists and has the phase table ----------------------
    checks.append(("plan: docs/plans/RESIDUE2_PLAN.md exists", plan != ""))
    table_rows = re.findall(r"^\| (T\d+) — ", plan, re.M)
    checks.append((
        "plan: the phase table lists exactly T0–T9 in order",
        table_rows == PHASE_ORDER))

    # --- the coverage contract ----------------------------------------
    boxes = todo_boxes(todo)
    checks.append((
        "todo: there is at least one open box to cover",
        len(boxes) > 0))
    untagged = [(n, t) for n, t in boxes
                if not re.search(r"\(RESIDUE2 T\d+\)", t)]
    checks.append((
        "coverage: every TODO box carries a (RESIDUE2 T#) tag "
        "(untagged: %s)" % [n for n, _ in untagged],
        not untagged))
    multi = [(n, t) for n, t in boxes
             if len(re.findall(r"\(RESIDUE2 T\d+\)", t)) > 1]
    checks.append((
        "coverage: no box carries two tags", not multi))

    tags = {}
    for _, t in boxes:
        m = re.search(r"\(RESIDUE2 (T\d+)\)", t)
        if not m:
            continue  # already counted by the untagged claim
        ph = m.group(1)
        tags[ph] = tags.get(ph, 0) + 1
    unknown = [ph for ph in tags if ph not in PHASE_ORDER]
    checks.append((
        "coverage: every tagged phase is one of T0–T9 (found %s)"
        % unknown, not unknown))

    headings = re.findall(r"^### (T\d+) —", plan, re.M)
    checks.append((
        "plan: every T0–T9 phase has a section heading",
        headings == PHASE_ORDER))

    orphans = [ph for ph in tags
               if ph not in BOX_OPTIONAL
               and tags[ph] > 0
               and not phase_section_names_ledger(plan, ph)
               and not re.search(r"Boxes?:.*%s" % ph, plan, re.S)
               and ph not in headings]
    # The real orphan rule: a phase tagged by boxes must have its own
    # section (it does if it is in `headings`) — the expression above
    # reduces to that after the exempt set.
    orphans = [ph for ph in tags if ph not in BOX_OPTIONAL
               and ph not in headings]
    checks.append(("coverage: no tag points at a headless phase",
                   not orphans))

    for ph in PHASE_ORDER:
        if ph in BOX_OPTIONAL:
            continue
        ok = tags.get(ph, 0) > 0 or phase_section_names_ledger(plan, ph)
        checks.append((
            "coverage: %s is tagged by ≥1 box or names its ledger rows"
            % ph, ok))

    # --- the ledger pairs ---------------------------------------------
    open_rows = re.findall(r"^\| (RES-\d\d) \| \w+ \| OPEN \|",
                           ledger, re.M)
    checks.append((
        "ledger: there are OPEN rows to schedule (found %s)" % open_rows,
        len(open_rows) > 0))
    unnamed = [r for r in open_rows if r not in plan]
    checks.append((
        "plan: every OPEN ledger row is named in the plan (unnamed: %s)"
        % unnamed, not unnamed))
    pu_missing = [r for r in PENDING_USER_ROWS if r not in plan]
    checks.append((
        "plan: every PENDING-USER row is named in the plan's §4 "
        "(missing: %s)" % pu_missing, not pu_missing))

    # --- status header vs phase table ---------------------------------
    done_heads = sum(1 for p in PHASE_ORDER if phase_done(plan, p))
    header = re.search(r"^## Status: (\w+)", plan, re.M)
    status_ok = False
    if header and header.group(1) == "PLANNED":
        status_ok = done_heads == 0
    elif header and header.group(1) == "IN":
        status_ok = 0 < done_heads < len(PHASE_ORDER)
    elif header and header.group(1) == "COMPLETE":
        status_ok = done_heads == len(PHASE_ORDER)
    checks.append((
        "plan: the Status header agrees with the table (%d/%d ✅, %r)"
        % (done_heads, len(PHASE_ORDER),
           header.group(0) if header else ""), status_ok))

    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(ok for _, ok in results):
            print("check_residue2_claims: SELFTEST inconclusive (tree "
                  "already red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(ok for _, ok in doctored):
            print("check_residue2_claims: SELFTEST FAIL -- passes against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_residue2_claims: selftest PASS (doctored tree "
              "detected)")
        return 0

    results = claims()
    bad = [name for name, ok in results if not ok]
    for name in bad:
        print(f"check_residue2_claims: FAIL -- {name}", file=sys.stderr)
    if bad:
        print(f"check_residue2_claims: {len(bad)} claim(s) disagree "
              f"with the tree", file=sys.stderr)
        return 1
    print(f"check_residue2_claims: OK -- {len(results)} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
