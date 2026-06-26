#!/usr/bin/env bash
# test_diskfs.sh — exercise /disk: write, read, persistence check.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "diskfs (/disk) operations"

LOG="$IL_LOGDIR/diskfs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

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

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "$MARK"                       "disk write/read round-trip"
il_assert_grep "$LOG" "wrote /disk/persist.txt"     "write succeeded"
il_assert_grep "$LOG" "persist.txt"                 "ls shows the file"

il_summary
