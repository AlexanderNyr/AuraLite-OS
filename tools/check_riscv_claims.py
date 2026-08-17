#!/usr/bin/env python3
"""Cross-check RISCV_PLAN.md's status against the tree.

Decision D8 of the plan it checks: check_i386_claims.py shipped in the
same phase as its plan's COMPLETION and was proud of never having an
unchecked day; this one ships in V0, the plan's FIRST delivered phase,
and grows a claim per phase.  A plan checked from birth cannot drift
at all.

Same structure as its two predecessors: each claim ties a phase to an
artefact that only exists if the phase happened; the plan's Status
header is checked against its own phase table; --selftest proves the
checks can fail (against a doctored ROOT).

Usage:
    tools/check_riscv_claims.py [--selftest]
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
    plan   = read("RISCV_PLAN.md")
    makefl = read("Makefile")
    boots  = read("kernel", "arch", "riscv64", "boot.S")
    ld     = read("kernel", "arch", "riscv64", "kernelrv.ld")

    checks = [
        # --- V0: toolchain gates + the S-mode stub ---
        ("V0: the rv64 entry, SBI layer and linker script exist",
         exists("kernel", "arch", "riscv64", "boot.S") and
         exists("kernel", "arch", "riscv64", "sbi.c") and
         exists("kernel", "arch", "riscv64", "kernelrv.ld")),
        ("V0: the Makefile has the kernelrv target with the rv64 flags",
         "kernelrv" in makefl and "-march=rv64gc" in makefl and
         "elf64lriscv" in makefl),
        ("V0: the hart lottery parks secondary harts",
         "amoswap" in boots and "hart_lottery" in boots),
        ("V0: the payload-base-not-e_entry fact is enforced in the "
         "linker script",
         ".text.boot" in ld and ".text.boot" in boots),
        ("V0: boot smoke test present",
         exists("tests", "integration", "rv_boot_smoke.sh")),
        ("V0: this checker is registered in test-unit",
         "check_riscv_claims.py" in makefl),
    ]

    # --- V1: boot_info_t from the Device Tree ---
    fdt   = read("kernel", "arch", "riscv64", "fdt.c")
    sweep = read("tests", "unit", "test_width_sweep.sh")
    smoke = read("tests", "integration", "rv_boot_smoke.sh")
    checks += [
        ("V1: the FDT shim exists and reads big-endian only",
         exists("kernel", "arch", "riscv64", "fdt.c") and
         "be32" in fdt and "be64" in fdt),
        ("V1: magic is written LAST (the partially-filled rule)",
         # the assignment must be the file's final act before return 0
         "bi->magic = BOOT_MAGIC" in fdt and
         fdt.rindex("bi->magic = BOOT_MAGIC") > fdt.rindex("hhdm_offset")),
        ("V1: hhdm carries the D3 Sv39 constant",
         "0xFFFFFFC000000000" in fdt),
        ("V1: the width contract compiles at the third width",
         "--target=riscv64" in sweep and "biw_rv64" in sweep),
        ("V1: the smoke test asserts the handoff and the initrd path",
         "handoff magic OK" in smoke and "initrd" in smoke),
    ]

    # --- V2: traps, timer, PLIC ---
    trapc = read("kernel", "arch", "riscv64", "trap.c")
    trape = read("kernel", "arch", "riscv64", "trapentry.S")
    plicc = read("kernel", "arch", "riscv64", "plic.c")
    checks += [
        ("V2: the trap entry saves the full frame and sret-returns",
         "rv_trap_vector" in trape and "sret" in trape and
         "sd    x31" in trape),
        ("V2: scause decode has the 16 named exception codes",
         exists("kernel", "arch", "riscv64", "trap.c") and
         "Illegal Instruction" in trapc and
         "Store/AMO Page Fault" in trapc),
        ("V2: FIX_R0 discipline -- cpu= in the frame dump",
         "cpu=hart" in trapc),
        ("V2: the timer re-arms via SBI (one-shot contract)",
         "sbi_set_timer" in trapc),
        ("V2: PLIC completes even unhandled claims (no stuck gateway)",
         "*reg32(PLIC_CLAIM(s_ctx)) = irq" in plicc),
        ("V2: the smoke test gates isr/timer/plic PASS lines",
         "resumed" in smoke and "ticks observed at 100 Hz" in smoke and
         "claim/complete" in smoke),
    ]

    # --- V3: Sv39, PMM, heap, W^X ---
    pagec = read("kernel", "arch", "riscv64", "paging_rv.c")
    pmmc  = read("kernel", "arch", "riscv64", "pmm_rv.c")
    ldscr = read("kernel", "arch", "riscv64", "kernelrv.ld")
    boots2 = read("kernel", "arch", "riscv64", "boot.S")
    checks += [
        ("V3: the linker script is higher-half (HHDM VMA, physical LMA)",
         "HHDM = 0xFFFFFFC000000000" in ldscr and "AT(ADDR(" in ldscr),
        ("V3: boot.S turns Sv39 on before any C runs",
         "csrw  satp" in boots2 and "early_root" in boots2),
        ("V3: the PMM is bitmap.h's third consumer, header unedited",
         'include "kernel/lib/bitmap.h"' in pmmc and
         "pmm32" not in read("kernel", "lib", "bitmap.h")),
        ("V3: no W+X PTE is ever built (grep the flag combos)",
         # pte_leaf()'s R|W|X MASK is a test, not a mapping -- excluded.
         "PTE_R | PTE_X" in pagec and "PTE_R | PTE_W" in pagec and
         "PTE_W | PTE_X" not in
         "".join(l for l in pagec.splitlines() if "pte_leaf" not in l)),
        ("V3: the vmm self-test probes all three faults",
         "probe_store_text" in pagec and "probe_exec_data" in pagec and
         "probe_load_identity" in pagec),
        ("V3: the smoke test asserts higher-half sepc and the W^X pair",
         "sepc=0xffffffc0" in smoke and
         "execute-from-data faulted" in smoke),
    ]

    # Structural: the Status header and the phase table must agree.
    # While phases are pending the header says PLANNED/IN PROGRESS and
    # complete-rows == complete-headings; when it claims a range, the
    # range must match the table (the same checks the i386 checker
    # ended with -- installed here from day one).
    done_rows = len(re.findall(r"^\| V\d .*?✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### Phase V\d .*?✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete table row has a COMPLETE heading",
                   done_rows == done_heads))

    status_ok = False
    if re.search(r"^## Status: PLANNED", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"V0(?:–V(\d))? complete", plan)
        claimed = (int(m.group(1)) + 1 if m and m.group(1) else 1) if m else 0
        status_ok = bool(m) and claimed == done_rows
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == 10)
    checks.append(("plan: the Status header agrees with the table",
                   status_ok))

    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(passed for _, passed in results):
            print("check_riscv_claims: SELFTEST inconclusive (tree already "
                  "red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(passed for _, passed in doctored):
            print("check_riscv_claims: SELFTEST FAIL -- checks pass against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_riscv_claims: selftest PASS (doctored tree detected)")
        return 0

    failed = 0
    for desc, passed in claims():
        if not passed:
            print(f"check_riscv_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_riscv_claims: {failed} claim(s) disagree with the tree",
              file=sys.stderr)
        return 1
    print(f"check_riscv_claims: OK -- {len(claims())} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
