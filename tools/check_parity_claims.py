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
AHCI_IN_FS_PIN = 41        # P1 takes this to 0 and it stays 0.
SYSCALL_CASE_PIN = {       # P4 takes all three to 11.
    ("kernel/arch/riscv64/user_rv.c",  r"case SYS_RV_\w+:"):  6,
    ("kernel/arch/aarch64/user_a64.c", r"case SYS_A64_\w+:"): 6,
    ("kernel/arch/i386/user32.c",      r"case SYS32_\w+:"):   6,
}
FS_FILE_COUNT = 19         # kernel/fs/*.c, all of which must compile.

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
            f"(measured {count}; P4 moves this pin to 11)",
            count == pin and text != ""))

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
