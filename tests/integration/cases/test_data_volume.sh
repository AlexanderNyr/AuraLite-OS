#!/usr/bin/env bash
# test_data_volume.sh — FSMODERN_PLAN.md M1: the first durability proof.
#
# Before M1 this tree had no filesystem that was both mounted on a default
# boot and survived a reboot: /disk, /fat and /ext2 were reachable but flat
# or exchange-shaped, and /home, /opt and /documents were tmpfs.  This case
# is the gate that says that is no longer true, and it says it the only way
# that means anything: write a file, POWER THE MACHINE OFF, boot it again
# from scratch, read the file back.
#
# The recipe is tools/run_qemu.sh's, because a durability test that boots a
# different machine than the user does proves nothing about the user's
# machine: exchange FAT32 on blk0, ext2 on blk1, the persistent volume on
# blk2, and the format gate open (a blank lab disk has no superblock yet).
#
# Two il_run_qemu calls, not two hand-rolled QEMU pipes.  The first draft
# drove QEMU directly with `sleep N; printf ...` and lost the first bytes of
# the first command on EVERY run ("write /data/note.txt X" arrived as
# "te.txt X", as "ata/note.txt X", as ".txt X") -- a transport race that has
# nothing to do with the filesystem and everything to do with guessing when
# a prompt appears.  lib.sh already solves exactly this: il_send() waits for
# the "auralite#" prompt in the live log before each line.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "/data: a persistent volume across a real power-off"

LOG1="$IL_LOGDIR/data_volume_boot1.log"
LOG2="$IL_LOGDIR/data_volume_boot2.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

DISK="$IL_LOGDIR/data_volume.img"
D0="$IL_LOGDIR/data_volume_d0.img"
D1="$IL_LOGDIR/data_volume_d1.img"
rm -f "$DISK" "$D0" "$D1"
truncate -s 64M "$DISK"
truncate -s 64M "$D0"
truncate -s 64M "$D1"

# The unique marker makes a stale disk unable to pass: on a freshly created
# image, only THIS run's write can put THIS run's marker on the volume.
MARK="DATAMARK_$$_$(date +%s)"

DISKS=( -drive "id=d0,file=$D0,format=raw,if=none" -device ahci,id=ahci0
        -device ide-hd,drive=d0,bus=ahci0.0
        -drive "id=d1,file=$D1,format=raw,if=none"
        -device ide-hd,drive=d1,bus=ahci0.1
        -drive "id=dv,file=$DISK,format=raw,if=none"
        -device ide-hd,drive=dv,bus=ahci0.2
        -fw_cfg name=opt/auralite.fsformat,string=1 )

il_section "boot 1: mount /data and write $MARK"
il_send "write /data/note.txt $MARK"
il_send "sync"
il_send "ls /data"
il_send "exit"
il_run_qemu "$LOG1" 90 "${DISKS[@]}"

il_assert_grep "$LOG1" "\[data\] PASS: .*mounted at /data" \
    "/data mounted on a persistent filesystem"
il_assert_grep "$LOG1" "PERSISTENT" \
    "the receipt claims persistence (not tmpfs)"
il_assert_grep "$LOG1" "wrote /data/note.txt" \
    "the file was written"
il_assert_grep "$LOG1" "\[btrfs\] fsync: persisted tree root" \
    "sync() reached the filesystem's own sync, not just the block cache"
# M1's own bug, pinned: `dir_emit()` never filled de->size, so `ls /data`
# printed "(0 bytes)" for a file `cat` then read correctly -- a listing that
# says every file is empty reads like corruption and is only an unfilled
# field.  Asserted with a FIXED-string search for the literal " bytes)",
# because a regex here costs an escaping layer for nothing.
# IL_ASSERT_COUNT is bumped by hand here: the library's il_pass/il_fail do
# not touch it (only the il_assert_* wrappers do), and a case that calls
# them directly prints "9/8 assertions passed" -- one more PASS than the
# total it divides by.  test_fsformat_knob.sh had the same bug.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if grep -qF "note.txt  (0 bytes)" "$LOG1"; then
    il_fail "the listing reports a real size (it says 0 bytes)"
elif grep -qF "note.txt  (" "$LOG1" && grep -qF " bytes)" "$LOG1"; then
    il_pass "the listing reports a real size (not 0)"
else
    il_fail "the listing reports a real size (note.txt not listed)"
fi

il_section "boot 2: power-on again, read it back"
il_send "ls /data"
il_send "cat /data/note.txt"
il_send "exit"
il_run_qemu "$LOG2" 90 "${DISKS[@]}"

il_assert_grep "$LOG2" "\[data\] PASS: .*mounted at /data" \
    "/data mounted again on the fresh boot"
il_assert_grep "$LOG2" "note.txt" \
    "the file survived the power-off"
il_assert_grep "$LOG2" "$MARK" \
    "the CONTENT survived the power-off, byte for byte"

# A stale disk cannot satisfy this: the marker is minted per run.
il_assert_count "$LOG2" "$MARK" 1 \
    "the marker in boot 2 is this run's marker"

il_summary
