#!/usr/bin/env bash
# test_selfhost_kernel_spike.sh -- SELFHOST_PLAN.md SH5a: the spike BOOTS.
#
# The link-level half of the spike is the host gate (tests/unit/test_sh5_spike.sh);
# this case proves the tcc+aulink kernel actually RUNS: it packs the spike
# kernel (boot.asm via mini-asm + kmain.c via host tcc, linked by aulink
# against kernel.ld) into a dual-boot ISO through the tree's own
# mkisoimage_dual.sh and boots it in QEMU, grepping the serial receipt the
# kernel prints from the higher half.
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$(dirname "$0")/.."
. lib/lib.sh

HOST_TCC="$ROOT/build/selfhost/host-tcc-src/tcc"
if [ ! -x "$HOST_TCC" ]; then
    echo "${C_YELLOW}[selfhost] host tcc not built -- skipping (run 'make selfhost-deps selfhost-host-tcc')${C_RESET}"
    il_skip "host tcc absent (selfhost-host-tcc not run)"
    exit 0
fi

# ---- build the spike artefacts + a spike-only ISO BEFORE il_init, which
# ---- would otherwise insist on the canonical build/auralite.iso ----------
SPIKE="$ROOT/build/spike"
mkdir -p "$SPIKE"
(
    cd "$ROOT"
    cc -std=c99 -O2 -o "$SPIKE/mini-asm" tools/mini-asm/mini-asm.c
    cc -std=c99 -O2 -o "$SPIKE/aulink"   tools/aulink/aulink.c
    "$SPIKE/mini-asm" -f elf64 kernel/arch/x86_64/boot.asm -o "$SPIKE/boot.o"
    "$HOST_TCC" -c -ffreestanding -fno-pic -o "$SPIKE/kmain.o" tools/selfhost/spike/kmain.c
    "$SPIKE/aulink" -T kernel.ld -o "$SPIKE/kernel-spike.elf" "$SPIKE/boot.o" "$SPIKE/kmain.o"
) || { echo "${C_RED}[lib] spike build failed${C_RESET}"; exit 2; }
( cd "$ROOT" && make mbr mbr-dual stage2 efi >/dev/null ) || { echo "${C_RED}[lib] make mbr/stage2/efi failed${C_RESET}"; exit 2; }
if ! bash "$ROOT/tools/mkisoimage_dual.sh" "$SPIKE/kernel-spike.elf" \
         "$ROOT/build/boot/BOOTX64.EFI" "$ROOT/build/spike.iso" >/dev/null 2>&1; then
    echo "${C_RED}[lib] mkisoimage_dual.sh failed${C_RESET}"; exit 2
fi

export IL_ISO="$ROOT/build/spike.iso"
il_init
il_have qemu-system-x86_64

il_section "self-host kernel spike (SH5a): tcc+aulink kernel boots to the higher half"

LOG="$IL_LOGDIR/selfhost_kernel_spike.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_run_qemu "$LOG" 60

il_assert_grep_fixed "$LOG" "SH5a spike: tcc+aulink kernel booted at the higher half" \
    "spike kernel prints its receipt from the higher half"
il_assert_grep "$LOG" "spike marker = 0x000000005a15a5e2" \
    "tcc-compiled .data global round-trips through the RIP-relative relocation"

il_summary
