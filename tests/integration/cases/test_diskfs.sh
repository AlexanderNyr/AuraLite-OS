#!/usr/bin/env bash
# test_diskfs.sh — exercise /disk: write, read, persistence check.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "diskfs (/disk) operations"

# AUDIT_A2: this case asserted against /disk while attaching no disk.  The
# guest printed "[diskfs] no AHCI disk available; /disk not mounted", every
# command failed, and two of the three assertions still passed -- "$MARK"
# and "persist.txt" both matched the echo of the write command itself, not
# any file.  Attach a real AHCI disk, and match the output rather than the
# input.
LOG="$IL_LOGDIR/diskfs.log"
DISK="$IL_BUILD/diskfs_test.img"
PART="$IL_BUILD/diskfs_part.img"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

rm -f "$DISK" "$PART"
dd if=/dev/zero of="$PART" bs=1M count=48 2>/dev/null
mformat -i "$PART" -F -h 32 -s 32 -t 96 ::
dd if=/dev/zero of="$DISK" bs=512 count=64 2>/dev/null
cat "$PART" >> "$DISK"

MARK="DISKMARK_$$_$(date +%s)"

il_send_delay 6
il_send "ls /disk"
il_send_delay 1
il_send "write /disk/persist.txt $MARK"
il_send_delay 1
il_send "cat /disk/persist.txt"
il_send_delay 1
il_send "ls /disk"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 60 \
    -drive "id=diskimg,file=$DISK,format=raw,if=none,snapshot=on" \
    -device ahci,id=ahci \
    -device ide-hd,drive=diskimg,bus=ahci.0

il_assert_no_grep "$LOG" "no AHCI disk available" \
    "/disk is actually mounted"
il_assert_grep "$LOG" "wrote /disk/persist.txt"  "write succeeded"

# The mark must come back from `cat`, not from the echoed write command.
# Count it: it appears once as input, and a genuine round-trip makes two.
# EXPECTED-RED, intermittently: this reveals a real defect in the FAT32
# write path, not a flaw in the test.  Roughly one run in three, `cat`
# returns directory-entry bytes instead of file contents:
#
#     cat /disk/persist.txt
#     AURALOG TXT ^@^@^M^@...        <- raw FAT directory entry
#
# The write itself always lands (the file grows 24 -> 26 bytes and appears
# in `ls`), so the bug is on the read-back or the cache-flush side.  The
# old form of this case could never have seen it: it matched "$MARK"
# anywhere in the log and the echo of the write command satisfied that
# every time.
#
# Left asserting the correct behaviour deliberately.  Tracked as A2-R1 in
# TESTAUDIT_PLAN.md; masking it with a retry loop would re-hide exactly
# what this phase exists to expose.
il_assert_count "$LOG" "$MARK" 2 \
    "the written mark is read back from the file (A2-R1: flaky, real bug)"

# Likewise, require the name in an `ls /disk` listing rather than anywhere.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if awk '/^ls \/disk$/{f=1;next} /^auralite#/{f=0} f && /persist.txt/{found=1} END{exit !found}' "$LOG"; then
    il_pass "ls /disk lists persist.txt"
else
    il_fail "persist.txt does not appear in an ls /disk listing"
fi

il_summary
