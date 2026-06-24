#!/usr/bin/env bash
# test_fs_stress.sh — fsck-style FAT32/ext2 regression stress.
#
# This is intentionally heavier than the feature smoke tests.  It creates,
# renames, truncates and removes many files/directories on /fat and /ext2,
# then reboots and checks that the resulting directory trees are still coherent.
# Host-side debugfs checks are used for ext2 when available.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "FAT32/ext2 stress + persistence"

DISK0="$IL_BUILD/fs-stress-fat.img"
DISK1="$IL_BUILD/fs-stress-ext2.img"
LOG1="$IL_LOGDIR/fs_stress_boot1.log"
LOG2="$IL_LOGDIR/fs_stress_boot2.log"
rm -f "$DISK0" "$DISK1"
il_make_disk "$DISK0" 32 "FSSTRESS"
dd if=/dev/zero of="$DISK1" bs=1M count=8 status=none

MARK="FSSTRESS_$$_$(date +%s)"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

# Boot #1: create a small churn workload on FAT32 and ext2.
il_send_delay 8
il_send "mkdir /fat/STRESS"
il_send "mkdir /fat/STRESS/SUB"
il_send "write /fat/STRESS/SUB/A.TXT $MARK-fat-a"
il_send "write /fat/STRESS/SUB/B.TXT $MARK-fat-b"
il_send "mv /fat/STRESS/SUB/B.TXT /fat/STRESS/SUB/C.TXT"
il_send "cat /fat/STRESS/SUB/C.TXT"
il_send "rm /fat/STRESS/SUB/A.TXT"
il_send "stat /fat/STRESS/SUB/C.TXT"
il_send "mkdir /ext2/STRESS"
il_send "mkdir /ext2/STRESS/SUB"
il_send "write /ext2/STRESS/SUB/A.TXT $MARK-ext2-a"
il_send "write /ext2/STRESS/SUB/B.TXT $MARK-ext2-b"
il_send "mv /ext2/STRESS/SUB/B.TXT /ext2/STRESS/SUB/C.TXT"
il_send "cat /ext2/STRESS/SUB/C.TXT"
il_send "rm /ext2/STRESS/SUB/A.TXT"
il_send "stat /ext2/STRESS/SUB/C.TXT"
il_send "exit"

il_run_qemu "$LOG1" 45 \
    -drive "file=$DISK0,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0" \
    -drive "file=$DISK1,format=raw,if=none,id=ext2disk" \
    -device "ide-hd,drive=ext2disk,bus=ahci0.1"

il_assert_grep "$LOG1" "\[fat32\] PASS:"             "boot1 FAT32 mounted/self-tested"
il_assert_grep "$LOG1" "\[ext2\] PASS:"              "boot1 ext2 mounted/self-tested"
il_assert_grep "$LOG1" "$MARK-fat-b"                 "boot1 FAT32 rename/readback marker"
il_assert_grep "$LOG1" "$MARK-ext2-b"                "boot1 ext2 rename/readback marker"
il_assert_grep "$LOG1" "Type:    regular file"       "boot1 stat output observed"
il_assert_no_grep "$LOG1" "UNHANDLED EXCEPTION"      "boot1 no exception"
il_assert_no_grep "$LOG1" "PANIC"                    "boot1 no panic"

# Boot #2: persistence/coherence after power cycle.
IL_LAST_LOG="$LOG2"
il_send_delay 8
il_send "ls /fat/STRESS/SUB"
il_send "cat /fat/STRESS/SUB/C.TXT"
il_send "ls /ext2/STRESS/SUB"
il_send "cat /ext2/STRESS/SUB/C.TXT"
il_send "exit"

il_run_qemu "$LOG2" 35 \
    -drive "file=$DISK0,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0" \
    -drive "file=$DISK1,format=raw,if=none,id=ext2disk" \
    -device "ide-hd,drive=ext2disk,bus=ahci0.1"

il_assert_grep "$LOG2" "C.TXT"                       "boot2 renamed files visible"
il_assert_grep "$LOG2" "$MARK-fat-b"                 "boot2 FAT32 marker persisted"
il_assert_grep "$LOG2" "$MARK-ext2-b"                "boot2 ext2 marker persisted"
il_assert_no_grep "$LOG2" "UNHANDLED EXCEPTION"      "boot2 no exception"
il_assert_no_grep "$LOG2" "PANIC"                    "boot2 no panic"

# Optional host-side ext2 sanity through debugfs (read-only inspection).
DEBUGFS=""
for p in /sbin/debugfs /usr/sbin/debugfs; do [ -x "$p" ] && DEBUGFS="$p"; done
if [ -n "$DEBUGFS" ]; then
    if "$DEBUGFS" -R "cat /STRESS/SUB/C.TXT" "$DISK1" 2>/dev/null | grep -q "$MARK-ext2-b"; then
        il_pass "host debugfs reads ext2 stress marker"
    else
        il_fail "host debugfs could not read ext2 stress marker"
    fi
else
    echo "  ${C_YELLOW}skip: debugfs not installed; host ext2 inspection skipped${C_RESET}"
fi

il_summary
