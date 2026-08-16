#!/usr/bin/env bash
# tests/unit/test_width_sweep.sh -- I386_PLAN I6 host gates.
#
# Four checks, each with a reason to exist:
#
#   1. tools/check_width_sweep.py       -- the three ratchets hold.
#   2. Its --selftest                    -- the checker still detects a
#      planted violation (a checker that never fails is
#      indistinguishable from a clean tree).
#   3. test_boot_info_width.c compiles for x86_64 AND for i686 with
#      -malign-double: the three-party offset contract (16-bit loader,
#      64-bit kernel, 32-bit kernel) holds by _Static_assert.
#   4. The NEGATIVE control: the same file must FAIL to compile for
#      i686 WITHOUT -malign-double -- that failing compile is the
#      regression test for the I1 "mmap entries: 0" ABI bug.  If it
#      ever starts compiling, the asserts have gone soft and check
#      nothing.
#
# Skips cleanly when clang is unavailable (same convention as
# test_userlibs.sh).
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

fail=0
note() { printf '  [width] %s\n' "$*"; }

# ---- 1 + 2: the ratchet checker and its self-test ----
if python3 tools/check_width_sweep.py; then
    note "OK   ratchets hold"
else
    note "FAIL ratchets"
    fail=1
fi
if python3 tools/check_width_sweep.py --selftest >/dev/null 2>&1; then
    note "OK   checker self-test (planted violation detected)"
else
    note "FAIL checker self-test"
    fail=1
fi

# ---- 3 + 4: the cross-width boot_info contract ----
if ! command -v clang >/dev/null 2>&1; then
    note "SKIP boot_info width contract (no clang)"
    exit "$fail"
fi

mkdir -p build
[ -f build/boot_offsets.h ] || make boot-offsets >/dev/null 2>&1 || true
if [ ! -f build/boot_offsets.h ]; then
    # gen_boot_offsets writes the .inc by default; ask for the header.
    cc -std=c11 -I . tools/gen_boot_offsets.c -o build/gen_boot_offsets_w
    ./build/gen_boot_offsets_w --c > build/boot_offsets.h
fi

T=tests/unit/test_boot_info_width.c

if clang --target=x86_64-elf -ffreestanding -std=c11 -I . \
        -c "$T" -o build/biw_64.o 2>/dev/null; then
    note "OK   contract compiles at 64-bit"
else
    note "FAIL contract at 64-bit"
    fail=1
fi

if clang --target=i686-elf -ffreestanding -std=c11 -malign-double -I . \
        -c "$T" -o build/biw_32.o 2>/dev/null; then
    note "OK   contract compiles at 32-bit with -malign-double"
else
    note "FAIL contract at 32-bit with -malign-double"
    fail=1
fi

if clang --target=i686-elf -ffreestanding -std=c11 -I . \
        -c "$T" -o build/biw_32_neg.o 2>/dev/null; then
    note "FAIL negative control: compiled WITHOUT -malign-double -- the"
    note "     asserts no longer catch the I1 ABI bug"
    fail=1
else
    note "OK   negative control: plain -m32 layout refused, as required"
fi

if [ "$fail" -eq 0 ]; then
    echo "[width] all I6 host gates passed"
else
    echo "[width] FAILED"
fi
exit "$fail"
