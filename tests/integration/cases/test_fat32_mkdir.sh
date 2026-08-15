#!/usr/bin/env bash
# test_fat32_mkdir.sh — FAT32 subdirectory creation, listing, and removal.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FAT32 mkdir/rmdir"

# AUDIT_A2: this case asserted against /fat while attaching no disk at all.
# The guest printed "[fat32] no AHCI disk; FAT32 disabled", every command
# failed, and the case was red for a reason that had nothing to do with
# FAT32 mkdir.  Attach a real FAT32 volume over AHCI, the way
# test_ahci_large_read.sh does.
LOG="$IL_LOGDIR/fat32_mkdir.log"
DISK="$IL_BUILD/fat32_mkdir_disk.img"
PART="$IL_BUILD/fat32_mkdir_part.img"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

rm -f "$DISK" "$PART"
dd if=/dev/zero of="$PART" bs=1M count=48 2>/dev/null
mformat -i "$PART" -F -h 32 -s 32 -t 96 ::
# 64 zero sectors ahead of the filesystem, matching the layout the AHCI
# cases use so the guest finds the BPB where it expects it.
dd if=/dev/zero of="$DISK" bs=512 count=64 2>/dev/null
cat "$PART" >> "$DISK"

il_send_delay 6
il_send "ls /fat"
il_send_delay 1
il_send "mkdir /fat/testdir"
il_send_delay 1
il_send "ls /fat"
il_send_delay 1
il_send "write /fat/testdir/file.txt inside_subdir"
il_send_delay 1
il_send "cat /fat/testdir/file.txt"
il_send_delay 1
il_send "rmdir /fat/testdir"
il_send_delay 1
il_send "ls /fat"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 60 \
    -drive "id=fatdisk,file=$DISK,format=raw,if=none,snapshot=on" \
    -device ahci,id=ahci \
    -device ide-hd,drive=fatdisk,bus=ahci.0

# The filesystem has to be mounted at all -- without this the rest of the
# assertions can pass on echoed command text alone.
il_assert_no_grep "$LOG" "no AHCI disk; FAT32 disabled" \
    "FAT32 volume is actually mounted"

il_assert_grep "$LOG" "mkdir: created /fat/testdir"  "mkdir succeeded"
il_assert_grep "$LOG" "wrote /fat/testdir/file.txt"  "write in subdir"

# "ls shows testdir" used to match the word anywhere in the log -- including
# the echo of the command that created it, so it passed even when mkdir had
# failed.  Require the listing line itself.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if awk '/^ls \/fat$/{f=1;next} /^auralite#/{f=0} f && /testdir/{found=1} END{exit !found}' "$LOG"; then
    il_pass "ls /fat lists testdir"
else
    il_fail "testdir does not appear in an ls /fat listing"
fi

il_assert_grep "$LOG" "inside_subdir"  "read back from the subdirectory"

il_summary
