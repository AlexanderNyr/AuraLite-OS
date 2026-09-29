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
    green-DONE -- unresolved ordinals block app gates honestly;
  - every done phase W32A-5..W32A-18 pins the module source + header it
    lands, its host unit test and its in-guest integration case, plus the
    greppable receipts (patch line + case name) its plan section must carry;
  - the Outcome-B guard is structural: an app that deliberately does NOT
    run (W32A-17, Audacity) has no .imports ledger and no REFUSE machinery;
    its gate is green only while its .gap ledger enumerates at least one
    unmet module and its receipt section is filled, never AWAITING.

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
    "W32A-14": "putty-0.85.imports",
    "W32A-15": "7zFM-24.09.imports",
    "W32A-16": "notepad++-8.8.9.imports",
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
    "W32A-2": ["w32/tests/w32a2_common.h",
               "w32/tests/w32a2_find.c",
               "w32/tests/w32a2_time.c",
               "w32/tests/w32a2_map.c",
               "w32/tests/w32a2_pipes.c",
               "w32/tests/w32a2_proc.c",
               "w32/tests/w32a2_locale.c",
               "w32/tests/w32a2_heap.c",
               "tests/integration/cases/test_w32_a2_kernel32.sh"],
    "W32A-3": ["w32/src/kernel32_thr.c",
               "w32/include/w32/w32_teb.h",
               "tests/unit/test_w32_a3.c",
               "w32/tests/w32a3_threads.asm",
               "w32/tests/w32a3_tls.asm",
               "tests/integration/cases/test_w32_a3.sh",
               "kernel/arch/x86_64/syscall_entry.asm"],
    "W32A-4": ["w32/src/w32_seh.c",
               "w32/include/w32/w32_seh.h",
               "tests/unit/test_w32_a4.c",
               "w32/tools/unwinddump.c",
               "tests/unit/test_w32_a4equiv.py",
               "w32/tests/w32a4_try.asm",
               "w32/tests/w32a4_unwind.asm",
               "w32/tests/w32a4_raise.asm",
               "w32/tests/w32a4_continue.asm",
               "w32/tests/w32a4_cxthrow.asm",
               "w32/tests/w32a4_filter.asm",
               "w32/tests/w32a4_crash.asm",
               "w32/tests/w32a4_term.asm",
               "w32/tests/w32a4_purecall.asm",
               "w32/tests/w32a4_dialog.asm",
               "w32/tests/w32a4_cxx.cpp",
               "w32/tests/msvcrt.def",
               "tests/integration/cases/test_w32_a4_unwind.sh"],

    # W32A-5..W32A-13: the personality modules.  Each done phase is pinned
    # to the module source + public header it lands, its host unit test and
    # its in-guest integration case.  (Patch existence is a RECEIPT grep in
    # the plan section, never a tree existence check -- see the docstring.)
    "W32A-5": ["w32/src/user32_win.c",
               "w32/include/w32/user32_priv.h",
               "tests/unit/test_w32_a5.c",
               "tests/integration/cases/test_w32a5_user32win.sh"],
    "W32A-6": ["w32/src/w32_dlg.c",
               "tests/unit/test_w32_a6.c",
               "tests/unit/test_w32_a6_pe.h",
               "tests/integration/cases/test_w32a6_user32dlg.sh"],
    "W32A-7": ["w32/src/w32_gdi.c",
               "w32/include/w32/gdi32.h",
               "tests/unit/test_w32_a7.c",
               "tests/integration/cases/test_w32a7_gdi.sh"],
    "W32A-8": ["w32/src/comctl32.c",
               "w32/include/w32/comctl32.h",
               "tests/unit/test_w32_a8.c",
               "tests/integration/cases/test_w32a8_comctl32.sh"],
    "W32A-9": ["w32/src/advapi32.c",
               "w32/include/w32/advapi32.h",
               "tests/unit/test_w32_a9.c",
               "tests/integration/cases/test_w32a9_registry.sh"],
    "W32A-10": ["w32/src/shell32.c",
                "w32/include/w32/shell32.h",
                "tests/unit/test_w32_a10.c",
                "tests/unit/test_w32_a10_pe.h",
                "tests/integration/cases/test_w32a10_furniture.sh"],
    "W32A-11": ["w32/src/ole32.c",
                "w32/src/w32_oleaut32.c",
                "w32/src/w32_ole_drag.c",
                "w32/src/w32_clipfmt.c",
                "w32/src/imm32.c",
                "w32/src/uxtheme.c",
                "tests/unit/test_w32_a11_com.c",
                "tests/integration/cases/test_w32a11_ole.sh"],
    "W32A-12": ["w32/src/ws2_32.c",
                "w32/include/w32/ws2_32.h",
                "tests/unit/test_w32_a12_ws2_32.c",
                "tests/integration/cases/test_w32a12_winsock.sh"],
    "W32A-13": ["w32/src/msvcrt.c",
                "w32/include/w32/msvcrt.h",
                "tests/unit/test_w32_a13_msvcrt.c",
                "tests/integration/cases/test_w32a13_msvcrt.sh"],

    # W32A-14..W32A-16: the three unmodified applications.  Pinned to the
    # committed import ledger(s) the census consumed, the host unit test and
    # the in-guest fixture case.  The REFUSE guard (below) is the honesty
    # gate that keeps these green only while their ledgers are clean.
    "W32A-14": ["w32/app_ledger/putty-0.85.imports",
                "tests/unit/test_w32_a14_con.c",
                "tests/integration/cases/test_w32a14_putty_fixture.sh"],
    "W32A-15": ["w32/app_ledger/7zFM-24.09.imports",
                "w32/app_ledger/7z-24.09.imports",
                "tests/unit/test_w32_a15_bitmap.c",
                "tests/integration/cases/test_w32a15_7zip_fixture.sh"],
    "W32A-16": ["w32/app_ledger/notepad++-8.8.9.imports",
                "w32/app_ledger/npp-plugins-8.8.9.imports",
                "w32/src/shlwapi.c",
                "tests/unit/test_w32_a16_shlwapi.c",
                "tests/unit/test_w32_a16_layout_icon.c",
                "tests/integration/cases/test_w32a16_npp_fixture.sh"],

    # W32A-17 (Audacity): Outcome B -- the app does NOT run, and that is the
    # deliverable.  There is no .imports ledger and no REFUSE guard; the
    # honest artefact is the .gap ledger enumerating the 19 unmet modules,
    # plus the provenance rule that governs .gap files.  The receipt gate
    # (GAP_RECEIPTS, below) proves the receipt section is filled, not a stub.
    "W32A-17": ["w32/app_ledger/audacity-3.7.5.gap",
                "w32/PROVENANCE.md"],

    # W32A-18 (this phase): integration, documentation, honest matrix.  The
    # artefacts are the generated API reference, the residue ledger, the
    # receipts protocol doc and this checker + the table generator it wires.
    "W32A-18": ["tools/check_w32app_claims.py",
                "tools/gen_w32_api_table.py",
                "docs/win32.md",
                "docs/residue_ledger.md",
                "docs/w32app_receipts.md"],
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
    "W32A-2": ["patches/W32A2_kernel32fs.patch",
               "677/0",
               "test_w32_a2_kernel32"],
    "W32A-3": ["patches/W32A3_threads.patch",
               "963/0",
               "test_w32_a3"],
    "W32A-4": ["patches/W32A4_unwind.patch",
               "19/19",
               "test_w32_a4_unwind"],
    # W32A-5..W32A-17: each section names its patch (a receipt grep, not a
    # tree existence check) and its in-guest case, so a section that lost
    # its evidence line fails even though the code files still exist.
    "W32A-5": ["patches/W32A5_user32win.patch", "test_w32a5_user32win"],
    "W32A-6": ["patches/W32A6_user32dlg.patch", "test_w32a6_user32dlg"],
    "W32A-7": ["patches/W32A7_gdi.patch", "test_w32a7_gdi"],
    "W32A-8": ["patches/W32A8_comctl32.patch", "test_w32a8_comctl32"],
    "W32A-9": ["patches/W32A9_registry.patch", "test_w32a9_registry"],
    "W32A-10": ["patches/W32A10_shell.patch", "test_w32a10_shell"],
    "W32A-11": ["patches/W32A11_ole.patch"],
    "W32A-12": ["patches/W32A12_winsock.patch", "test_w32a12_winsock"],
    "W32A-13": ["patches/W32A13_msvcrt.patch", "test_w32a13_msvcrt"],
    "W32A-14": ["patches/W32A14_putty.patch", "test_w32a14_putty_fixture"],
    "W32A-15": ["patches/W32A15_7zip.patch", "test_w32a15_7zip_fixture"],
    "W32A-16": ["patches/W32A16_npp.patch", "test_w32a16_npp_fixture"],
    "W32A-17": ["patches/W32A17_audacity.patch", "Outcome B"],
    "W32A-18": ["patches/W32A18_integration.patch", "byte-identical"],
}

# Outcome-B app gates (no .imports ledger, no REFUSE guard): the honest
# artefact is a .gap ledger, and the gate is green only while its receipt
# section is filled (not an AWAITING stub).  phase -> gap-ledger file.
GAP_LEDGERS = {
    "W32A-17": "audacity-3.7.5.gap",
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

    # Outcome-B gates: an app that deliberately does NOT run.  Its honesty
    # gate is the .gap ledger (which enumerates the unmet modules) plus a
    # receipt section that is filled, never an AWAITING stub.  Runs whenever
    # the heading claims green-DONE, regardless of any REFUSE machinery.
    for phase, gap in sorted(GAP_LEDGERS.items()):
        sec = phase_section(plan, phase)
        if sec is None or "\u2705 DONE" not in sec.split("\n", 1)[0]:
            continue
        gp = os.path.join(root, "w32", "app_ledger", gap)
        if not os.path.isfile(gp):
            bad("%s is DONE but gap-ledger %s is missing" % (phase, gap))
        else:
            # A gap ledger lists `<module>.<ext>  <count>  <status>` rows;
            # a genuine gap has at least one row whose status is not REAL
            # (an all-REAL ledger would not be a gap at all).
            gaps = 0
            for ln in open(gp):
                m = re.match(r"^\s+\S+\.(?:dll|drv)\s+\d+\s+(\S+)", ln)
                if m and m.group(1) != "REAL":
                    gaps += 1
            if gaps == 0:
                bad("%s is DONE but %s enumerates no unmet modules"
                    % (phase, gap))
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
