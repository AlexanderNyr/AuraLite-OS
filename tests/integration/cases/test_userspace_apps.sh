#!/usr/bin/env bash
# test_userspace_apps.sh — launch various userspace programs and verify they produce output.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Userspace applications"

LOG="$IL_LOGDIR/userspace_apps.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6

# hello
il_send "run /hello"
il_send_delay 2

# sysinfo
il_send "run /sysinfo"
il_send_delay 2
il_send "exit"
il_send_delay 1

# calc
il_send "run /calc"
il_send_delay 1
il_send "2+3*4"
il_send_delay 1
il_send "100/7"
il_send_delay 1
il_send "exit"
il_send_delay 1

# guess
il_send "run /guess"
il_send_delay 2
il_send "50"
il_send_delay 1
il_send "exit"
il_send_delay 1

il_send "exit"

il_run_qemu "$LOG" 50

il_assert_grep "$LOG" "(Hello|hello)"                "/hello output"
il_assert_grep "$LOG" "(sysinfo|System|cpu|Aura)"     "/sysinfo output"
il_assert_grep "$LOG" "14"                            "calc: 2+3*4 = 14"
il_assert_grep "$LOG" "(14|15)"                       "calc: 100/7 ≈ 14"

il_summary
