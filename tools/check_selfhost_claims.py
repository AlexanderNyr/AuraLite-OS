#!/usr/bin/env python3
"""Cross-check SELFHOST_PLAN.md's status against the tree.

Why this exists
---------------
The same failure class AUDIT_A7 found in FIXES_PLAN.md and AUDIT_A3 in
MATURITY_PLAN.md: a plan's status lines are prose, and prose does not
fail a build.  SELFHOST_PLAN.md is a multi-phase arc whose entire point
is that the guest cannot be trusted -- so its own status table gets the
same treatment it prescribes for the OS: verified by an outside party.

Ties the plan to the tree:
  - the status table has all ten phases SH0..SH9;
  - SH0 (the landed phase) is backed by this checker and the plan file;
  - every ✅ phase SH1..SH9 has its deliverable patch in patches/;
  - a patch that exists while its phase is not ✅ is a drift finding
    (a deliverable without a status is how plans rot);
  - every phase section has a "**Gate." line (no phase lands ungated);
  - the §8 receipt strings are still listed in the plan (a renamed
    receipt silently orphans a host integration case);
  - the ledger rows are well-formed four-column rows.

Usage:
    tools/check_selfhost_claims.py [--check]
    tools/check_selfhost_claims.py --selftest   # prove the checker can fail
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASES = ["SH%d" % n for n in range(10)]

RECEIPTS = [
    "[selfhost] tcc PASS:",
    "[selfhost] userland rebuild PASS:",
    "[selfhost] aulink PASS:",
    "[selfhost] asm PASS:",
    "[selfhost] kernel PASS:",
    "[selfhost] build PASS:",
    "[selfhost] iso PASS:",
    "[selfhost] FULL LOOP PASS",
]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def check_plan(plan, tree_has_patch, tree_has_file):
    """Return a list of failure strings.  The tree_* predicates are injected
    so --selftest can plant violations."""
    fails = []

    if not plan:
        return ["SELFHOST_PLAN.md missing or unreadable"]

    # Status header must be honest about being a plan.
    if not re.search(r"^## Status:", plan, re.M):
        fails.append("header: missing '## Status:' line")

    # Phase table rows: | SH<n> — ... | <status> | <deliverable> |
    rows = re.findall(r"^\|\s*(SH\d+)\s+—[^|]*\|([^|]*)\|([^|]*)\|", plan,
                      re.M)
    seen = {phase for phase, _, _ in rows}
    for phase in PHASES:
        if phase not in seen:
            fails.append("phase table: missing row %s" % phase)

    for phase, status, deliverable in rows:
        if phase == "SH0":
            if "✅" in status:
                if not tree_has_file("SELFHOST_PLAN.md"):
                    fails.append("SH0: marked ✅ but SELFHOST_PLAN.md missing")
                if not tree_has_file("tools", "check_selfhost_claims.py"):
                    fails.append("SH0: marked ✅ but checker file missing")
        else:
            m = re.search(r"`(patches/SELFHOST_%s_[^`]+\.patch)`" % phase,
                          deliverable)
            if "✅" in status:
                if not m:
                    fails.append("%s: marked ✅ but no deliverable patch "
                                 "named in the table" % phase)
                elif not tree_has_patch(m.group(1)):
                    fails.append("%s: marked ✅ but deliverable %s missing"
                                 % (phase, m.group(1)))
            else:
                if m and tree_has_patch(m.group(1)):
                    fails.append("%s: patch %s exists but the phase is not "
                                 "✅ -- status or file is stale"
                                 % (phase, m.group(1)))

    # Required sections (the plan's own contract).
    for sec in ["## 2. Decisions", "## 3. Phases", "## 6. What this plan "
                "does not do", "## 7. Ledger", "## 8. Receipt strings"]:
        if sec not in plan:
            fails.append("structure: missing section '%s'" % sec)

    # Every phase section must carry a Gate line.
    gates = re.findall(r"^### Phase (SH\d+).*?\n(.*?)(?=^### Phase |\Z)",
                       plan, re.S | re.M)
    for phase, body in gates:
        if "**Gate." not in body:
            fails.append("%s: phase section has no '**Gate.' line" % phase)

    # The receipt contract: every receipt still listed in the plan.
    for receipt in RECEIPTS:
        if receipt not in plan:
            fails.append("receipts: '%s' no longer listed in the plan"
                         % receipt)

    # Ledger rows must be four-column tables.
    ledger_rows = re.findall(r"^\|\s*(SH-\d+)\s*\|", plan, re.M)
    if len(ledger_rows) < 10:
        fails.append("ledger: expected >= 10 rows, found %d"
                     % len(ledger_rows))
    for row in re.findall(r"^\|.*\|.*\|.*\|.*\|.*\|$", plan, re.M):
        if re.match(r"^\|\s*SH-\d+\s*\|", row) and row.count("|") != 6:
            fails.append("ledger: malformed row: %s" % row.strip()[:60])

    return fails


def main():
    if "--selftest" in sys.argv:
        plan = read("SELFHOST_PLAN.md")
        # Planted violation 1: a phase marked ✅ whose patch is missing.
        fake_plan = plan.replace("| SH1 — Runtime limits + TinyCC "
                                 "userspace port | 🚧 pending |",
                                 "| SH1 — Runtime limits + TinyCC "
                                 "userspace port | ✅ landed |")
        fails = check_plan(fake_plan, lambda name: False, lambda *p: True)
        if not any("SH1: marked ✅ but deliverable" in f for f in fails):
            print("check_selfhost_claims: SELFTEST FAILED -- planted "
                  "missing-deliverable violation not caught")
            return 1
        # Planted violation 2: a patch exists for a phase that is not ✅.
        # SH2 is still pending, so a liar claiming its deliverable exists
        # must trip the stale-deliverable finding.
        def liar(name):
            return name == "patches/SELFHOST_SH2_userland_tcc.patch"
        fails = check_plan(plan, liar, lambda *p: True)
        if not any("patches/SELFHOST_SH2_userland_tcc.patch exists but the "
                   "phase is not ✅" in f for f in fails):
            print("check_selfhost_claims: SELFTEST FAILED -- planted "
                  "stale-deliverable violation not caught")
            return 1
        print("check_selfhost_claims: SELFTEST OK (planted violations "
              "caught)")
        return 0

    plan = read("SELFHOST_PLAN.md")

    def tree_has_patch(name):
        # name is a repo-root-relative path ("patches/SELFHOST_SH1_*.patch").
        return bool(read(name))

    def tree_has_file(*parts):
        return bool(read(*parts))

    fails = check_plan(plan, tree_has_patch, tree_has_file)
    if fails:
        print("check_selfhost_claims: FAIL -- %d finding(s):"
              % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("check_selfhost_claims: OK -- plan, phases, gates, receipts "
          "and ledger agree with the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
