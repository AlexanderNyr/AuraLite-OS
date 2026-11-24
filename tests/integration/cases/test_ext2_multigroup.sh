#!/usr/bin/env bash
# test_ext2_multigroup.sh — FSMODERN M4: a volume too big for one bitmap
# block must get more than one group, and the result must survive Linux's
# own fsck.
#
# Why this case exists: one bitmap block addresses 8 * block_size blocks
# (8192 at 1 KiB).  The pre-M4 formatter wrote blocks_per_group = total_blocks
# regardless, so a 64 MiB volume declared ONE group of 65536 blocks whose
# bitmap could reach 8192 of them; the superblock claimed 65468 free blocks
# of which 8171 existed, and the allocator scanned past its bitmap buffer
# looking for the rest.  test_ext2.sh only formats 4 MiB, which fits one
# group and therefore never met the bug.  This case is the size that does.
#
# Asserted, in order:
#   - the kernel formats from the device's own size (65536 blocks of 1 KiB);
#   - the volume has 8 groups, not one impossible one;
#   - the mount-time consistency walk says CLEAN (per-group bitmaps and the
#     free counters agree -- the exact drift the old geometry produced);
#   - the kernel self-test passes on it;
#   - e2fsprogs' fsck finds nothing to fix in what the kernel wrote.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "ext2 (64 MiB: multi-group geometry + Linux fsck)"

DISK0="$IL_BUILD/ext2-mg-disk0.img"
DISK="$IL_BUILD/ext2-multigroup.img"
LOG="$IL_LOGDIR/ext2_multigroup.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

FSCK=""
for p in /sbin/fsck.ext2 /usr/sbin/fsck.ext2; do [ -x "$p" ] && FSCK="$p"; done

rm -f "$DISK"
truncate -s 64M "$DISK" 2>/dev/null || dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
# A blank volume: no magic at offset 1024, so the kernel formats it.
rm -f "$DISK0"
il_make_disk "$DISK0" 16 "AURALHCI"

il_send_delay 8
il_send "ls /ext2"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 40 \
    -drive "file=$DISK0,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0" \
    -drive "file=$DISK,format=raw,if=none,id=ext2disk" \
    -device "ide-hd,drive=ext2disk,bus=ahci0.1"

il_assert_grep "$LOG" "formatting fresh volume \\(65536 blocks of 1 KiB" \
    "the formatter used the device's own 64 MiB"
il_assert_grep "$LOG" "65536 blocks of 1024 B in 8 group\\(s\\) of 8192 blocks" \
    "a 64 MiB volume got 8 groups, not one impossible one"
il_assert_grep "$LOG" "\[fscheck\] ext2: CLEAN \\(65536 blocks, 1024 inodes, 8 groups\\)" \
    "the consistency walk says CLEAN on the new geometry"
il_assert_grep "$LOG" "\[ext2\] mount check: CLEAN" \
    "mount check CLEAN (the old geometry reported free-block findings)"
il_assert_grep "$LOG" "\[ext2\] PASS:" \
    "self-test on the multi-group volume"

# The part test_ext2.sh cannot do at 4 MiB: Linux's own checker on the
# geometry the kernel wrote.  `fsck -fn` exits 0 and prints no "WARNING:
# Filesystem still has errors" when there is nothing to fix.
if [ -z "$FSCK" ]; then
    echo "  ${C_YELLOW}skip: e2fsprogs not installed (need fsck.ext2)${C_RESET}"
else
    if "$FSCK" -fn "$DISK" >"$IL_LOGDIR/ext2_multigroup_fsck.log" 2>&1; then
        il_pass "Linux fsck finds nothing to fix in the kernel-formatted volume"
    else
        il_fail "Linux fsck reported errors: $(tail -3 "$IL_LOGDIR/ext2_multigroup_fsck.log" | tr '\n' ' ')"
    fi
    if grep -q "WARNING: Filesystem still has errors" "$IL_LOGDIR/ext2_multigroup_fsck.log"; then
        il_fail "fsck left errors behind"
    else
        il_pass "fsck reports no leftover errors"
    fi
fi

il_summary
