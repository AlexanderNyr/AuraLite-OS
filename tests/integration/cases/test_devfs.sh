#!/usr/bin/env bash
# test_devfs.sh — exercise devfs: /dev/null discards, /dev/zero reads, errors on invalid.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "devfs (/dev) operations"

LOG="$IL_LOGDIR/devfs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 5
il_send "ls /dev"
il_send_delay 1
il_send "cat /dev/null"
il_send_delay 1
il_send "write /dev/null this_should_be_discarded"
il_send_delay 1
il_send "ls /dev"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 30

il_assert_grep "$LOG" "null"           "ls /dev shows null"
il_assert_grep "$LOG" "zero"           "ls /dev shows zero"

il_summary
