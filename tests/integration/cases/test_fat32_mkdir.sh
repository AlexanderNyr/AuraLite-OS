#!/usr/bin/env bash
# test_fat32_mkdir.sh — FAT32 subdirectory creation, listing, and removal.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FAT32 mkdir/rmdir"

LOG="$IL_LOGDIR/fat32_mkdir.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

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

il_run_qemu "$LOG" 40

il_assert_grep "$LOG" "mkdir: created /fat/testdir"          "mkdir succeeded"
il_assert_grep "$LOG" "testdir"                               "ls shows testdir"
il_assert_grep "$LOG" "wrote /fat/testdir/file.txt"           "write in subdir"
il_assert_grep "$LOG" "inside_subdir"                         "read from subdir"

il_summary
