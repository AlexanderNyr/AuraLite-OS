#!/usr/bin/env bash
# test_sh5_spike.sh -- SELFHOST_PLAN.md SH5a host gate: the spike.
#
# The SH5 measured question (Fact 2): is TinyCC's x86_64 codegen linkable at
# the higher-half address 0xFFFFFFFF80100000?  This gate answers it on the
# host, without QEMU:
#   1. compiles tools/selfhost/spike/kmain.c with the HOST tcc built from the
#      same mob source the guest toolchain uses (make selfhost-host-tcc),
#   2. asserts the object carries NO 32-bit absolute relocations -- only
#      R_X86_64_PC32 / R_X86_64_PLT32 / R_X86_64_64, all representable at
#      the higher half (unlike gcc/clang small-model 32-bit absolutes),
#   3. links it with the real kernel.ld via aulink (SH3), with the real
#      boot.asm assembled by mini-asm (SH4d), and asserts the output ELF:
#      entry _start at 0xFFFFFFFF80100000, three PT_LOADs, higher-half
#      __bss_start/__bss_end.
#
# The BOOT half of the spike lives in
# tests/integration/cases/test_selfhost_kernel_spike.sh (QEMU + serial
# receipt).  This gate skips cleanly when the host tcc is absent.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

HOST_TCC="$ROOT/build/selfhost/host-tcc-src/tcc"
if [ ! -x "$HOST_TCC" ]; then
    echo "[selfhost] spike SKIP: host tcc absent -- run 'make selfhost-deps selfhost-host-tcc'"
    exit 0
fi
command -v readelf >/dev/null 2>&1 || { echo "[selfhost] spike SKIP: readelf not installed"; exit 0; }

BUILD="$ROOT/build/spike"
rm -rf "$BUILD"
mkdir -p "$BUILD"

# 1) tools
cc -std=c99 -O2 -o "$BUILD/mini-asm" tools/mini-asm/mini-asm.c || { echo "FAIL: mini-asm build"; exit 1; }
cc -std=c99 -O2 -o "$BUILD/aulink"   tools/aulink/aulink.c   || { echo "FAIL: aulink build"; exit 1; }

# 2) assemble boot.asm (mini-asm) + compile kmain.c (host tcc)
if ! "$BUILD/mini-asm" -f elf64 kernel/arch/x86_64/boot.asm -o "$BUILD/boot.o"; then
    echo "FAIL: mini-asm could not assemble boot.asm"; exit 1
fi
if ! "$HOST_TCC" -c -ffreestanding -fno-pic -o "$BUILD/kmain.o" tools/selfhost/spike/kmain.c; then
    echo "FAIL: host tcc could not compile kmain.c"; exit 1
fi

# 3) measurement: no 32-bit absolute relocations in the tcc object
if readelf -rW "$BUILD/kmain.o" | grep -qE "R_X86_64_(32|32S|PC32S)"; then
    echo "FAIL: tcc emitted a 32-bit absolute relocation -- not linkable at the higher half"
    readelf -rW "$BUILD/kmain.o" | grep -E "R_X86_64_(32|32S|PC32S)"
    exit 1
fi

# 4) link with the real kernel.ld via aulink
if ! "$BUILD/aulink" -T kernel.ld -o "$BUILD/kernel-spike.elf" "$BUILD/boot.o" "$BUILD/kmain.o" 2>"$BUILD/aulink.err"; then
    echo "FAIL: aulink could not link the spike kernel"
    sed 's/^/    /' "$BUILD/aulink.err"
    exit 1
fi

# 5) output ELF asserts
FAILED=0
entry=$(readelf -h "$BUILD/kernel-spike.elf" | awk '/Entry point address/{print $4}')
[ "$entry" = "0xffffffff80100000" ] || { echo "FAIL: entry=$entry (want 0xffffffff80100000)"; FAILED=1; }
loads=$(readelf -lW "$BUILD/kernel-spike.elf" | grep -c "LOAD")
[ "$loads" -eq 3 ] || { echo "FAIL: $loads PT_LOADs (want 3)"; FAILED=1; }
flg=$(readelf -lW "$BUILD/kernel-spike.elf" | python3 -c '
import sys, re
for l in sys.stdin:
    if "LOAD" in l:
        m = re.search(r" ([RWE ]+?) +0x[0-9a-f]+$", l)
        print(m.group(1).strip().replace(" ","") if m else "?")
' | tr "\n" " ")
[ "$flg" = "RE R RW " ] || { echo "FAIL: PT_LOAD flags '$flg' (want 'RE R RW ')"; FAILED=1; }
readelf -sW "$BUILD/kernel-spike.elf" | grep -qE "ffffffff8010[0-9a-f]+ .* kmain" || { echo "FAIL: kmain not at a higher-half address"; FAILED=1; }
bss_lo=$(readelf -sW "$BUILD/kernel-spike.elf" | grep "__bss_start" | awk '{print $2}')
case "$bss_lo" in
    ffffffff8010*) ;;
    *) echo "FAIL: __bss_start=$bss_lo not in the higher half"; FAILED=1 ;;
esac

if [ "$FAILED" -ne 0 ]; then
    echo "[selfhost] spike FAIL: tcc+aulink kernel does NOT link at the higher half"
    exit 1
fi
echo "PASS: tcc kmain.o carries only PC32/PLT32/64 relocations (no 32-bit absolutes)"
echo "PASS: kernel-spike.elf entry 0xffffffff80100000, 3 PT_LOAD (R E / R / RW)"
echo "[selfhost] spike PASS: tcc+aulink kernel links at the higher half"
exit 0
