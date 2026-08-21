#!/usr/bin/env python3
"""Cross-check HW_PLAN.md's status against the tree.

Fifth checker in the D8 family (i386, riscv, arm64, opt came first),
shipped in H0 for the family's standing reason: a plan checked from
birth cannot drift at all.

Same structure as its siblings: each claim ties a phase to an
artefact that only exists if the phase happened; the Status header is
checked against the phase table; --selftest proves the checks can
fail (against a doctored ROOT).

Usage:
    tools/check_hw_claims.py [--selftest]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def exists(*parts):
    return os.path.exists(os.path.join(ROOT, *parts))


def claims():
    plan   = read("HW_PLAN.md")
    makefl = read("Makefile")

    checks = [
        # --- H0: the rig ---
        ("H0: both tenant membenches exist and bench the LINKED "
         "string ops (not a private copy)",
         exists("kernel", "arch", "riscv64", "membench_rv.c") and
         exists("kernel", "arch", "aarch64", "membench_a64.c") and
         'kernel/lib/string.h' in read("kernel", "arch", "riscv64",
                                       "membench_rv.c") and
         'kernel/lib/string.h' in read("kernel", "arch", "aarch64",
                                       "membench_a64.c")),
        ("H0: every bench pass is VERIFIED (a fast wrong copy must "
         "fail loudly)",
         "FAIL: memcpy verify" in read("kernel", "arch", "riscv64",
                                       "membench_rv.c") and
         "FAIL: memcpy verify" in read("kernel", "arch", "aarch64",
                                       "membench_a64.c")),
        ("H0: the rv64 kernel ADOPTED kernel/lib/string.c (the OPT "
         "\u00a77 residue line, paid)",
         re.search(r"KERNELRV_SHARED :=.*kernel/lib/string\.c",
                   makefl) is not None),
        ("H0: the x86 boot prints the feature receipt and the "
         "IA32_PAT readback (from the arch tree -- ratchet 2's "
         "measured catch)",
         "features: pat=%d pcid=%d invpcid=%d erms=%d" in
         read("kernel", "arch", "x86_64", "diagnostics.c") and
         "IA32_PAT = 0x%016llx" in
         read("kernel", "arch", "x86_64", "diagnostics.c") and
         "diag_cpu_feature_receipts" in read("kernel", "kernel.c")),
        ("H0: the smokes pin the bench tables and the receipts",
         "memcpy 64KiB" in read("tests", "integration",
                                "rv_boot_smoke.sh") and
         "memcpy 64KiB" in read("tests", "integration",
                                "a64_boot_smoke.sh") and
         "IA32_PAT" in read("tests", "integration", "cases",
                            "test_perf_smoke.sh")),
        ("H0: the plan records the measured surprises (unrolled "
         "byte loops; PCID absent under -cpu max TCG)",
         "clang at -O2 already" in plan and
         "does not expose PCID" in plan),

        # --- H1: word-wide portable string ops ---
        ("H1: the word bodies exist with the strict-align-proof "
         "spelling (may_alias word type, co-alignment fork)",
         "may_alias" in read("kernel", "lib", "string.c") and
         "sw_word" in read("kernel", "lib", "string.c") and
         "Co-aligned" in read("kernel", "lib", "string.c")),
        ("H1: the host suite tortures the new seams (offset x size "
         "x overlap sweep)",
         "test_word_sweep_memcpy" in read("tests", "unit",
                                          "test_string.c") and
         "test_word_sweep_memmove_overlap" in read("tests", "unit",
                                                   "test_string.c")),
        ("H1: the H0 attribution correction is recorded (the O6 "
         "tradition -- the number was real, the attribution was not)",
         "the attribution was not" in plan and
         "NOT unrolled" in plan),
        ("H1: the numbers table carries the measured win",
         "2449" in plan and "1964" in plan and "3178" in plan),
    ]

    # Structural: the Status header and the phase table must agree
    # (armed at H0, before most rows are green -- the D8 shape).
    PHASE_ORDER = ["H0", "H1", "H2", "H3", "H4", "H5"]
    done_rows = len(re.findall(r"^\| H\d .*?✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### Phase H\d .*?✅ COMPLETE",
                                plan, re.M))
    checks.append(("plan: every complete table row has a COMPLETE heading",
                   done_rows == done_heads))

    status_ok = False
    if re.search(r"^## Status: PLANNED", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"H0(?:–(H\d))? complete", plan)
        if m:
            label = m.group(1) if m.group(1) else "H0"
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
        if not all(passed for _, passed in results):
            print("check_hw_claims: SELFTEST inconclusive (tree already "
                  "red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(passed for _, passed in doctored):
            print("check_hw_claims: SELFTEST FAIL -- checks pass against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_hw_claims: selftest PASS (doctored tree detected)")
        return 0

    failed = 0
    for desc, passed in claims():
        if not passed:
            print(f"check_hw_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_hw_claims: {failed} claim(s) disagree with the tree",
              file=sys.stderr)
        return 1
    print(f"check_hw_claims: OK -- {len(claims())} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
