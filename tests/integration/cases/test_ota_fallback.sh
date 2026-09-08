#!/usr/bin/env bash
# test_ota_fallback.sh — OTA_PLAN O3: stage2 A/B fallback.
#
# The bootloader must survive a corrupt KERNEL.ELF: exactly ONE bounded
# retry via KERNEL.OLD, then the existing loud halt.  Three lanes:
#   A: KERNEL.ELF replaced by garbage, KERNEL.OLD = the real kernel
#      → the fallback receipts and a FULL boot (shell, uname answered).
#   B: KERNEL.ELF garbage, no KERNEL.OLD → both receipts + the halt,
#      and the kernel never runs.
#   C: untouched image → the fallback line never prints; the happy-path
#      receipts are exactly what they always were.
#
# Doctoring is host-side mtools on a COPY of the dual image.  The ESP
# byte offset is parsed from the image's own MBR (entry 0's start LBA),
# not hardcoded — the same table O1's partition-aware mount reads in the
# guest, so a relayout moves this test with it instead of silently
# doctoring the wrong sectors.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 mcopy mdir python3

il_section "OTA O3: stage2 KERNEL.OLD fallback"

ESP_OFFS=$(python3 - "$IL_ISO" <<'PY'
import struct, sys
with open(sys.argv[1], 'rb') as f:
    mbr = f.read(512)
start = struct.unpack_from('<I', mbr, 0x1BE + 8)[0]   # entry 0 start LBA
print(start * 512)
PY
)
if [ -z "$ESP_OFFS" ] || [ "$ESP_OFFS" = "0" ]; then
    echo "FATAL: no MBR entry 0 in $IL_ISO — cannot locate the ESP"
    exit 2
fi
echo "  [ota] ESP at byte offset $ESP_OFFS (MBR entry 0)"

GARBAGE="$IL_LOGDIR/ota_fallback_garbage.bin"
dd if=/dev/zero of="$GARBAGE" bs=4096 count=1 status=none

# ---- Lane A: corrupt ELF slot, healthy OLD slot ---------------------------
LOGA="$IL_LOGDIR/ota_fallback_a.log"
IL_LAST_LOG="$LOGA"
DOCA="$IL_LOGDIR/ota_fallback_a.iso"
rm -f "$LOGA" "$DOCA"
cp "$IL_ISO" "$DOCA"

# Stage the OLD slot with the REAL kernel, then trash the ELF slot.
mcopy -i "$DOCA@@$ESP_OFFS" ::/KERNEL.ELF "$IL_LOGDIR/ota_fallback_kernel.elf"
mcopy -i "$DOCA@@$ESP_OFFS" "$IL_LOGDIR/ota_fallback_kernel.elf" ::/KERNEL.OLD
mcopy -o -i "$DOCA@@$ESP_OFFS" "$GARBAGE" ::/KERNEL.ELF

( { sleep 16; printf 'uname\n'; sleep 2; printf 'exit\n'; } \
  | timeout 90 qemu-system-x86_64 \
    -drive "file=$DOCA,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci0 -device ide-hd,drive=btndisk,bus=ahci0.0 \
    -m 512M -smp 2 -display none -serial stdio -no-reboot -cpu qemu64 \
    -boot order=c -fw_cfg "name=opt/auralite.selftest,string=fast" \
    > "$LOGA" 2>&1 ) || true

il_assert_grep "$LOGA" "\[BL4\] kernel.elf located" \
    "the garbage KERNEL.ELF is still FOUND (it fails at parse time, not find)"
il_assert_grep "$LOGA" "\[BL4\] ELF parse FAILED" \
    "garbage is rejected by the ELF parser"
il_assert_grep "$LOGA" "KERNEL.ELF unloadable -- falling back to KERNEL.OLD" \
    "the O3 fallback receipt prints"
il_assert_grep "$LOGA" "\[BL4\] kernel.old located" \
    "KERNEL.OLD is located"
il_assert_grep "$LOGA" "\[BL4\] kernel.old loaded to 0x00200000" \
    "and staged at the same 0x00200000 buffer"
il_assert_grep "$LOGA" "\[BL4\] ELF PT_LOAD segments copied to phys" \
    "the OLD kernel's PT_LOAD segments are copied"
il_assert_grep "$LOGA" "Hello from AuraLite" \
    "the fallback boot reaches userspace"
il_assert_grep "$LOGA" "AuraLite OS .* x86_64" \
    "uname answers from the fallback boot"

# ---- Lane B: corrupt ELF slot, no OLD slot --------------------------------
LOGB="$IL_LOGDIR/ota_fallback_b.log"
IL_LAST_LOG="$LOGB"
DOCB="$IL_LOGDIR/ota_fallback_b.iso"
rm -f "$LOGB" "$DOCB"
cp "$IL_ISO" "$DOCB"
mcopy -o -i "$DOCB@@$ESP_OFFS" "$GARBAGE" ::/KERNEL.ELF

# No shell will ever come up; no feeder needed — the stage2 halt receipt
# lands in the log long before the timeout cuts QEMU.
( timeout 45 qemu-system-x86_64 \
    -drive "file=$DOCB,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci1 -device ide-hd,drive=btndisk,bus=ahci1.0 \
    -m 512M -smp 2 -display none -serial stdio -no-reboot -cpu qemu64 \
    -boot order=c -fw_cfg "name=opt/auralite.selftest,string=off" \
    > "$LOGB" 2>&1 ) || true

il_assert_grep "$LOGB" "\[BL4\] ELF parse FAILED" \
    "the garbage slot is rejected"
il_assert_grep "$LOGB" "KERNEL.ELF unloadable -- falling back to KERNEL.OLD" \
    "the fallback is attempted exactly once"
il_assert_grep "$LOGB" "KERNEL.OLD missing or unloadable; halting" \
    "with no bootable slot the loud halt receipt prints"
il_assert_no_grep "$LOGB" "\[BL4\] kernel.old located" \
    "KERNEL.OLD was never there"
il_assert_no_grep "$LOGB" "Hello from AuraLite" \
    "the kernel never runs when both slots are broken"

# ---- Lane C: untouched image ----------------------------------------------
LOGC="$IL_LOGDIR/ota_fallback_c.log"
IL_LAST_LOG="$LOGC"
rm -f "$LOGC"

( { sleep 14; printf 'uname\n'; sleep 2; printf 'exit\n'; } \
  | timeout 90 qemu-system-x86_64 \
    -drive "file=$IL_ISO,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci2 -device ide-hd,drive=btndisk,bus=ahci2.0 \
    -m 512M -smp 2 -display none -serial stdio -no-reboot -cpu qemu64 \
    -boot order=c -fw_cfg "name=opt/auralite.selftest,string=fast" \
    > "$LOGC" 2>&1 ) || true

il_assert_no_grep "$LOGC" "falling back to KERNEL.OLD" \
    "the happy path NEVER prints the fallback line"
il_assert_grep "$LOGC" "\[BL4\] kernel.elf located" \
    "the normal receipts are unchanged"
il_assert_grep "$LOGC" "\[BL4\] ELF PT_LOAD segments copied to phys" \
    "the normal load flow runs to the end"
il_assert_grep "$LOGC" "Hello from AuraLite" \
    "the untouched image boots normally"

il_summary
