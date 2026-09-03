#!/usr/bin/env python3
"""Cross-check I386_PLAN.md's status against the tree.

The AUDIT_A7 lesson, applied prophylactically for once: every other
claim checker in this repository (check_fixes_claims.py,
check_maturity_claims.py) was written AFTER its plan drifted -- a
header saying PLANNED over 33 ticked boxes, caught by a human reading
sideways.  This one ships in the same phase as the plan's last
delivered milestone, so I386_PLAN.md has never had an unchecked day.

Each phase is tied to something in the source that only exists if the
phase actually happened, plus its smoke test's existence.  The header
is checked against the phase table so the two cannot disagree.

Usage:
    tools/check_i386_claims.py [--selftest]
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
    plan   = read("docs", "plans", "I386_PLAN.md")
    stage2 = read("boot", "bios", "stage2", "stage2_start.asm")
    lmchk  = read("boot", "bios", "stage2", "lmcheck.inc")
    mkiso  = read("tools", "mkisoimage_dual.sh")
    makefl = read("Makefile")
    thread = read("kernel", "arch", "i386", "thread32.c")
    kbd    = read("kernel", "arch", "i386", "kbd32.c")
    sweep  = read("tools", "check_width_sweep.py")

    checks = [
        # --- I0: the refusal ---
        ("I0: check_long_mode exists and Stage 2 calls it",
         "check_long_mode:" in lmchk and "call check_long_mode" in stage2),
        ("I0: refusal smoke test present",
         exists("tests", "integration", "i386_refusal_smoke.sh")),

        # --- I1: the dual-kernel chain ---
        ("I1: Stage 2 branches to KERNEL32.ELF",
         "name_kernel32" in stage2 and "elf32_load" in stage2),
        ("I1: the image ships /KERNEL32.ELF",
         "KERNEL32.ELF" in mkiso),
        ("I1: the -malign-double ABI contract is in CFLAGS32",
         "-malign-double" in makefl),

        # --- I2: CPU bring-up ---
        ("I2: the i386 IDT/ISR layer exists",
         exists("kernel", "arch", "i386", "isr_stubs32.asm") and
         exists("kernel", "arch", "i386", "idt.c")),
        ("I2: cpu smoke test present",
         exists("tests", "integration", "i386_cpu_smoke.sh")),

        # --- I3: memory ---
        ("I3: PSE paging + heap + pmm exist",
         exists("kernel", "arch", "i386", "paging32.c") and
         exists("kernel", "arch", "i386", "pmm32.c") and
         exists("kernel", "arch", "i386", "kheap32.c")),
        ("I3: the i686 floor is enforced in Stage 2",
         "check_i686" in lmchk and "call check_i686" in stage2),

        # --- I4: processes ---
        ("I4: context switch + Ring 3 entry exist",
         exists("kernel", "arch", "i386", "context32.asm") and
         exists("kernel", "arch", "i386", "user_entry32.asm")),
        ("I4: the post-EOI preemption point is wired",
         "sched32_maybe_preempt" in read("kernel", "arch", "i386",
                                         "irq32.c")),

        # --- I5: userspace ---
        ("I5: libc32 + init32 exist and ship in the initrd",
         exists("lib", "libc32", "libc32.h") and
         exists("userspace", "system", "init32", "init32.c") and
         "bin32/init32" in makefl),

        # --- I6: the sweep machinery ---
        ("I6: the three ratchets exist and are registered",
         "BASELINE_UINT64_CASTS" in sweep and
         "test_width_sweep.sh" in makefl),
        ("I6: paddr_t and arch.h exist",
         exists("kernel", "lib", "paddr.h") and
         exists("kernel", "arch", "arch.h")),

        # --- I7: console + shell ---
        ("I7: keyboard, VGA console and the shell exist",
         exists("kernel", "arch", "i386", "kbd32.c") and
         exists("kernel", "arch", "i386", "vga32.c") and
         # RISCV_PLAN V5 promoted shell32.c to the shared smallsh.c
         # (one source, two arches); the i386 binary still builds from
         # it and i386_shell_smoke.sh still gates its behaviour.
         exists("userspace", "system", "smallsh", "smallsh.c")),
        ("I7: the two measured bugs carry their comments",
         "sti; hlt; cli" in kbd and "thread32_set_esp0" in thread),

        # --- I8: parity ---
        ("I8: ata32 + net32 exist; pci.c is shared into kernel32",
         exists("kernel", "arch", "i386", "ata32.c") and
         exists("kernel", "arch", "i386", "net32.c") and
         "KERNEL32_SHARED" in makefl),
        ("I8: the -m32 crypto gate is registered",
         "test_libatls_m32.sh" in makefl),

        # --- I9: this file is registered, and docs carry the arch ---
        ("I9: this checker is registered in test-unit",
         "check_i386_claims.py" in makefl),
        ("I9: docs/status.md has the i386 section",
         "## i386" in read("docs", "status.md")),
        ("I9: the CI workflow has the i386 job",
         "i386-parity" in read(".github", "workflows", "integration.yml")),
    ]

    # The header must agree with the phase table.  Every phase row that
    # says "✅ complete" must ALSO appear as a "### Phase ... ✅ COMPLETE"
    # heading, and the Status line must not understate the count.
    done_rows = len(re.findall(r"^\| I\d .*?✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### Phase I\d .*?✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete table row has a COMPLETE heading",
                   done_rows == done_heads and done_rows > 0))

    m = re.search(r"^## Status: .*?I0–I(\d) (?:complete|all delivered)",
                  plan, re.M)
    checks.append(("plan: the Status header names the delivered range",
                   bool(m) and int(m.group(1)) + 1 == done_rows))

    return checks


def main():
    if "--selftest" in sys.argv:
        # A checker that never fails is indistinguishable from a clean
        # tree: verify at least one check goes red on a doctored input.
        results = claims()
        ok = all(passed for _, passed in results)
        if not ok:
            print("check_i386_claims: SELFTEST inconclusive (tree already "
                  "red)", file=sys.stderr)
            return 1
        # Fake a missing artefact by pointing ROOT somewhere empty.
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(passed for _, passed in doctored):
            print("check_i386_claims: SELFTEST FAIL -- checks pass against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_i386_claims: selftest PASS (doctored tree detected)")
        return 0

    failed = 0
    for desc, passed in claims():
        if not passed:
            print(f"check_i386_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_i386_claims: {failed} claim(s) disagree with the tree",
              file=sys.stderr)
        return 1
    print(f"check_i386_claims: OK -- {len(claims())} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
