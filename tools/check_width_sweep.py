#!/usr/bin/env python3
"""Fail when the pointer-width sweep regresses (I386_PLAN I6;
ratchet 4 added by RISCV_PLAN V6).

Four ratchets, all measured against the tree:

  1. `(uint64_t)` casts in PORTABLE kernel/driver code -- kernel/ and
     drivers/ excluding kernel/arch/ entirely.  Inside an arch tree the
     cast is usually the truth (x86_64 registers ARE 64-bit); in
     portable code each one is either a virtual address wearing the
     wrong type (bug on both widths), a physical address that should be
     spelled paddr_t, or an honest integer conversion.  The sweep reads
     them one by one; this counter guarantees the total only goes DOWN.

  2. Direct `#include "kernel/arch/x86_64/..."` from portable code
     (outside kernel/arch/).  These are the sites arch.h must absorb;
     the counter stops new ones appearing while the absorption runs.

  3. Cross-arch includes (i386 code including x86_64 headers or vice
     versa): always zero, no baseline, no exceptions.  The include
     path IS the arch boundary.

  4. `__asm__`-bearing PORTABLE files (RISCV_PLAN V6).  Inline
     assembly in portable code is an x86 assumption wearing a
     portability costume: the riscv64 build needs every one of these
     files to compile with ZERO asm outside kernel/arch/.  Counted as
     files-with-at-least-one-real-asm-statement (the regex wants the
     opening paren, so a comment MENTIONING __asm__ is not a hit).
     Migration route: kernel/arch/arch.h's irqflags block
     (arch_irq_save/restore, arch_wait_for_interrupt, arch_cpu_relax).

Same shape as check_test_registry.py / check_fixes_claims.py: a
hand-maintained claim drifts, a checked one cannot.  Run with
--baseline to print current counts; run with --selftest to prove the
checker still detects a violation (a checker that never fails is
indistinguishable from a clean tree).
"""

import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- Ratchet baselines (lower is better; raising either is a failure).
# Measured at the I6 landing commit.  When your change removes casts or
# migrates includes to arch.h, lower the number in the SAME commit --
# that is the ratchet clicking, and the whole point.
BASELINE_UINT64_CASTS = 359   # was 361 at I6 landing; slab.c paid 2
BASELINE_X64_INCLUDES = 69    # was 80; the portio.h -> arch.h batch paid 11
BASELINE_ASM_FILES    = 29    # was 33 at V6 arming; the first batch paid 4
                              # (spinlock->C11, kprintf, time, scheduler)

PORTABLE_DIRS = ["kernel", "drivers", "w32/src"]
CAST_RE = re.compile(r"\(uint64_t\)")
X64_INC_RE = re.compile(r'#include\s+"kernel/arch/x86_64/')
I386_INC_RE = re.compile(r'#include\s+"kernel/arch/i386/')
ASM_RE = re.compile(r'__asm__\s*(?:volatile\s*)?\(')


def source_files(top, exclude_prefixes=()):
    for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, top)):
        rel = os.path.relpath(dirpath, ROOT)
        if any(rel.startswith(p) for p in exclude_prefixes):
            continue
        for f in filenames:
            if f.endswith((".c", ".h")):
                yield os.path.join(dirpath, f)


def count_matches(files, regex):
    total = 0
    per_file = {}
    for path in files:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                n = sum(len(regex.findall(line)) for line in fh)
        except OSError:
            continue
        if n:
            per_file[os.path.relpath(path, ROOT)] = n
            total += n
    return total, per_file


def measure():
    # Ratchet 1: casts in portable code -- kernel/arch/ excluded (both
    # arch trees; an arch tree is entitled to its own width).
    cast_files = []
    for top in ["kernel", "drivers"]:
        cast_files.extend(source_files(top, exclude_prefixes=(
            os.path.join("kernel", "arch"),)))
    casts, _ = count_matches(cast_files, CAST_RE)

    # Ratchet 2: x86_64 includes from PORTABLE code only.
    inc_files = []
    for top in PORTABLE_DIRS:
        inc_files.extend(source_files(top, exclude_prefixes=(
            os.path.join("kernel", "arch"),)))
    x64_incs, _ = count_matches(inc_files, X64_INC_RE)

    # Ratchet 3: cross-arch includes, both directions.
    i386_files = list(source_files(os.path.join("kernel", "arch", "i386")))
    x64_files = list(source_files(os.path.join("kernel", "arch", "x86_64")))
    cross_a, cross_a_files = count_matches(i386_files, X64_INC_RE)
    cross_b, cross_b_files = count_matches(x64_files, I386_INC_RE)

    # Ratchet 4: FILES bearing real asm statements, portable code only.
    _, asm_per_file = count_matches(inc_files, ASM_RE)
    asm_files = len(asm_per_file)

    return (casts, x64_incs, cross_a + cross_b,
            {**cross_a_files, **cross_b_files}, asm_files)


def selftest():
    """Plant violations in temp files and prove the checker sees them
    -- then remove them."""
    victim = os.path.join(ROOT, "kernel", "arch", "i386",
                          "width_sweep_selftest_tmp.h")
    asm_victim = os.path.join(ROOT, "kernel", "lib",
                              "width_sweep_selftest_asm_tmp.h")
    try:
        with open(victim, "w") as fh:
            fh.write('#include "kernel/arch/x86_64/portio.h"\n')
        baseline_asm = measure()[4]
        with open(asm_victim, "w") as fh:
            fh.write('static inline void st(void) '
                     '{ __asm__ volatile ("nop"); }\n')
        _, _, cross, _, asm_files = measure()
        if cross < 1:
            print("check_width_sweep: SELFTEST FAIL -- planted cross-arch "
                  "include was not detected", file=sys.stderr)
            return 1
        if asm_files != baseline_asm + 1:
            print("check_width_sweep: SELFTEST FAIL -- planted __asm__ "
                  "file was not counted by ratchet 4", file=sys.stderr)
            return 1
        print("check_width_sweep: selftest PASS (planted violations "
              "detected, ratchet 4 included)")
        return 0
    finally:
        for v in (victim, asm_victim):
            if os.path.exists(v):
                os.unlink(v)


def main():
    if "--selftest" in sys.argv:
        return selftest()

    casts, x64_incs, cross, cross_files, asm_files = measure()

    if "--baseline" in sys.argv:
        print(f"BASELINE_UINT64_CASTS = {casts}")
        print(f"BASELINE_X64_INCLUDES = {x64_incs}")
        print(f"cross-arch includes   = {cross}")
        print(f"BASELINE_ASM_FILES    = {asm_files}")
        return 0

    rc = 0
    if casts > BASELINE_UINT64_CASTS:
        print(f"check_width_sweep: FAIL -- (uint64_t) casts rose to {casts} "
              f"(ratchet: {BASELINE_UINT64_CASTS}).  New code must use "
              f"paddr_t (physical) or uintptr_t (virtual); see "
              f"kernel/lib/paddr.h.", file=sys.stderr)
        rc = 1
    if x64_incs > BASELINE_X64_INCLUDES:
        print(f"check_width_sweep: FAIL -- direct x86_64 includes from "
              f"portable code rose to {x64_incs} (ratchet: "
              f"{BASELINE_X64_INCLUDES}).  Include kernel/arch/arch.h "
              f"instead.", file=sys.stderr)
        rc = 1
    if cross > 0:
        print(f"check_width_sweep: FAIL -- {cross} cross-arch include(s); "
              f"the include path IS the arch boundary:", file=sys.stderr)
        for f, n in sorted(cross_files.items()):
            print(f"  {f}: {n}", file=sys.stderr)
        rc = 1
    if asm_files > BASELINE_ASM_FILES:
        print(f"check_width_sweep: FAIL -- __asm__-bearing portable files "
              f"rose to {asm_files} (ratchet: {BASELINE_ASM_FILES}).  Use "
              f"kernel/arch/arch.h's arch_irq_save/restore/"
              f"arch_wait_for_interrupt/arch_cpu_relax instead.",
              file=sys.stderr)
        rc = 1

    if rc == 0:
        print(f"check_width_sweep: OK -- casts {casts}/{BASELINE_UINT64_CASTS}, "
              f"x64-includes {x64_incs}/{BASELINE_X64_INCLUDES}, "
              f"cross-arch 0, asm-files {asm_files}/{BASELINE_ASM_FILES}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
