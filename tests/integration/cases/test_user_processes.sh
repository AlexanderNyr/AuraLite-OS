#!/usr/bin/env bash
# test_user_processes.sh — per-process address spaces and spawn().

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "User processes (spawn + isolated AS)"

LOG="$IL_LOGDIR/user_procs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "run /hello"
il_send_delay 2
il_send "run /calc"
il_send_delay 1
il_send "1+1"
il_send_delay 1
il_send "q"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 25

il_assert_grep "$LOG" "\[proc\] PASS: /hello ran in isolated address space" \
                                                       "kernel process self-test"
il_assert_grep "$LOG" "(Hello|hello)"                  "/hello output via spawn"

# Sanity: spawning a second program (calc) didn't crash the shell.
il_assert_grep "$LOG" "auralite#"                      "shell still alive after spawns"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"         "no exception during spawn"

il_summary
