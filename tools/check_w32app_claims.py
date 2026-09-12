#!/usr/bin/env python3
"""Cross-check W32APP_PLAN.md's phase claims against the tree.

Why this exists
---------------
The same failure class AUDIT_A7 found in FIXES_PLAN.md (and pinned by
FSFULL's F6 checker, OTA's O5 checker and LX's claim checker): a plan's
status lines are prose, and prose does not fail a build.  W32APP_PLAN.md
lands the OS's Win32 personality across nineteen phases (W32A-0..W32A-18),
from import ledgers to three unmodified Windows applications running
in-guest.  Each phase's "done" is only durable if the tree artefacts and
greppable RECEIPTS the phase claims actually exist.

Ties the plan to the tree:
  - every phase W32A-0..W32A-18 has a `### Phase W32A-N` section;
  - done phases (green-DONE in the heading) are backed by the files the
    phase names -- the ledgers, the instruments, the receipts doc, the
    provenance rule -- AND by their greppable receipts (a phase that
    lost its `CHECK OK` pattern, or a ledger that lost its `agree`
    confirmation, would prove nothing);
  - the W32A-0 census is structural: this checker imports the ledger
    instrument and reruns its `check` (census 348/298/86/590, union 611,
    gap 569 against the LIVE export count from w32/src/w32_bind.c), so a
    plan edit that fakes the numbers fails without touching the ledgers;
  - the REFUSE guard is structural: while an application's ledger
    contains any REFUSE row, that app's gate heading must not claim
    green-DONE -- unresolved ordinals block app gates honestly.

Deliberately NOT asserted: existence of `patches/*.patch` receipts (a
patch file on disk is evidence a FILE EXISTS, not that code works; the
RINET2 precedent), agreement with llvm-readobj (a dev-machine step -- the
`agree` confirmations are recorded in the W32A-0 Result instead), and
correctness of any implementation (that is the job of the phase gates
and the in-guest receipts in docs/w32app_receipts.md).

Usage:
    tools/check_w32app_claims.py --check
    tools/check_w32app_claims.py --selftest   # prove the checker can fail
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAN = os.path.join(ROOT, "docs", "plans", "W32APP_PLAN.md")

PHASES = ["W32A-%d" % n for n in range(0, 19)]

# App phases whose gate is guarded by a ledger's REFUSE rows:
# phase -> ledger file whose REFUSE rows block that phase's green-DONE.
APP_LEDGERS = {
    "W32A-4": "putty-0.85.imports",
    "W32A-7": "7zFM-24.09.imports",
    "W32A-11": "notepad++-8.8.9.imports",
    "W32A-13": "7z-24.09.imports",
    "W32A-15": "npp-plugins-8.8.9.imports",
}

# Durable artefacts each done phase must have (existence checks).
ARTEFACTS = {
    "W32A-0": ["docs/plans/W32APP_PLAN.md",
               "tools/w32_import_ledger.py",
               "w32/app_ledger/putty-0.85.imports",
               "w32/app_ledger/7zFM-24.09.imports",
               "w32/app_ledger/7z-24.09.imports",
               "w32/app_ledger/notepad++-8.8.9.imports",
               "w32/app_ledger/npp-plugins-8.8.9.imports",
               "docs/w32app_receipts.md",
               "tools/check_w32app_claims.py",
               "tools/check_provenance.sh"],
    "W32A-1": ["w32/ordinal_map.tsv",
               "w32/stub_map.tsv",
               "w32/include/w32/w32_gen.h",
               "w32/src/w32_stubs_gen.c",
               "tools/w32_gen_stubs.py",
               "tools/w32_mkstubmap.py",
               "w32/include/w32/oleaut32.h",
               "w32/src/w32_oleaut32.c",
               "w32/include/w32/w32_manifest.h",
               "w32/src/w32_manifest.c",
               "w32/tests/W32A1.bindreport",
               "tests/unit/test_w32_a1.c",
               "tests/unit/test_w32_a1.h",
               "tests/unit/test_w32_a1_check.c",
               "tests/unit/test_w32_a1_fixtures1.c",
               "tests/unit/test_w32_a1_fixtures2.c",
               "tests/unit/test_w32_a1_report.c",
               "tests/integration/cases/test_w32_a1_loader.sh"],
}

# Greppable receipts each done phase must carry in its plan section.
RECEIPTS = {
    "W32A-0": ["CHECK OK",
               "llvm-readobj",
               "611",
               "569",
               "patches/W32A0_ledger.patch"],
    "W32A-1": ["patches/W32A1_loader.patch",
               "34/34",
               "W32A1.bindreport"],
}


def phase_section(plan_text, phase):
    """Return the markdown of one phase section, or None."""
    heads = [(m.start(), m.group(1))
             for m in re.finditer(r"^### Phase (W32A-\d+)\b", plan_text, re.M)]
    for i, (pos, name) in enumerate(heads):
        if name == phase:
            end = heads[i + 1][0] if i + 1 < len(heads) else len(plan_text)
            return plan_text[pos:end]
    return None


def run_check(root=ROOT):
    fails = []

    def bad(msg):
        fails.append(msg)
        print("  FAIL: %s" % msg)

    plan_path = os.path.join(root, "docs", "plans", "W32APP_PLAN.md")
    if not os.path.isfile(plan_path):
        bad("missing docs/plans/W32APP_PLAN.md")
        return fails
    plan = open(plan_path).read()

    for phase in PHASES:
        sec = phase_section(plan, phase)
        if sec is None:
            bad("plan has no '### Phase %s' section" % phase)
            continue
        done = "\u2705 DONE" in sec.split("\n", 1)[0]
        if not done:
            continue
        # Artefacts exist.
        for rel in ARTEFACTS.get(phase, []):
            if not os.path.isfile(os.path.join(root, rel)):
                bad("%s is DONE but %s is missing" % (phase, rel))
        # Receipts are greppable in the section.
        for pat in RECEIPTS.get(phase, []):
            if pat not in sec:
                bad("%s is DONE but its section never says %r" % (phase, pat))

    # W32A-0 structural checks (only meaningful once it claims DONE).
    sec0 = phase_section(plan, "W32A-0")
    if sec0 is not None and "\u2705 DONE" in sec0.split("\n", 1)[0]:
        # The census reruns from the committed ledgers + live exports.
        sys.path.insert(0, os.path.join(root, "tools"))
        try:
            import w32_import_ledger
        except ImportError as e:
            bad("cannot import tools/w32_import_ledger.py: %s" % e)
        else:
            # Point the instrument at the tree under test.
            w32_import_ledger.LEDGER_DIR = os.path.join(
                root, "w32", "app_ledger")
            w32_import_ledger.BIND_C = os.path.join(
                root, "w32", "src", "w32_bind.c")
            if w32_import_ledger.cmd_check() != 0:
                bad("tools/w32_import_ledger.py check fails on this tree")
        # The provenance gate carries the W32A-0 PE rule.
        prov = os.path.join(root, "tools", "check_provenance.sh")
        if os.path.isfile(prov):
            if "W32A-0: PE rule" not in open(prov).read():
                bad("tools/check_provenance.sh lacks the W32A-0 PE rule")
        # The receipts doc defines the protocol the plan cites.
        rec = os.path.join(root, "docs", "w32app_receipts.md")
        if os.path.isfile(rec):
            rect = open(rec).read()
            for pat in ("REFUSE guard", "MZ", "agree"):
                if pat not in rect:
                    bad("docs/w32app_receipts.md never says %r" % pat)

    # The REFUSE guard: no app gate flips green while its ledger has
    # REFUSE rows.  Runs regardless of DONE flags -- a violation is a
    # violation the moment the heading claims it.
    for phase, ledger in sorted(APP_LEDGERS.items()):
        sec = phase_section(plan, phase)
        if sec is None or "\u2705 DONE" not in sec.split("\n", 1)[0]:
            continue
        lp = os.path.join(root, "w32", "app_ledger", ledger)
        if not os.path.isfile(lp):
            bad("%s is DONE but %s is missing" % (phase, ledger))
            continue
        n_refuse = sum(1 for ln in open(lp)
                       if re.match(r"^\S+ \S+ REFUSE ", ln))
        if n_refuse:
            bad("%s is DONE but %s still has %d REFUSE row(s)" % (
                phase, ledger, n_refuse))
        # The receipt pin: a green app gate names a filled receipt
        # section, never an AWAITING stub (D2).
        rec = os.path.join(root, "docs", "w32app_receipts.md")
        if not os.path.isfile(rec):
            bad("%s is DONE but docs/w32app_receipts.md is missing" % phase)
            continue
        rect = open(rec).read()
        m = re.search(r"^## %s\b.*" % re.escape(phase), rect, re.M)
        if m is None or "AWAITING" in rect[m.start():m.start() + 400]:
            bad("%s is DONE but its receipt section is AWAITING" % phase)

    return fails


def main(argv):
    if len(argv) == 2 and argv[1] == "--check":
        print("[w32app-claims] checking W32APP_PLAN.md against the tree ...")
        fails = run_check()
        if fails:
            print("[w32app-claims] FAIL: %d problem(s)" % len(fails))
            return 1
        print("[w32app-claims] PASS: done-phase claims are backed by the tree")
        return 0
    if len(argv) == 2 and argv[1] == "--selftest":
        import tempfile
        import shutil
        tmp = tempfile.mkdtemp()
        try:
            # Plant a lying tree: plan claims W32A-0 DONE, artefacts absent;
            # plan claims W32A-4 DONE with no receipt section at all.
            os.makedirs(os.path.join(tmp, "docs", "plans"))
            open(os.path.join(tmp, "docs", "plans", "W32APP_PLAN.md"),
                 "w").write("### Phase W32A-0 \u2705 DONE\nno receipts here\n"
                            "### Phase W32A-4 \u2705 DONE\nno receipt\n")
            print("[w32app-claims] self-test: planting a DONE claim "
                  "with no artefacts ...")
            fails = run_check(root=tmp)
            if not fails:
                print("[w32app-claims] SELF-TEST FAILED: checker accepted "
                      "an unbacked DONE claim")
                return 1
            print("    (%d problem(s) detected as required)" % len(fails))
            print("[w32app-claims] self-test PASS: violation was detected "
                  "as required")
            return 0
        finally:
            shutil.rmtree(tmp)
    print(__doc__.split("Usage:")[1].strip())
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
