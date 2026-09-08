#!/usr/bin/env bash
# test_ota_bootvol.sh — OTA_PLAN O1: the boot volume is visible and untouchable.
#
# The kernel used to mount /fat at a FIXED LBA 64 of blkdev 0 and both slot-0
# formatters (diskfs at LBA 2, fat32 at LBA 64) plus the AHCI self-test's
# scratch write (LBA 1) treated any "empty-looking" area of the first AHCI
# disk as scratch.  Booting the hybrid image from AHCI therefore destroyed
# the GPT header, the stage2 area and the ESP's lead sectors during a single
# boot (measured on the baseline tree; snapshot=on hid it from the host).
#
# This case proves the O1 contract end to end, booting the REAL bootable
# image from AHCI as the only disk (SeaBIOS boots AHCI fine -- that is the
# OTA harness shape: boot disk == AHCI blkdev 0 == /fat):
#
#   Lane A: the boot disk mounts its own ESP.
#     - AHCI self-test verifies its DMA write NON-destructively (the
#       GPT header is saved and restored; the "via GPT" mount receipt
#       below is the proof it survived the whole boot)
#     - diskfs REFUSES to format (partitioned disk)
#     - fat32 finds the FAT32 partition at LBA 256 (via GPT)
#     - zero "formatting" lines in the whole boot
#     - `ls /fat` lists the bootloader's KERNEL.ELF
#   Lane B: a partitioned disk WITHOUT any FAT32 volume refuses loudly and
#     still formats nothing.
#   Lane C: a table-less scratch disk keeps the legacy semantics (the
#     selfhost SH5d flow depends on the auto-format at LBA 64).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "OTA O1: boot-volume visibility and safety"

# ---- Lane A: boot the dual image from AHCI as the only disk --------------
LOGA="$IL_LOGDIR/ota_bootvol_a.log"
IL_LAST_LOG="$LOGA"
rm -f "$LOGA"

# Hand-rolled QEMU (the test_gui shape): il_run_qemu would attach the ISO
# as an IDE disk too, and the point here is that the AHCI disk IS the boot
# disk.  Input must be a PIPE, not a file redirect -- a still-empty regular
# file hands QEMU an immediate EOF on stdin and the whole feed dies silently
# (found the hard way: boot reached the prompt, then nothing was ever read).
# Boot-to-prompt with selftest=fast measures ~13 s; 14 s is safe.
( { sleep 14; printf 'ls /fat\n'; sleep 3; printf 'exit\n'; } \
  | timeout 90 qemu-system-x86_64 \
    -drive "file=$IL_ISO,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci0 -device ide-hd,drive=btndisk,bus=ahci0.0 \
    -m 512M -smp 2 -display none -serial stdio -no-reboot -cpu qemu64 \
    -boot order=c -fw_cfg "name=opt/auralite.selftest,string=fast" \
    > "$LOGA" 2>&1 ) || true

il_assert_grep "$LOGA" "self-test: non-destructive write verify on sector 1" \
    "AHCI self-test knows the boot disk is partitioned"
il_assert_grep "$LOGA" "\[ahci\] PASS: SATA read/write DMA works" \
    "and still verifies the DMA write path (save/write/readback/restore)"
il_assert_grep "$LOGA" "\[diskfs\] partitioned disk is not an AUFS scratch disk" \
    "diskfs refuses to format the boot disk"
il_assert_grep "$LOGA" "\[fat32\] found FAT32 partition at LBA 256 \(via GPT\)" \
    "fat32 mounts the boot disk's own ESP (GPT leg)"
il_assert_no_grep "$LOGA" "formatting" \
    "no formatter ran anywhere during the boot"
il_assert_grep "$LOGA" "KERNEL.ELF" \
    "the bootloader's KERNEL.ELF is visible in /fat"
il_assert_grep "$LOGA" "Hello from AuraLite" \
    "the system booted to userspace from its own ESP-hosted kernel"

# ---- Lane B: partitioned disk with no FAT32 volume ------------------------
LOGB="$IL_LOGDIR/ota_bootvol_b.log"
IL_LAST_LOG="$LOGB"
DISKB="$IL_LOGDIR/ota_nofat_mbr.img"
rm -f "$LOGB" "$DISKB"

# An MBR with one Linux (0x83) entry at LBA 2048 and no filesystem bytes
# anywhere: a table-bearing disk that is NOT ours to format.
python3 - "$DISKB" <<'PY'
import sys
img = bytearray(16 * 1024 * 1024)
mbr = bytearray(512)
mbr[510:512] = b'\x55\xaa'
# entry 0, written in place -- a slice copy would silently discard it
# (exactly the bug the first run of this case tripped over: the "table"
# never made it into the image and the disk came out table-less).
o = 446
mbr[o + 0] = 0x80          # bootable
mbr[o + 4] = 0x83          # type: Linux
mbr[o + 8:o + 12]  = (2048).to_bytes(4, 'little')    # start LBA
mbr[o + 12:o + 16] = (16384).to_bytes(4, 'little')   # sector count
img[0:512] = mbr
with open(sys.argv[1], 'wb') as f:
    f.write(img)
PY

# The feeder is prompt-gated, and the mounts happen during boot BEFORE the
# first prompt -- at prompt time the disks are already in their final state.
il_send "exit"

il_run_qemu "$LOGB" 60 \
    -drive "file=$DISKB,format=raw,if=none,id=nofatdisk,snapshot=on" \
    -device ahci,id=ahci1 -device ide-hd,drive=nofatdisk,bus=ahci1.0

il_assert_grep "$LOGB" "\[fat32\] partitioned disk has no FAT32 volume; not formatting a partitioned disk" \
    "fat32 refuses a table-bearing disk without a FAT32 volume"
il_assert_no_grep "$LOGB" "\[fat32\] formatting|\[diskfs\] formatting" \
    "neither slot-0 formatter touched the partitioned disk"

# ---- Lane C: table-less scratch disks keep the legacy semantics -----------
LOGC="$IL_LOGDIR/ota_bootvol_c.log"
IL_LAST_LOG="$LOGC"
DISKC="$IL_LOGDIR/ota_scratch.img"
rm -f "$LOGC" "$DISKC"
il_make_disk "$DISKC" 16 "AURSCRTCH"

il_send "ls /fat"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOGC" 60 \
    -drive "file=$DISKC,format=raw,if=none,id=scratchdisk,snapshot=on" \
    -device ahci,id=ahci2 -device ide-hd,drive=scratchdisk,bus=ahci2.0

il_assert_grep "$LOGC" "\[fat32\] formatting default FAT32 volume at LBA 64" \
    "table-less scratch disks still auto-format (the SH5d contract)"
il_assert_grep "$LOGC" "\[fat32\] mounted FAT32 at /fat" \
    "and mount as /fat exactly as before"

il_summary
