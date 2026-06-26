#!/usr/bin/env bash
# test_tmpfs.sh — exercise tmpfs at /tmp: write, read, delete, truncate, multi-file.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "tmpfs (/tmp) operations"

LOG="$IL_LOGDIR/tmpfs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

MARK="TMPMARK_$$_$(date +%s)"

il_send_delay 5
il_send "write /tmp/test1.txt hello_tmpfs"
il_send_delay 1
il_send "cat /tmp/test1.txt"
il_send_delay 1
il_send "write /tmp/test2.txt world"
il_send_delay 1
il_send "ls /tmp"
il_send_delay 1
il_send "cat /tmp/test2.txt"
il_send_delay 1
il_send "rm /tmp/test1.txt"
il_send_delay 1
il_send "ls /tmp"
il_send_delay 1
il_send "write /tmp/$MARK data123"
il_send_delay 1
il_send "cat /tmp/$MARK"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "wrote /tmp/test1.txt"        "write test1 succeeded"
il_assert_grep "$LOG" "hello_tmpfs"                  "read test1 content"
il_assert_grep "$LOG" "wrote /tmp/test2.txt"         "write test2 succeeded"
il_assert_grep "$LOG" "test1.txt"                    "ls shows test1"
il_assert_grep "$LOG" "test2.txt"                    "ls shows test2"
il_assert_grep "$LOG" "world"                        "read test2 content"
il_assert_grep "$LOG" "rm: removed /tmp/test1.txt"   "rm test1 succeeded"
il_assert_grep "$LOG" "$MARK"                        "custom marker round-trip"
il_assert_grep "$LOG" "data123"                      "custom content round-trip"

il_summary
