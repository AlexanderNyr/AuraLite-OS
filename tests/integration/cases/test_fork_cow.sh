#!/usr/bin/env bash
# test_fork_cow.sh — verify Copy-on-Write (COW) fork() mechanics and address space cloning.
# H3 gate criterion: verify fork() clones user space via paging_clone_user_space,
# child executes successfully in user mode, and memory is reaped cleanly on exit.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Copy-on-Write (COW) fork() mechanics (H3)"

LOG="$IL_LOGDIR/fork_cow.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "echo COW_FORK_TEST &"
il_send_delay 3
il_send "jobs"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

# Verify kernel log messages from do_fork() and fork_child_entry()
il_assert_grep "$LOG" "\[proc\] fork: cloning address space"     "fork: cloning address space initiated"
il_assert_grep "$LOG" "\[proc\] fork: cloned to PML4 phys"       "fork: cloned to new PML4 successfully"
il_assert_grep "$LOG" "\[proc\] fork: parent returns child PID"  "fork: parent received child PID"
il_assert_grep "$LOG" "\[proc\] fork child starting, jumping"    "fork: child jumping to user mode via sysret"

# Verify child process executed the command successfully in user mode
il_assert_grep "$LOG" "COW_FORK_TEST"                            "fork: child successfully executed echo in user mode"

# Ensure no panics or exceptions occurred during fork or COW handling
il_assert_no_grep "$LOG" "PANIC"                                 "no kernel panic during fork/COW"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                   "no unhandled exception during fork/COW"

il_summary