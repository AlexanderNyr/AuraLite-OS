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
    "[selfhost] script PASS:",
    "[selfhost] redirect PASS:",
    "[selfhost] pipe PASS:",
    "[selfhost] control PASS:",
    "[selfhost] shmake PASS:",
    "[selfhost] build PASS:",
    "[selfhost] sha256 PASS:",
    "[selfhost] mkinitrd PASS:",
    "[selfhost] boot-offset header PASS:",
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

    # SH6e is a sub-phase (SH6e, not SH6), so the SH\d+ table walk above
    # does not see it.  Pin the artefacts the ✅ claims independently.
    if re.search(r"^\| SH6e .*\| ✅", plan, re.M):
        for parts in [
            ("tools", "shmake", "shmake.c"),
            ("tests", "integration", "cases", "test_selfhost_shmake.sh"),
            ("tests", "unit", "test_shmake.sh"),
        ]:
            if not tree_has_file(*parts):
                fails.append("SH6e: marked ✅ but %s missing"
                             % "/".join(parts))

    if re.search(r"^\| SH6f .*\| ✅", plan, re.M):
        for parts in [
            ("tools", "selfhost", "build.sh"),
            ("tools", "selfhost", "Selfhost.mk"),
            ("tests", "integration", "cases", "test_selfhost_build.sh"),
        ]:
            if not tree_has_file(*parts):
                fails.append("SH6f: marked ✅ but %s missing"
                             % "/".join(parts))
        # D5: host Makefile and Selfhost.mk name the same target set.
        make_txt = read("Makefile")
        guest_txt = read("tools", "selfhost", "Selfhost.mk")
        hm = re.search(r"^SELFHOST_TARGETS\s*:?=\s*(.*)$", make_txt, re.M)
        gm = re.search(r"^# SELFHOST_TARGETS:\s*(.*)$", guest_txt, re.M)
        hs = set(hm.group(1).split()) if hm else None
        gs = set(gm.group(1).split()) if gm else None
        if hs is None:
            fails.append("D5: Makefile missing SELFHOST_TARGETS")
        if gs is None:
            fails.append("D5: Selfhost.mk missing SELFHOST_TARGETS")
        if hs is not None and gs is not None and hs != gs:
            fails.append("D5: target set drift Makefile=%s Selfhost.mk=%s"
                         % (" ".join(sorted(hs)), " ".join(sorted(gs))))

    # SH7a is a sub-phase (SH7a, not SH7), so the SH\d+ table walk does not
    # see it.  Pin the artefacts the ✅ claims independently, like SH6e/SH6f:
    # the tool source, its host unit test, and the in-guest gate case.
    if re.search(r"^\| SH7a .*\| ✅", plan, re.M):
        for parts in [
            ("tools", "selfhost", "sha256sum.c"),
            ("tools", "selfhost", "sh7a_probe.sh"),
            ("tests", "unit", "test_sha256sum.c"),
            ("tests", "integration", "cases",
             "test_selfhost_sha256sum.sh"),
        ]:
            if not tree_has_file(*parts):
                fails.append("SH7a: marked ✅ but %s missing"
                             % "/".join(parts))

    # SH7b is likewise a sub-phase: the in-guest USTAR writer.  Pin the tool
    # source, its host unit test, the in-guest probe and the integration case.
    if re.search(r"^\| SH7b .*\| ✅", plan, re.M):
        for parts in [
            ("tools", "selfhost", "mkinitrd.c"),
            ("tools", "selfhost", "sh7b_probe.sh"),
            ("tests", "unit", "test_mkinitrd.c"),
            ("tests", "integration", "cases",
             "test_selfhost_mkinitrd.sh"),
        ]:
            if not tree_has_file(*parts):
                fails.append("SH7b: marked ✅ but %s missing"
                             % "/".join(parts))

    # SH7c: the in-guest boot_info_t offset generator/verifier.
    if re.search(r"^\| SH7c .*\| ✅", plan, re.M):
        for parts in [
            ("tools", "selfhost", "bootoffsets.c"),
            ("tools", "selfhost", "sh7c_probe.sh"),
            ("tests", "unit", "test_bootoffsets_twin.c"),
            ("tests", "integration", "cases",
             "test_selfhost_bootoffsets.sh"),
        ]:
            if not tree_has_file(*parts):
                fails.append("SH7c: marked ✅ but %s missing"
                             % "/".join(parts))

    # SH7d: the in-guest MBR + GPT + FAT32 ESP writer.  Pin the tool source,
    # its host twin test, the in-guest probe and the integration case.
    if re.search(r"^\| SH7d .*\| ✅", plan, re.M):
        for parts in [
            ("tools", "selfhost", "mkiso.c"),
            ("tools", "selfhost", "sh7d_probe.sh"),
            ("tests", "unit", "test_mkiso.c"),
            ("tests", "integration", "cases",
             "test_selfhost_mkiso.sh"),
        ]:
            if not tree_has_file(*parts):
                fails.append("SH7d: marked ✅ but %s missing"
                             % "/".join(parts))

    # SH7e: the terminal ISO-assembly gate.  Pin the in-guest probe and the
    # host integration case; the wiring it drives (build.sh + Selfhost.mk) is
    # already pinned under SH6f (D5), so SH7e pins only what it adds.
    if re.search(r"^\| SH7e .*\| ✅", plan, re.M):
        for parts in [
            ("tools", "selfhost", "sh7e_probe.sh"),
            ("tests", "integration", "cases",
             "test_selfhost_iso.sh"),
        ]:
            if not tree_has_file(*parts):
                fails.append("SH7e: marked ✅ but %s missing"
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
