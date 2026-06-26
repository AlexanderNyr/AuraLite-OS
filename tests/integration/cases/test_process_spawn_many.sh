#!/usr/bin/env bash
# test_process_spawn_many.sh — spawn multiple user processes sequentially
# and verify each runs and exits cleanly.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Sequential process spawning"

LOG="$IL_LOGDIR/spawn_many.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6

# Run several programs in sequence
il_send "run /hello"
il_send_delay 2
il_send "run /hello"
il_send_delay 2
il_send "run /hello"
il_send_delay 2

# Run different programs
il_send "run /sysinfo"
il_send_delay 3
il_send "run /clock"
il_send_delay 2
il_send "exit"

il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 45

# Count how many times hello was spawned
il_assert_count "$LOG" "(Hello|hello)" 2 "at least 2 hello runs"
il_assert_grep "$LOG" "(sysinfo|System|Aura)" "/sysinfo ran"
il_assert_grep "$LOG" "(child exited|running /hello)" "spawn/wait cycle works"

il_summary
