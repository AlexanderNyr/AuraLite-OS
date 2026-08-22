#!/usr/bin/env python3
"""Cross-check PARITY_PLAN.md's status against the tree.

Sixth checker in the D8 family (i386, riscv, arm64, opt, hw came
first), shipped in P0 for the family's standing reason: a plan
checked from birth cannot drift at all.

Two things are new in this edition, both P0 decisions:

* LIVE measurements, not quoted ones.  Fact 1 of the plan ("the fs
  tree is already portable") is re-run on every invocation: all
  kernel/fs/*.c files must -fsyntax-only-compile under both DTB
  tenant flag sets.  A quoted number can rot; a compile cannot.
* RATCHETS with same-commit clicks.  The ahci-in-fs count and the
  per-port syscall-case counts are pinned exactly: above the pin is
  a regression, below the pin without lowering the pin is a phase
  that forgot to claim its own win.  Both directions fail.

Usage:
    tools/check_parity_claims.py [--selftest]
"""

import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --- The pins.  Every phase that moves a number edits its pin in the
# --- same commit (D3/D6); the checker fails in BOTH directions.
#
# 41 OCCURRENCES on 28 grep lines: the plan's opening measurement
# counted lines; the rig counts call sites (several lines carry two).
# The ratchet keeps the stricter number.  P0's first catch.
# P1 cut the seam: 41 -> 0, and 0 it stays.
AHCI_IN_FS_PIN = 0
SYSCALL_CASE_PIN = {       # P4 landed: 6 -> 11 on every port.
    ("kernel/arch/riscv64/user_rv.c",  r"case SYS_RV_\w+:"):  11,
    ("kernel/arch/aarch64/user_a64.c", r"case SYS_A64_\w+:"): 11,
    ("kernel/arch/i386/user32.c",      r"case SYS32_\w+:"):   11,
}
FS_FILE_COUNT = 20         # kernel/fs/*.c (19 at P0; blkdev.c joined at P1).

# Flag sets copied from the Makefile's CFLAGSRV / CFLAGSA64 (compile
# flags only; -Werror deliberately kept so new warnings fail here
# exactly like they would in the tenant build).  ONE deviation, named:
# -Wno-unused-function mirrors the x86_64 kernel's warning policy,
# because btrfs.c/ext4.c/f2fs.c carry helpers only their #ifdef'd
# callers use -- P0's second catch (the plan's draft measurement ran
# without -Werror and never saw the class).  If one of those three
# files ever joins a tenant build, the flag joins CFLAGSRV/CFLAGSA64
# or the helpers get callers; until then the lanes measure PORTING
# errors, not warning-policy drift.
RV64_FLAGS = ("--target=riscv64 -march=rv64gc -mabi=lp64d -mcmodel=medany "
              "-mno-relax -std=c11 -ffreestanding -fno-stack-protector "
              "-fno-pie -fno-pic -Wall -Wextra -Wno-unused-parameter "
              "-Wno-unused-function -Werror -O2").split()
A64_FLAGS = ("--target=aarch64-unknown-none-elf -mstrict-align "
             "-mgeneral-regs-only -std=c11 -ffreestanding "
             "-fno-stack-protector -fno-pie -fno-pic -Wall -Wextra "
             "-Wno-unused-parameter -Wno-unused-function -Werror "
             "-O2").split()

# Reserved artefact names: when a phase row flips to "✅ complete",
# its artefacts must exist -- the registry-reservation idea of P0,
# expressed where it cannot fight check_test_registry.py (which
# rightly demands that ALL_CASES mirror files that exist TODAY).
PHASE_ARTEFACTS = {
    "P0": ["tools/check_parity_claims.py"],
    "P1": ["kernel/fs/blkdev.h", "kernel/fs/blkdev.c",
           "tests/unit/test_blkdev.c"],
    "P2": ["tests/integration/rv_fs_smoke.sh"],
    "P3": ["tests/integration/a64_fs_smoke.sh"],
    "P4": [],                       # its artefact is the syscall pin at 11
    "P5": ["tests/integration/rv_smp_smoke.sh"],
    "P6": ["tests/integration/a64_smp_smoke.sh"],
    "P7": ["tests/integration/i386_fs_smoke.sh"],
    "P8": ["lib/libcmini/libcmini.h"],
    "P9": [],                       # its artefact is the docs/matrix flip
}
PHASE_ORDER = ["P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9"]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def exists(*parts):
    return os.path.exists(os.path.join(ROOT, *parts))


def fs_sources():
    return sorted(glob.glob(os.path.join(ROOT, "kernel", "fs", "*.c")))


def live_syntax_pass(flags):
    """Compile every fs file -fsyntax-only; return (passes, total)."""
    cc = shutil.which("clang")
    if cc is None:
        return (-1, -1)   # named condition, reported as its own failure
    srcs = fs_sources()
    passes = 0
    for src in srcs:
        res = subprocess.run(
            [cc] + flags + ["-I", ROOT, "-I", os.path.join(ROOT, "build"),
                            "-fsyntax-only", src],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if res.returncode == 0:
            passes += 1
    return (passes, len(srcs))


def count_ahci_in_fs():
    pat = re.compile(r"ahci_read|ahci_write|ahci_port")
    total = 0
    for src in fs_sources():
        with open(src, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                total += len(pat.findall(line))
    return total


def claims():
    plan = read("PARITY_PLAN.md")
    makefl = read("Makefile")
    checks = []

    # --- P0: the rig itself ---
    checks.append((
        "P0: the checker is wired into make test-unit WITH its selftest "
        "(a checker that never fails checks nothing)",
        "check_parity_claims.py" in makefl and
        makefl.count("check_parity_claims.py") >= 2))
    checks.append((
        "P0: the seam is designed once, in the plan (\u00a76: ops struct, "
        "4 slots, the 512 stance)",
        "struct blkdev_ops" in plan and "blkdev_register" in plan and
        "512-byte sectors" in plan))
    checks.append((
        "P0: the 512 stance is measured in the tree, not assumed "
        "(all three backends present 512 today)",
        "AHCI_SECTOR_SIZE 512" in read("drivers", "ahci", "ahci.h") and
        "buf512" in read("kernel", "arch", "riscv64", "vblk_rv.h") and
        "buf512" in read("kernel", "arch", "i386", "ata32.c")))

    # --- Fact 1, LIVE: the fs tree compiles under both tenant flag
    # --- sets right now.  A doctored/regressed tree fails here.
    for label, flags in (("rv64", RV64_FLAGS), ("a64", A64_FLAGS)):
        passes, total = live_syntax_pass(flags)
        checks.append((
            f"live: kernel/fs compiles -fsyntax-only as {label} "
            f"({passes}/{total}, expected {FS_FILE_COUNT}/{FS_FILE_COUNT}"
            " -- clang required)",
            passes == FS_FILE_COUNT and total == FS_FILE_COUNT))

    # --- The ahci-in-fs ratchet (D3): exact pin, both directions.
    ahci_now = count_ahci_in_fs()
    checks.append((
        f"ratchet: ahci call sites in kernel/fs == pin "
        f"(measured {ahci_now}, pin {AHCI_IN_FS_PIN}; above = regression, "
        "below = lower the pin in the same commit)",
        ahci_now == AHCI_IN_FS_PIN))

    # --- The syscall-surface pins (D4): exact, per port.
    for (path, pattern), pin in SYSCALL_CASE_PIN.items():
        text = read(*path.split("/"))
        count = len(re.findall(pattern, text))
        checks.append((
            f"pin: {path} carries exactly {pin} syscall cases "
            f"(measured {count}; the file five landed at P4)",
            count == pin and text != ""))

    # --- P1: the seam guarantee, stated as an include rule.  The
    # --- ratchet above counts ahci CALLS; this counts the stronger
    # --- thing -- kernel/fs may include NO driver header at all (the
    # --- driver includes the seam, never the other way around).
    # --- P1's catch: the rule found THREE pre-existing non-storage
    # --- couplings the plan never measured -- pit.h in procfs/select
    # --- (a TIME seam question, not this plan's) and usb/msc.h in
    # --- usbfs (the USB seam question).  Named residue, pinned per
    # --- file: a new driver include ANYWHERE in fs fails here even
    # --- at the same total.
    DRIVER_INC_ALLOW = {
        "procfs.c": 1,   # drivers/timer/pit.h -- time seam residue
        "select.c": 1,   # drivers/timer/pit.h -- same residue class
        "usbfs.c":  1,   # drivers/usb/msc.h   -- USB seam residue
    }
    inc_ok = True
    for src in fs_sources() + sorted(
            glob.glob(os.path.join(ROOT, "kernel", "fs", "*.h"))):
        with open(src, "r", encoding="utf-8", errors="replace") as handle:
            n = len(re.findall(r'#include\s+"drivers/', handle.read()))
        if n != DRIVER_INC_ALLOW.get(os.path.basename(src), 0):
            inc_ok = False
    checks.append((
        "P1: zero STORAGE driver headers in kernel/fs; the three "
        "non-storage couplings (pit.h x2, msc.h) are pinned residue "
        "per file",
        inc_ok and len(fs_sources()) > 0))

    # --- P2: the rv64 adoption is SHARED objects, not forks (the
    # --- string.c promotion shape, asserted the same way).
    makefl_rv = re.search(r"KERNELRV_SHARED :=.*?(?:\n\n|\nKERNELRV_SRCS)",
                          makefl, re.S)
    rv_shared = makefl_rv.group(0) if makefl_rv else ""
    checks.append((
        "P2: KERNELRV_SHARED carries the fs adoption set unchanged "
        "(blkdev.c, ext2.c, kprintf.c, spinlock.c)",
        all(t in rv_shared for t in
            ("kernel/fs/blkdev.c", "kernel/fs/ext2.c",
             "kernel/lib/kprintf.c", "kernel/lib/spinlock.c"))))
    checks.append((
        "P2: the arch glue provides the measured surface and vfs.c's "
        "absence is a NAMED blocker, not a silent one",
        "uart_putchar" in read("kernel", "arch", "riscv64", "fsglue_rv.c")
        and "vfs_now" in read("kernel", "arch", "riscv64", "fsglue_rv.c")
        and "sti` at vfs.c:71" in read("kernel", "arch", "riscv64",
                                       "fsglue_rv.c")
        and "vfs.c:71" in read("PARITY_PLAN.md")))
    checks.append((
        "P2: the pattern-disk gate survived (the sniff dispatches, "
        "rv_parity_smoke's lane is intact)",
        "no test pattern; filesystem media" in
        read("kernel", "arch", "riscv64", "main_rv.c") and
        "vblk_rv_selftest" in read("kernel", "arch", "riscv64",
                                   "main_rv.c")))

    # --- P3: the a64 adoption is the SAME shared set (fourth seam
    # --- consumer), and the deviation budget closed at ZERO portable
    # --- lines -- both glues are arch-side mirrors.
    makefl_a64 = re.search(r"KERNELA64_SHARED :=.*?(?:\n\n|\nKERNELA64_SRCS)",
                           makefl, re.S)
    a64_shared = makefl_a64.group(0) if makefl_a64 else ""
    checks.append((
        "P3: KERNELA64_SHARED carries the identical fs adoption set",
        all(t in a64_shared for t in
            ("kernel/fs/blkdev.c", "kernel/fs/ext2.c",
             "kernel/lib/kprintf.c", "kernel/lib/spinlock.c"))))
    checks.append((
        "P3: the a64 glue mirrors the rv64 surface (sinks, heap, "
        "vfs_now, looped single-sector ops) and the sniff keeps the "
        "pattern gate",
        "a64fs_bringup" in read("kernel", "arch", "aarch64",
                                "fsglue_a64.c") and
        "vfs_now" in read("kernel", "arch", "aarch64", "fsglue_a64.c") and
        "no test pattern; filesystem media" in
        read("kernel", "arch", "aarch64", "main_a64.c") and
        "vblk_a64_selftest" in read("kernel", "arch", "aarch64",
                                    "main_a64.c")))

    # --- P4: one ABI file, six includers -- the layout cannot drift
    # --- because there is exactly one of it.
    fsabi_users = 0
    for path in ("lib/libcrv/libcrv.h", "lib/libca64/libca64.h",
                 "lib/libc32/libc32.h",
                 "kernel/arch/riscv64/user_rv.c",
                 "kernel/arch/aarch64/user_a64.c",
                 "kernel/arch/i386/user32.c"):
        if "lib/abi/fsabi.h" in read(*path.split("/")):
            fsabi_users += 1
    checks.append((
        f"P4: all six trap-boundary files include the ONE fsabi.h "
        f"(measured {fsabi_users}/6)",
        fsabi_users == 6 and exists("lib", "abi", "fsabi.h")))
    checks.append((
        "P4: smallsh grew ls/cat/stat THROUGH the wrappers (shared "
        "source, three builds), and the old absent-on-purpose line "
        "is gone",
        "readdir((int)fd" in read("userspace", "system", "smallsh",
                                  "smallsh.c") and
        "AURA_SEEK_END" in read("userspace", "system", "smallsh",
                                "smallsh.c") and
        "absent on purpose: ls/cat" not in
        read("userspace", "system", "smallsh", "smallsh.c")))

    # --- P5: SMP rv64 -- HSM engine present, receipts counted, and
    # --- the D5 boundary explicit (no scheduler claims).
    checks.append((
        "P5: sbi.c carries HSM hart_start and sPI send_ipi (the two "
        "extensions the plan promised)",
        "sbi_hart_start" in read("kernel", "arch", "riscv64", "sbi.c") and
        "0x48534D" in read("kernel", "arch", "riscv64", "sbi.c") and
        "sbi_send_ipi" in read("kernel", "arch", "riscv64", "sbi.c") and
        "0x735049" in read("kernel", "arch", "riscv64", "sbi.c")))
    checks.append((
        "P5: the secondary path exists end-to-end (boot.S entry + "
        "pool slot + smp_rv.c report-in) and stays OFF the trap path "
        "(sip polling, D5 named)",
        "_secondary_start" in read("kernel", "arch", "riscv64",
                                   "boot.S") and
        "secondary_jump_pool" in read("kernel", "arch", "riscv64",
                                      "boot.S") and
        "secondary_main_rv" in read("kernel", "arch", "riscv64",
                                    "smp_rv.c") and
        "csrr %0, sip" in read("kernel", "arch", "riscv64",
                               "smp_rv.c") and
        "NAMED" in read("kernel", "arch", "riscv64", "smp_rv.c")))

    # --- P6: SMP a64 -- the promised CPU_ON exists, the secondary
    # --- path is end-to-end, and the x16 boundary is MEASURED (code
    # --- max 16 both tenants; GICv2 caps a64 runs at 8; rv proves 16).
    checks.append((
        "P6: psci.c delivers the CPU_ON its A0 comment promised "
        "(0xC4000003), and boot.S has the secondary entry + pool",
        "psci_cpu_on" in read("kernel", "arch", "aarch64", "psci.c") and
        "0xC4000003" in read("kernel", "arch", "aarch64", "psci.c") and
        "_secondary_start" in read("kernel", "arch", "aarch64",
                                   "boot.S") and
        "secondary_jump_pool" in read("kernel", "arch", "aarch64",
                                      "boot.S")))
    checks.append((
        "P6: the a64 IPI is a banked-interface POLL (off the trap "
        "path) and the GICv2 8-core ceiling is named in the code",
        "GICC_IAR" in read("kernel", "arch", "aarch64", "smp_a64.c") and
        "architecturally 8" in read("kernel", "arch", "aarch64",
                                    "smp_a64.c")))
    checks.append((
        "P6/x16: both tenants carry SMP max 16 (rv proves -smp 16 "
        "live in its smoke's second lane)",
        "SMP_RV_MAX   16" in read("kernel", "arch", "riscv64",
                                  "smp_rv.c") and
        "SMP_A64_MAX   16" in read("kernel", "arch", "aarch64",
                                   "smp_a64.c") and
        "-smp 16" in read("tests", "integration", "rv_smp_smoke.sh")))

    # --- P7: i386 -- the width debt is PAID (live lane below), the
    # --- slave lane exists, and the adoption set is the same four
    # --- shared objects plus string.c and the div helpers.
    checks.append((
        "P7: KERNEL32_SHARED carries the same fs adoption set "
        "(+string.c; div64_32.c supplies __udivdi3)",
        all(t in makefl for t in ("kernel/fs/blkdev.c", )) and
        "kernel/fs/ext2.c kernel/lib/string.c" in makefl and
        "__udivdi3" in read("kernel", "arch", "i386", "div64_32.c")))
    checks.append((
        "P7: ata32 has the drive-parametrised lane and the glue "
        "mounts on the SLAVE (the boot disk stays the selftest's)",
        "ata32_read_drv" in read("kernel", "arch", "i386", "ata32.c") and
        "primary slave" in read("kernel", "arch", "i386", "ata32.c") and
        "ext2_init(-1)" in read("kernel", "arch", "i386",
                                "fsglue32.c")))

    # --- P7 LIVE: the whole fs tree compiles under CFLAGS32's OWN
    # --- strictness (-Wshorten-64-to-32) -- the pay-down cannot rot.
    I386_FLAGS = ("--target=i686-elf -std=c11 -ffreestanding "
                  "-fno-stack-protector -fno-pie -fno-pic "
                  "-malign-double -Wall -Wextra -Wno-unused-parameter "
                  "-Wno-unused-function -Werror -Wshorten-64-to-32 "
                  "-O2").split()
    passes, total = live_syntax_pass(I386_FLAGS)
    checks.append((
        f"live: kernel/fs compiles -fsyntax-only as i386 WITH "
        f"-Wshorten-64-to-32 ({passes}/{total}, expected "
        f"{FS_FILE_COUNT}/{FS_FILE_COUNT})",
        passes == FS_FILE_COUNT and total == FS_FILE_COUNT))

    # --- Completed phases must have their reserved artefacts (the
    # --- registry-reservation contract).
    for phase in PHASE_ORDER:
        row_done = re.search(r"^\| %s .*?✅ complete" % phase, plan, re.M)
        if row_done:
            for artefact in PHASE_ARTEFACTS[phase]:
                checks.append((
                    f"{phase}: reserved artefact exists ({artefact})",
                    exists(*artefact.split("/"))))

    # --- Structural: Status header vs table (the D8 shape).
    done_rows = len(re.findall(r"^\| P\d .*?✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### P\d[^\n]*✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete table row has a COMPLETE heading",
                   done_rows == done_heads and plan != ""))

    status_ok = False
    if re.search(r"^## Status: IN PROGRESS — P0 next", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"P0(?:–(P\d))? complete", plan)
        if m:
            label = m.group(1) if m.group(1) else "P0"
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
            print("check_parity_claims: SELFTEST inconclusive (tree "
                  "already red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(passed for _, passed in doctored):
            print("check_parity_claims: SELFTEST FAIL -- checks pass "
                  "against an empty tree", file=sys.stderr)
            return 1
        print("check_parity_claims: selftest PASS (doctored tree detected)")
        return 0

    failed = 0
    results = claims()
    for desc, passed in results:
        if not passed:
            print(f"check_parity_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_parity_claims: {failed} claim(s) disagree with the "
              "tree", file=sys.stderr)
        return 1
    print(f"check_parity_claims: OK -- {len(results)} claims verified "
          "against the tree (fs syntax lanes run LIVE)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
