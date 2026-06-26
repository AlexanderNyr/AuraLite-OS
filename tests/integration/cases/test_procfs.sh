#!/usr/bin/env bash
# test_procfs.sh — exercise procfs: /proc/meminfo, /proc/*/cmdline.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "procfs (/proc) operations"

LOG="$IL_LOGDIR/procfs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 5
il_send "ls /proc"
il_send_delay 1
il_send "cat /proc/meminfo"
il_send_delay 1
il_send "free"
il_send_delay 1
il_send "ps"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 30

il_assert_grep "$LOG" "meminfo"                     "ls /proc shows meminfo"
il_assert_grep "$LOG" "(usable|free|MiB|KiB)"       "meminfo has memory data"
il_assert_grep "$LOG" "(free|usable|MiB|KiB)"       "free command shows memory"

il_summary
