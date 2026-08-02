#!/usr/bin/env bash
# test_spawn_argv.sh — spawn() forwards arguments (SDK_PLAN phase S3).
#
# execve() has always carried argv correctly; spawn() could not, so programs
# passed arguments through a file the child agreed to read (/tmp/apm.args).
# This asserts the syscall does it, and that the workaround is gone.
#
# The same program, /tests/argv_echo, is used by the kernel's own execve
# self-test at boot, so the two paths can be compared against one shared
# reference rather than against two different notions of "correct".

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "spawn() argument forwarding"

LOG="$IL_LOGDIR/spawn_argv.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run argv_echo alpha beta 42"
il_send_delay 3
# A bare command, with no `run`, must forward too -- leaving one path
# hardcoded is how the two end up behaving differently.
il_send "argv_echo solo"
il_send_delay 3
# `run <prog>` with nothing after it.  The program now receives argc=1 with
# argv[0] set to its own name, which is what POSIX programs expect and what
# execve has always produced.  Before S3 it saw argc=0 -- it could not even
# learn what it had been invoked as.
il_send "run argv_echo"
il_send_delay 3
# apm now receives its subcommand as argv rather than through a temp file.
il_send "apm list"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 45

# --- with arguments ---
il_assert_grep "$LOG" "ARGV_ECHO argv\[1\]=alpha"    "argv[1] forwarded"
il_assert_grep "$LOG" "ARGV_ECHO argv\[2\]=beta"     "argv[2] forwarded"
il_assert_grep "$LOG" "ARGV_ECHO argv\[3\]=42"       "argv[3] forwarded"
il_assert_grep "$LOG" "ARGV_ECHO argv\[0\]=argv_echo" "argv[0] is the program name"
il_assert_grep "$LOG" "ARGV_ECHO argv_terminated=1"  "the vector is NULL-terminated"

# --- a bare command forwards too ---
il_assert_grep "$LOG" "ARGV_ECHO argv\[1\]=solo"     "a bare command forwards arguments"

# --- and no arguments still works ---
il_assert_grep "$LOG" "ARGV_ECHO argc=1"             "run <prog> alone gives argc=1 with argv[0]"

# --- the workaround is gone ---
il_assert_grep "$LOG" "NAME"                         "apm ran with an argv subcommand"
il_assert_no_grep "$LOG" "apm.args"                  "no temp-file argument passing"

il_assert_no_grep "$LOG" "PANIC"                     "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"       "no unhandled exception"

il_summary
