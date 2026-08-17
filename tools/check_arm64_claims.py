#!/usr/bin/env python3
"""Cross-check ARM64_PLAN.md's status against the tree.

Decision D8 of the plan it checks -- now a tradition, third checker in
the family: check_i386_claims.py shipped with its plan's completion,
check_riscv_claims.py shipped in V0, and this one ships in A0 for the
same reason.  A plan checked from birth cannot drift at all.

Same structure as its predecessors: each claim ties a phase to an
artefact that only exists if the phase happened; the plan's Status
header is checked against its own phase table; --selftest proves the
checks can fail (against a doctored ROOT).

Usage:
    tools/check_arm64_claims.py [--selftest]
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
    plan   = read("ARM64_PLAN.md")
    makefl = read("Makefile")
    boots  = read("kernel", "arch", "aarch64", "boot.S")
    ld     = read("kernel", "arch", "aarch64", "kernela64.ld")
    mainc  = read("kernel", "arch", "aarch64", "main_a64.c")
    psci   = read("kernel", "arch", "aarch64", "psci.c")
    smoke  = read("tests", "integration", "a64_boot_smoke.sh")

    checks = [
        # --- A0: toolchain gates + the EL1 stub ---
        ("A0: the a64 entry, PL011/PSCI layers and linker script exist",
         exists("kernel", "arch", "aarch64", "boot.S") and
         exists("kernel", "arch", "aarch64", "pl011.c") and
         exists("kernel", "arch", "aarch64", "psci.c") and
         exists("kernel", "arch", "aarch64", "kernela64.ld")),
        ("A0: the Makefile has the kernela64 target with the a64 flags",
         "kernela64" in makefl and "aarch64-unknown-none-elf" in makefl and
         "aarch64linux" in makefl),
        ("A0: -mstrict-align is on (the pre-MMU Device-memory fact)",
         "-mstrict-align" in makefl),
        ("A0: the EL1 check refuses non-EL1 entry (D1)",
         "CurrentEL" in boots and "refusing to boot" in boots),
        ("A0: .text.boot placed first (the shared four-kernel discipline)",
         ".text.boot" in ld and ".text.boot" in boots),
        ("A0: the DTB is probed at the RAM base, not taken from x0 "
         "(the measured Fact 2 trap)",
         "0x40000000" in mainc and "fdt_magic_at_ram_base" in mainc and
         "not the DTB pointer" in mainc),
        ("A0: PSCI SYSTEM_OFF over hvc ends every healthy boot",
         "0x84000008" in psci and "hvc #0" in psci and
         "psci_system_off" in mainc),
        ("A0: boot smoke covers banner, EL1, x0=0, magic, power-off, "
         "-smp 4 and the EL2 refusal",
         "CurrentEL: EL1" in smoke and
         "x0 at entry: 0x0000000000000000" in smoke and
         "virtualization=on" in smoke),
        ("A0: this checker is registered in test-unit",
         "check_arm64_claims.py" in makefl),
    ]

    # --- A1: boot_info_t from the Device Tree, the walker promoted ---
    fdt_c  = read("kernel", "dt", "fdt.c")
    fdt_h  = read("kernel", "dt", "fdt.h")
    checks += [
        ("A1: the walker lives in kernel/dt/ and the riscv copy is GONE",
         exists("kernel", "dt", "fdt.c") and
         not exists("kernel", "arch", "riscv64", "fdt.c") and
         not exists("kernel", "arch", "riscv64", "fdt.h")),
        ("A1: BOTH kernels compile the shared walker (single-object "
         "promotion, not a fork)",
         "KERNELRV_SHARED := kernel/net/miniproto.c kernel/dt/fdt.c"
         in makefl and
         "KERNELA64_SHARED := kernel/dt/fdt.c" in makefl),
        ("A1: interrupt normalisation is central -- SPI+32/PPI+16 in "
         "the walker, never in a driver",
         "nr + 32" in fdt_c and "nr + 16" in fdt_c and
         "NORMALISED" in fdt_h),
        ("A1: the per-depth device-state lesson is recorded (the GIC's "
         "v2m child wiped the scalar)",
         "v2m" in fdt_c and "Per-DEPTH, not per-walk" in fdt_c and
         "ndev[depth]" in fdt_c),
        ("A1: the PSCI conduit is asserted against the tree, not assumed",
         "psci_method" in fdt_h and
         "FDT_PSCI_HVC" in read("kernel", "arch", "aarch64", "main_a64.c")),
        ("A1: both arches define the dt_phys_to_virt contract",
         "dt_phys_to_virt" in read("kernel", "arch", "riscv64",
                                   "main_rv.c") and
         "dt_phys_to_virt" in read("kernel", "arch", "aarch64",
                                   "main_a64.c")),
        ("A1: the smoke asserts normalised INTIDs and the shared-walker "
         "boot_info lines",
         "irq 33" in smoke and "virtio-mmio windows: 32" in smoke and
         "handoff magic OK" in smoke),
    ]

    # --- A2: exceptions, the generic timer, GICv2 ---
    vecs  = read("kernel", "arch", "aarch64", "vectors.S")
    trapc = read("kernel", "arch", "aarch64", "trap_a64.c")
    gicc_ = read("kernel", "arch", "aarch64", "gic.c")
    checks += [
        ("A2: the 16x128 vector table with one shared spill path",
         ".balign 2048" in vecs and ".balign 128" in vecs and
         "a64_trap_common" in vecs and "eret" in vecs),
        ("A2: the tag formula is position-major and its lesson is "
         "recorded (one formula, used twice, or none)",
         "origin * 4 + \\kind" in vecs and
         "dispatched as if it were another row" in vecs),
        ("A2: SPSel declared, not inherited -- the measured QEMU "
         "SPSel=0 entry fact lives in boot.S",
         "msr   spsel, #1" in boots and "MEASURED in A2" in boots),
        ("A2: ESR decode names the classes; unexpected rows halt "
         "loudly by vector name",
         "ec_name" in trapc and "UNEXPECTED vector row" in trapc and
         "kind_names" in trapc),
        ("A2: the timer is CNTFRQ-derived (a register, not a DTB "
         "field) with the TVAL re-arm property named",
         "cntfrq_read" in trapc and "TICK_HZ" in trapc and
         "un-asserts the line" in trapc),
        ("A2: the timer INTID is arithmetic with a paper trail, "
         "never a bare 27",
         "11u + 16u" in trapc),
        ("A2: the GIC completes even unhandled claims (no stuck "
         "gateway -- the plic_dispatch rule)",
         "EOIR" in gicc_ and "even for a line without" in gicc_),
        ("A2: the smoke gates isr/timer/gic/rng PASS lines",
         "resumed" in smoke and "ticks observed at 100 Hz" in smoke and
         "claim/complete" in smoke and "jitter" in smoke),
        ("A2: the alignment premise is probed, not trusted "
         "(-mstrict-align has a measured reason)",
         "trap_alignment_probe_a64" in trapc and
         "unaligned load faulted" in smoke),
    ]

    # Structural: the Status header and the phase table must agree.
    # Installed in A0, before a single row is green -- the D8 shape.
    done_rows = len(re.findall(r"^\| A\d .*?✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### Phase A\d .*?✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete table row has a COMPLETE heading",
                   done_rows == done_heads))

    status_ok = False
    if re.search(r"^## Status: PLANNED", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"A0(?:–A(\d))? complete", plan)
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
            print("check_arm64_claims: SELFTEST inconclusive (tree already "
                  "red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(passed for _, passed in doctored):
            print("check_arm64_claims: SELFTEST FAIL -- checks pass against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_arm64_claims: selftest PASS (doctored tree detected)")
        return 0

    failed = 0
    for desc, passed in claims():
        if not passed:
            print(f"check_arm64_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_arm64_claims: {failed} claim(s) disagree with the tree",
              file=sys.stderr)
        return 1
    print(f"check_arm64_claims: OK -- {len(claims())} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
