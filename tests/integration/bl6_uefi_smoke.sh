#!/usr/bin/env bash
# tests/integration/bl6_uefi_smoke.sh -- boot BOOTX64.EFI through OVMF.
#
# Builds a small ESP-shaped FAT32 image containing:
#   /EFI/BOOT/BOOTX64.EFI      (from `make efi`)
#   /EFI/BOOT/KERNEL.ELF       (from `make kernel`)
# and starts QEMU with OVMF firmware.  The default UEFI boot manager
# looks for /EFI/BOOT/BOOTX64.EFI on removable media, so no extra
# nvram configuration is required.
#
# Pass gate: kernel banner + "booted via UEFI" appear on COM1.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/bl6_uefi.img"
LOG="$BUILD/bl6_uefi.log"
FAT_IMG="$BUILD/bl6_uefi_esp.img"

OVMF="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
if [ ! -f "$OVMF" ]; then
    echo "  [bl6-uefi] SKIP: OVMF firmware not found at $OVMF"
    echo "             set OVMF_CODE=/path/to/OVMF_CODE.fd to override."
    exit 0
fi

# 1. Build the two artefacts we need.
make -C "$ROOT" efi kernel >/dev/null

# 2. Populate a directory tree that QEMU's built-in VFAT emulation
#    will present to OVMF as a FAT-formatted disk with the standard
#    UEFI removable-media boot layout.  We use `-drive fat:rw:<dir>`
#    rather than mformat because OVMF's boot manager insists on a
#    fully partitioned ESP-with-GPT to see a raw FAT image, but is
#    happy to boot any /EFI/BOOT/BOOTX64.EFI on a VFAT-emulated disk.
ESP_ROOT="$BUILD/bl6_esp_root"
rm -rf "$ESP_ROOT"
mkdir -p "$ESP_ROOT/EFI/BOOT"
cp "$BUILD/boot/BOOTX64.EFI" "$ESP_ROOT/EFI/BOOT/BOOTX64.EFI"
cp "$BUILD/kernel.elf"       "$ESP_ROOT/EFI/BOOT/KERNEL.ELF"
[ -f "$BUILD/initrd.tar" ] && cp "$BUILD/initrd.tar" "$ESP_ROOT/EFI/BOOT/INITRD.TAR"

# 3. Copy OVMF VARS to a scratch location -- variable-store writes
#    persist between runs, so we hand it a fresh copy every time.
VARS="$BUILD/OVMF_VARS_scratch.fd"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$VARS" 2>/dev/null || \
    cp /usr/share/OVMF/OVMF_VARS.fd "$VARS" 2>/dev/null || \
    { echo "  [bl6-uefi] SKIP: no OVMF_VARS available"; exit 0; }

rm -f "$LOG"
timeout 40 qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive format=raw,file=fat:rw:"$ESP_ROOT",if=ide \
    -m 256M \
    -display none -serial file:"$LOG" -no-reboot \
    >/dev/null 2>&1 || true

# 5. Assertions.
fail=0
for want in "[BL6] BOOTX64.EFI entered" \
            "[BL6] KERNEL.ELF loaded from ESP" \
            "[BL6] PT_LOAD segments copied to phys" \
            "[BL6] page tables built" \
            "[BL6] ExitBootServices OK" \
            "[BL6] jumping to kernel _start" \
            "Hello from AuraLite OS kernel!" \
            "booted via UEFI" \
            "HHDM offset: 0xffff800000000000"; do
    if grep -qF "$want" "$LOG"; then
        printf '  [bl6-uefi] serial OK   %s\n' "$want"
    else
        printf '  [bl6-uefi] serial MISS %s\n' "$want"
        fail=1
    fi
done

if grep -qE '(PANIC|panic:|BL6\] FATAL)' "$LOG"; then
    echo "  [bl6-uefi] serial FAIL kernel/loader error observed"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "[bl6-uefi] PASS -- BOOTX64.EFI boots to kernel via UEFI/OVMF"
    exit 0
else
    echo "[bl6-uefi] FAIL"
    echo "--- last 80 lines of serial log ---"
    tail -80 "$LOG" | sed 's/^/    /'
    exit 1
fi
