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
  - landed phases that own terminal tree artefacts (SH0, SH5) are backed by
    those files, so a status flip cannot outlive the code it claims;
  - every phase section has a "**Gate." line (no phase lands ungated);
  - the §8 receipt strings are still listed in the plan (a renamed
    receipt silently orphans a host integration case);
  - the ledger rows are well-formed four-column rows.

Deliberately NOT asserted: existence of `.patch` files.  A patch on
disk is evidence that a FILE EXISTS, not that code works -- the
RINET2 precedent (check_rinet2_claims.py) removed exactly those
claims: they could be satisfied by `touch`-ing empty files while
every real deliverable was broken.  The deliverables this plan
tracks are code, tests and docs in the tree; the status table names
no patch files.

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


def check_plan(plan, tree_has_file):
    """Return a list of failure strings.  The tree_has_file predicate is
    injected so --selftest can plant violations."""
    fails = []

    if not plan:
        return ["SELFHOST_PLAN.md missing or unreadable"]

    # Status header must be honest about being a plan.
    if not re.search(r"^## Status:", plan, re.M):
        fails.append("header: missing '## Status:' line")

    # Phase table rows: | SH<n> — ... | <status> |
    rows = re.findall(r"^\|\s*(SH\d+)\s+—[^|]*\|([^|]*)\|", plan, re.M)
    seen = {phase for phase, _ in rows}
    for phase in PHASES:
        if phase not in seen:
            fails.append("phase table: missing row %s" % phase)

    for phase, status in rows:
        if phase == "SH0":
            if "✅" in status:
                if not tree_has_file("SELFHOST_PLAN.md"):
                    fails.append("SH0: marked ✅ but SELFHOST_PLAN.md missing")
                if not tree_has_file("tools", "check_selfhost_claims.py"):
                    fails.append("SH0: marked ✅ but checker file missing")
        if phase == "SH5" and "✅" in status:
            # SH5's whole claim is "the guest built the kernel", and the only
            # durable evidence of that is the gate that ran the build plus the
            # portable generators the guest had to use.  If any of them is
            # renamed or deleted, the ✅ becomes prose again -- exactly the
            # drift AUDIT_A7/A3 found in the other plans.
            for parts in [
                ("tests", "integration", "cases",
                 "test_selfhost_kernel_guest.sh"),
                ("tests", "integration", "lib", "prompt_qemu.py"),
                ("tests", "unit", "test_sh5d_generators.sh"),
                ("tools", "gen_user_binary.c"),
                ("tools", "gen_ap_trampoline_inc.c"),
                ("tools", "aulink", "aulink.c"),
            ]:
                if not tree_has_file(*parts):
                    fails.append("SH5: marked ✅ but %s missing"
                                 % "/".join(parts))
            # SH5 also *removed* the Python-only header emitters from the
            # kernel build path; their C twins are what the guest compiles.
            # A resurrected .py would mean the in-guest build no longer
            # covers the path the plan says it covers.
            for parts in [("tools", "gen_user_binary.py"),
                          ("tools", "gen_ap_trampoline_inc.py")]:
                if tree_has_file(*parts):
                    fails.append("SH5: marked ✅ but %s is back in the tree "
                                 "(the kernel header path must stay C-only)"
                                 % "/".join(parts))

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
        # Planted violation 1: SH0 marked ✅ but the checker file "missing".
        fails = check_plan(plan, lambda *p: p[-1] != "check_selfhost_claims.py")
        if not any("SH0: marked ✅ but checker file missing" in f
                   for f in fails):
            print("check_selfhost_claims: SELFTEST FAILED -- planted "
                  "missing-checker violation not caught")
            return 1
        # Planted violation 2: a receipt removed from the plan's §8.
        fake_plan = plan.replace("[selfhost] iso PASS:", "[selfhost] iso FAIL:")
        fails = check_plan(fake_plan, lambda *p: True)
        if not any("receipts: '[selfhost] iso PASS:' no longer listed"
                   in f for f in fails):
            print("check_selfhost_claims: SELFTEST FAILED -- planted "
                  "receipt-drift violation not caught")
            return 1
        # Planted violation 3: SH5 ✅ while its terminal gate case is gone.
        # The predicate models the real tree (the Python emitters really are
        # deleted) minus the gate case, so only the missing-artefact check
        # can fire and the assertion below is specific to it.
        if any("✅" in st for ph, st in
               re.findall(r"^\|\s*(SH\d+)\s+—[^|]*\|([^|]*)\|", plan, re.M)
               if ph == "SH5"):
            gone = ("test_selfhost_kernel_guest.sh", "gen_user_binary.py",
                    "gen_ap_trampoline_inc.py")
            fails = check_plan(plan, lambda *p: p[-1] not in gone)
            if not any("SH5: marked ✅ but tests/integration/cases/"
                       "test_selfhost_kernel_guest.sh missing" in f
                       for f in fails):
                print("check_selfhost_claims: SELFTEST FAILED -- planted "
                      "SH5-artefact violation not caught")
                return 1
        print("check_selfhost_claims: SELFTEST OK (planted violations "
              "caught)")
        return 0

    plan = read("SELFHOST_PLAN.md")

    def tree_has_file(*parts):
        return bool(read(*parts))

    fails = check_plan(plan, tree_has_file)
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
