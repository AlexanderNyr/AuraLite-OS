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
#
# RISCV_PLAN V1 added the third width lane (rv64); ARM64_PLAN A6
# added the fourth (a64) plus the backend lanes: the irqflags
# contract probe compiled once per target, the a64 port-I/O negative
# control, and the zero-asm-after-preprocessing check on the shared
# portable files.
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

# ---- RISCV_PLAN V1: the third width ----
# LP64 should agree with AMD64 on every offset (both are 8-byte
# uint64_t, natural alignment); the compile makes "should" permanent.
# No negative control here: there is no rv64 ABI flag that mis-aligns
# uint64_t the way plain i686 does -- the i686 control already guards
# the assert set itself.
if clang --target=riscv64 -march=rv64gc -mabi=lp64d -ffreestanding \
        -std=c11 -I . -c "$T" -o build/biw_rv64.o 2>/dev/null; then
    note "OK   contract compiles at rv64 (third width, LP64)"
else
    note "FAIL contract at rv64 -- the riscv64 kernel reads boot_info_t"
    note "     at different offsets than its own FDT shim writes"
    fail=1
fi

# ---- ARM64_PLAN A6: the fourth width + the fourth backend ----
# Lane 1: the boot_info contract at aarch64.  LP64 again, so it must
# agree with AMD64/rv64 on every offset; the compile makes it
# permanent (same reasoning as the rv64 lane: no a64 ABI flag
# mis-aligns uint64_t the way plain i686 does, so no negative control
# -- the i686 one already guards the assert set).
if clang --target=aarch64-unknown-none-elf -mgeneral-regs-only \
        -ffreestanding -std=c11 -I . -c "$T" -o build/biw_a64.o 2>/dev/null; then
    note "OK   contract compiles at a64 (fourth width, LP64)"
else
    note "FAIL contract at a64 -- the aarch64 kernel reads boot_info_t"
    note "     at different offsets than its own FDT shim writes"
    fail=1
fi

# Lane 2: the irqflags contract closes over ALL FOUR backends.  One
# probe TU exercising arch_irq_save/restore/arch_wait_for_interrupt/
# arch_cpu_relax through kernel/arch/arch.h, compiled once per
# target.  This is the D6 thesis as a compile: if any backend's
# signatures drift, exactly one of these four lanes goes red.
PROBE=build/width_irqflags_probe.c
cat > "$PROBE" <<'EOF'
#include "kernel/arch/arch.h"
void probe(void)
{
    arch_irqflags_t f = arch_irq_save();
    arch_cpu_relax();
    arch_irq_restore(f);
    arch_wait_for_interrupt();
}
EOF
for lane in \
    "x86_64:--target=x86_64-elf" \
    "i386:--target=i686-elf" \
    "rv64:--target=riscv64 -march=rv64gc -mabi=lp64d" \
    "a64:--target=aarch64-unknown-none-elf -mgeneral-regs-only"; do
    name="${lane%%:*}"; flags="${lane#*:}"
    # shellcheck disable=SC2086
    if clang $flags -ffreestanding -std=c11 -I . \
            -c "$PROBE" -o "build/width_irqflags_$name.o" 2>/dev/null; then
        note "OK   irqflags contract closes at $name"
    else
        note "FAIL irqflags contract at $name"
        fail=1
    fi
done

# Lane 3: the NEGATIVE control for the a64 port-I/O fence.  A TU that
# USES inb() must refuse to compile at aarch64, and the error must
# name the device route (virtio-mmio) -- a fence that silently stubs
# is the xHCI lesson repeated.  If this ever compiles, the
# unavailable-attribute fence has gone soft.
NPROBE=build/width_portio_neg.c
printf '#include "kernel/arch/arch.h"\nunsigned char p(void){ return inb(0x60); }\n' > "$NPROBE"
if clang --target=aarch64-unknown-none-elf -mgeneral-regs-only \
        -ffreestanding -std=c11 -I . -c "$NPROBE" \
        -o build/width_portio_neg.o 2>build/width_portio_neg.err; then
    note "FAIL negative control: port I/O compiled at a64 -- the fence"
    note "     no longer stops x86 drivers from drifting into the a64 build"
    fail=1
elif grep -q "virtio-mmio" build/width_portio_neg.err; then
    note "OK   negative control: a64 port I/O refused, route named"
else
    note "FAIL negative control: a64 port I/O refused but the error"
    note "     does not name the virtio-mmio route"
    fail=1
fi

# Lane 4: zero asm reaches the a64 compile of the shared portable
# files.  Textual grep would lie (string.c carries x86 fast paths
# behind #ifdef); the PREPROCESSED output at the a64 target is what
# the compiler actually sees, so that is what gets counted.  The file
# list comes from the Makefile variable, not a copy that could drift.
A64_SHARED=$(sed -n 's/^KERNELA64_SHARED := //p' Makefile)
if [ -n "$A64_SHARED" ]; then
    for f in $A64_SHARED; do
        if clang --target=aarch64-unknown-none-elf -mgeneral-regs-only \
                -ffreestanding -std=c11 -I . -E "$f" 2>/dev/null \
                | grep -q '__asm__'; then
            note "FAIL $f: __asm__ survives preprocessing at a64"
            fail=1
        else
            note "OK   $f: zero __asm__ reaches the a64 compile"
        fi
    done
else
    note "FAIL KERNELA64_SHARED not found in the Makefile"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "[width] all I6 host gates passed"
else
    echo "[width] FAILED"
fi
exit "$fail"
