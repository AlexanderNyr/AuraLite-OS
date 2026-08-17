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
