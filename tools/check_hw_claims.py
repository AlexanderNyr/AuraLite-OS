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
    plan   = read("docs", "plans", "HW_PLAN.md")
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

        # --- H2: ERMSB receipt + crossover ---
        ("H2: the crossover is runtime and CPUID-fed (string_fast_init "
         "reads ERMS, 64 -> 0)",
         exists("kernel", "arch", "x86_64", "string_fast.h") and
         "string_fast_init" in read("kernel", "arch", "x86_64",
                                    "string_fast.c") and
         "small_n = 0" in read("kernel", "arch", "x86_64",
                               "string_fast.c") and
         "string_fast_init" in read("kernel", "arch", "x86_64",
                                    "diagnostics.c")),
        ("H2: both lanes pin their threshold line (qemu64 keeps 64, "
         "-cpu max drops to 0)",
         "crossover: 64 (no ERMS)" in read("tests", "integration",
                                           "cases",
                                           "test_perf_smoke.sh") and
         "crossover: 0 (ERMS fast-string)" in
         read("tests", "integration", "x86_cpumax_smoke.sh")),
        ("H2: the -cpu max shell-banner oddity is recorded as "
         "pre-existing (control run), not hidden",
         "pre-H2" in read("tests", "integration",
                          "x86_cpumax_smoke.sh") and
         "recorded here as `-cpu max` residue" in plan),

        # --- H3: PAT + WC framebuffer ---
        ("H3: PAT PA4=WC is programmed in the per-CPU features path "
         "(BSP + every AP), readback printed",
         "PA4 := WC" in read("kernel", "arch", "x86_64", "paging.c") and
         "PA4=WC (readback" in read("kernel", "arch", "x86_64",
                                    "paging.c") and
         "paging_cpu_features_init" in read("kernel", "arch", "x86_64",
                                            "smp.c")),
        ("H3: the fb remap exists, splits huge pages exactly, and "
         "prints the DECODED first PTE",
         "paging_fb_set_wc" in read("kernel", "arch", "x86_64",
                                    "paging.c") and
         "fb: WC via PAT4" in read("kernel", "arch", "x86_64",
                                   "paging.c") and
         "paging_fb_set_wc" in read("kernel", "kernel.c")),
        ("H3: both lanes pinned -- the UEFI WC decode and the BIOS "
         "honest skip",
         "fb: WC via PAT4 (1000 pages" in
         read("tests", "integration", "cases",
              "test_gui_dirty_uefi.sh") and
         "fb: none present; WC remap skipped" in
         read("tests", "integration", "cases", "test_perf_smoke.sh")),

        # --- H4: PCID -- the written design + the deferral protocol ---
        ("H4: the design is written with named decisions, reviewed "
         "against the O5 shootdown code",
         "D-PCID-1" in plan and "D-PCID-4" in plan and
         "D-PCID-5" in plan and
         "tlb_shootdown.c" in plan),
        ("H4: the receipt slots are reserved at zero in /proc/perf",
         "PERF_CR3_NOFLUSH_SWITCHES" in read("kernel", "lib",
                                             "perfstat.h") and
         "cr3_noflush_switches" in read("kernel", "lib",
                                        "perfstat.c") and
         "pcid_generation_wraps" in read("kernel", "lib",
                                         "perfstat.c")),
        ("H4->R11: the smoke SELF-SELECTS on the feature bit (the "
         "re-open gate FIRED — RESIDUE R11 implemented D-PCID-1..4; "
         "pcid=0 lanes still pin both counters at zero, pcid=1 lanes "
         "demand movement)",
         "cr3_noflush_switches stays zero" in
         read("tests", "integration", "cases",
              "test_perf_smoke.sh") and
         "cr3_noflush_switches [1-9]" in
         read("tests", "integration", "cases",
              "test_perf_smoke.sh") and
         "pcid_cr3_for" in read("kernel", "arch", "x86_64", "pcid.c")),

        # --- H5: close-out ---
        ("H5: docs/status.md carries the HW rows with the TCG/metal "
         "split stated",
         "Real-hardware package + string-ops parity" in
         read("docs", "status.md") and
         "TCG-half" in read("docs", "status.md")),
        ("H5: the -cpu max lane runs in CI",
         "x86_cpumax_smoke.sh" in read(".github", "workflows",
                                       "integration.yml")),
        ("H5: the receipt protocol is paste-the-line-back concrete",
         "paste" in plan and "run membench" in plan and
         "D-PCID-5 trigger" in plan),
        ("H5: the plan records what its planner did not expect (the "
         "series' yield)",
         "did not expect" in plan and
         "doing its job" in plan),
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
