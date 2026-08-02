#!/usr/bin/env bash
# test_search_path.sh — the program search path (FSLAYOUT_PLAN phase F2).
#
# THE POINT OF THIS FILE IS THAT IT MUST NOT CHANGE IN F3.
#
# Every assertion here is written against a program NAME, never a path. It
# passes today, with everything flat in the root, and it must pass unmodified
# after F3 moves the programs into /apps, /demos and /tests. A test that has
# to be edited alongside the move proves nothing about the move; this one is
# the evidence that the move is safe.
#
# The one deliberate exception is the absolute-path check, which must keep
# working precisely because an explicit path bypasses the search.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "program search path"

LOG="$IL_LOGDIR/search_path.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
# By name, through `run`.
il_send "run hello"
il_send_delay 2
# By name, with no `run` at all.
il_send "sysinfo"
il_send_delay 2
# By absolute path — must bypass the search and still work.
il_send "run /bin/hello"
il_send_delay 2
# A name that is nowhere: the failure must say where it looked.
il_send "run definitely_not_a_program"
il_send_delay 1
# A built-in must still win over any program of the same name.
il_send "echo BUILTIN_STILL_WINS"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 35

# The bare name resolved to a concrete path...
il_assert_grep "$LOG" "running /bin/hello in isolated" "run <name> resolved to a path"
# ...and the program at that path actually ran to completion.
il_assert_grep "$LOG" "reaped '/bin/hello'"           "the resolved program ran and exited"
# A name with no `run` in front of it: /sysinfo prints its banner.
il_assert_grep "$LOG" "AuraLite OS System Information" "bare <name> ran without 'run'"
il_assert_grep "$LOG" "not found in .*/bin.*/apps"    "a failed search names what it searched"
il_assert_grep "$LOG" "BUILTIN_STILL_WINS"            "built-ins still take precedence"

# A name that does not resolve must not be reported as a spawn failure —
# those are different problems with different fixes.
il_assert_no_grep "$LOG" "failed to spawn definitely_not_a_program" \
                                                      "unresolved name is a search failure, not a spawn failure"

il_assert_no_grep "$LOG" "PANIC"                      "no kernel panic"

il_summary
