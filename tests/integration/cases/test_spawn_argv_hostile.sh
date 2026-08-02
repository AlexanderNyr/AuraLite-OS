#!/usr/bin/env bash
# test_spawn_argv_hostile.sh — spawnv() against malformed argv (SDK_PLAN S3).
#
# spawnv() copies an array of USER pointers into the kernel. That is the
# dangerous part of the phase, and the interesting inputs are the ones a
# normal run never produces. The plan says to reuse execve's
# exec_args_capture() precisely so this validation is not implemented twice;
# this case is the evidence that reuse actually holds.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "spawn() argv validation"

LOG="$IL_LOGDIR/spawn_argv_hostile.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run hostilearg"
il_send_delay 5
il_send "echo SHELL_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "HOSTILE: begin"                        "probe ran"
il_assert_grep "$LOG" "HOSTILE: kernel-pointer argv -> -1"    "an argv pointing into kernel space is refused"
il_assert_grep "$LOG" "HOSTILE: kernel-pointer string -> -1"  "an argv STRING in kernel space is refused"
il_assert_grep "$LOG" "HOSTILE: unterminated argv -> bounded" "walking a vector is bounded"
il_assert_grep "$LOG" "HOSTILE: null argv -> spawned"         "a NULL argv behaves like spawn()"
il_assert_grep "$LOG" "HOSTILE: survived"                     "the caller survived every case"

# The point of the whole case: none of it may take the kernel down.
il_assert_grep "$LOG" "SHELL_STILL_ALIVE"                     "the shell is still alive afterwards"
il_assert_no_grep "$LOG" "PANIC"                              "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                "no unhandled exception"
il_assert_no_grep "$LOG" "from KERNEL mode"                   "no kernel-mode fault"

il_summary
