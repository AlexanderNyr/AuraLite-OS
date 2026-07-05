#!/usr/bin/env bash
# tests/integration/bl7_dual_smoke.sh -- BL7 gate criterion.
#
# Builds a single dual-boot ISO with tools/mkisoimage_dual.sh and
# runs it under QEMU twice:
#
#   1. Legacy BIOS  -- `-drive format=raw,file=<iso>,if=ide`
#                      SeaBIOS boots the hybrid MBR at LBA 0, which
#                      reads Stage 2 from LBA 34..159, which reads
#                      KERNEL.ELF from the ESP at LBA 256.
#   2. UEFI         -- OVMF firmware sees the GPT + ESP, follows the
#                      /EFI/BOOT/BOOTX64.EFI fallback path, which
#                      reads KERNEL.ELF via SimpleFileSystem.
#
# Both paths must reach the kernel banner and print `booted via BIOS`
# / `booted via UEFI` respectively.  The same physical bytes on disk
# service both firmware types -- there is exactly one ISO file, no
# per-firmware artefact.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite-dual.iso"
LOG_BIOS="$BUILD/bl7_dual_bios.log"
LOG_UEFI="$BUILD/bl7_dual_uefi.log"

OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
[ -f "$OVMF_CODE"    ] || { echo "  [bl7-dual] SKIP: no $OVMF_CODE"; exit 0; }
[ -f "$OVMF_VARS_SRC" ] || { echo "  [bl7-dual] SKIP: no $OVMF_VARS_SRC"; exit 0; }

# 1. Build every artefact + assemble the ISO.
make -C "$ROOT" mbr-dual stage2 efi kernel >/dev/null
bash "$ROOT/tools/mkisoimage_dual.sh" \
    "$BUILD/kernel.elf" "$BUILD/boot/BOOTX64.EFI" "$ISO" >/dev/null

# ISO sanity checks.
sz=$(wc -c < "$ISO")
sig=$(od -An -tx1 -N2 -j510 "$ISO" | tr -d ' \n')
gpt_sig=$(dd if="$ISO" bs=1 skip=512 count=8 status=none)
part1_type=$(od -An -tx1 -N1 -j$((0x1BE + 4)) "$ISO" | tr -d ' \n')
part2_type=$(od -An -tx1 -N1 -j$((0x1CE + 4)) "$ISO" | tr -d ' \n')

echo "  [bl7-dual] hybrid image OK  (size=$sz bytes)"
[ "$sig" = "55aa" ]           || { echo "    MBR signature is 0x$sig, expected 55aa"; exit 1; }
[ "$gpt_sig" = "EFI PART" ]   || { echo "    GPT signature at LBA 1 is '$gpt_sig', expected 'EFI PART'"; exit 1; }
[ "$part1_type" = "0c" ]      || { echo "    MBR part 1 type is 0x$part1_type, expected 0x0c (FAT32-LBA)"; exit 1; }
[ "$part2_type" = "ee" ]      || { echo "    MBR part 2 type is 0x$part2_type, expected 0xee (GPT protective)"; exit 1; }
echo "                      MBR sig=0x$sig, GPT='$gpt_sig', MBR[1]=0x$part1_type (FAT32-LBA), MBR[2]=0x$part2_type (GPT protective)"

# 2. Run the BIOS path.
rm -f "$LOG_BIOS"
timeout 30 qemu-system-x86_64 \
    -drive format=raw,file="$ISO",if=ide \
    -m 256M -display none -serial file:"$LOG_BIOS" -no-reboot \
    >/dev/null 2>&1 || true

# 3. Run the UEFI path.
VARS="$BUILD/OVMF_VARS_dual.fd"
cp "$OVMF_VARS_SRC" "$VARS"
rm -f "$LOG_UEFI"
timeout 30 qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive format=raw,file="$ISO",if=ide \
    -m 256M -display none -serial file:"$LOG_UEFI" -no-reboot \
    >/dev/null 2>&1 || true

# 4. Assertions.
fail=0
check_log() {
    local tag=$1; local log=$2; local want=$3
    if grep -qF "$want" "$log"; then
        printf '  [%s] serial OK   %s\n' "$tag" "$want"
    else
        printf '  [%s] serial MISS %s\n' "$tag" "$want"
        fail=1
    fi
}

# BIOS path.
check_log "bl7/bios" "$LOG_BIOS" "[BL3] AuraLite stage2 alive"
check_log "bl7/bios" "$LOG_BIOS" "[BL4] entering long mode; jumping to kernel _start"
check_log "bl7/bios" "$LOG_BIOS" "Hello from AuraLite OS kernel!"
check_log "bl7/bios" "$LOG_BIOS" "booted via BIOS"
check_log "bl7/bios" "$LOG_BIOS" "HHDM offset: 0xffff800000000000"

# UEFI path.
check_log "bl7/uefi" "$LOG_UEFI" "[BL6] BOOTX64.EFI entered"
check_log "bl7/uefi" "$LOG_UEFI" "[BL6] ExitBootServices OK"
check_log "bl7/uefi" "$LOG_UEFI" "Hello from AuraLite OS kernel!"
check_log "bl7/uefi" "$LOG_UEFI" "booted via UEFI"
check_log "bl7/uefi" "$LOG_UEFI" "HHDM offset: 0xffff800000000000"

# No panics on either path.
if grep -qE '(PANIC|panic:)' "$LOG_BIOS" "$LOG_UEFI" 2>/dev/null; then
    echo "  [bl7-dual] FAIL kernel PANIC observed on at least one path"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "[bl7-dual] PASS -- one ISO boots to kernel via BOTH BIOS and UEFI"
    exit 0
else
    echo "[bl7-dual] FAIL"
    echo "--- BIOS log tail ---"
    tail -30 "$LOG_BIOS" | sed 's/^/    /'
    echo "--- UEFI log tail ---"
    tail -30 "$LOG_UEFI" | sed 's/^/    /'
    exit 1
fi
