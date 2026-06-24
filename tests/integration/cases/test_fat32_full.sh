#!/usr/bin/env bash
# test_fat32_full.sh — exercise the full FAT32 surface: subdirs, LFN,
# mkdir/rmdir/unlink/rename/truncate, from the user-mode shell.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FAT32 (subdirs, LFN, mkdir/rmdir/rm/mv/stat)"

DISK="$IL_BUILD/fat32-full.img"
rm -f "$DISK"
il_make_disk "$DISK" 32 "FATFULL_"

LOG="$IL_LOGDIR/fat32_full.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

MARK="FULLFAT_$$_$(date +%s)"

il_send_delay 8
# 1. mkdir + ls (should show MYDIR/)
il_send "mkdir /fat/MYDIR"
il_send_delay 1
il_send "ls /fat"
il_send_delay 1
# 2. nested file create + write + cat
il_send "write /fat/MYDIR/FILE.TXT $MARK"
il_send_delay 1
il_send "cat /fat/MYDIR/FILE.TXT"
il_send_delay 1
# 3. stat
il_send "stat /fat/MYDIR/FILE.TXT"
il_send_delay 1
# 4. rename
il_send "mv /fat/MYDIR/FILE.TXT /fat/MYDIR/RENAMED.TXT"
il_send_delay 1
il_send "ls /fat/MYDIR"
il_send_delay 1
# 5. rm + rmdir
il_send "rm /fat/MYDIR/RENAMED.TXT"
il_send_delay 1
il_send "rmdir /fat/MYDIR"
il_send_delay 1
il_send "ls /fat"
il_send_delay 1
# 6. LFN — kernel self-test already creates ALongFileName.txt; confirm visible
il_send "ls /fat"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 35 \
    -drive "file=$DISK,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0"

# Kernel side
il_assert_grep "$LOG" "\[fat32\] PASS:"             "FAT32 kernel self-test (incl LFN/subdir/rename)"

# Shell side
il_assert_grep "$LOG" "mkdir: created /fat/MYDIR"    "mkdir created subdir"
il_assert_grep "$LOG" "MYDIR/"                       "ls shows MYDIR as directory"
il_assert_grep "$LOG" "$MARK"                        "wrote+read marker through nested file"
il_assert_grep "$LOG" "Type:    regular file"        "stat reports type"
il_assert_grep "$LOG" "Mode:    0644"                "stat reports mode 0644"
il_assert_grep "$LOG" "mv: /fat/MYDIR/FILE.TXT"      "rename succeeded"
il_assert_grep "$LOG" "RENAMED.TXT"                  "renamed file visible"
il_assert_grep "$LOG" "rm: removed /fat/MYDIR/RENAMED.TXT" "unlink succeeded"
il_assert_grep "$LOG" "rmdir: removed /fat/MYDIR"    "rmdir succeeded"
il_assert_grep "$LOG" "ALongFileName.txt"            "LFN visible in directory listing"

il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"       "no exception during FAT32 ops"

il_summary
