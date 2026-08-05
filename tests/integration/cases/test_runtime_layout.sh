#!/usr/bin/env bash
# test_runtime_layout.sh — the moved runtime layout (FSLAYOUT_PLAN phase F3).
#
# F3 moved every program into /bin, /apps, /demos, /tests or /pkg. F5 then
# removed the root-level aliases, so this case asserts the layout AND that
# each program has exactly one location.
#
# The alias assertions that used to live here were grouped precisely so that
# F5 would know what to revisit. They are now their negatives: the old root
# path must NOT resolve.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "runtime layout"

LOG="$IL_LOGDIR/runtime_layout.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "ls /"
il_send_delay 1
il_send "ls /bin"
il_send_delay 1
il_send "ls /apps"
il_send_delay 1
il_send "ls /demos"
il_send_delay 1
il_send "ls /pkg"
il_send_delay 1
# The new location, by full path.  It has to be a program that EXITS: calc
# and editor are interactive and would swallow every command after them,
# failing the next three assertions for a reason that has nothing to do with
# the layout.  /bin/sysinfo prints and returns.
il_send "run /bin/sysinfo"
il_send_delay 2
# The old root path must no longer resolve (F5 removed the aliases).
il_send "run /hello"
il_send_delay 2
# By name, which is what the search path is for.
il_send "run clock"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 40

# --- the directories exist and hold what they should ---
il_assert_grep "$LOG" "bin/"                          "ls / shows bin"
il_assert_grep "$LOG" "apps/"                         "ls / shows apps"
il_assert_grep "$LOG" "demos/"                        "ls / shows demos"
il_assert_grep "$LOG" "tests/"                        "ls / shows tests"
il_assert_grep "$LOG" "pkg/"                          "ls / shows pkg"
il_assert_grep "$LOG" "matrix.apkg"                   "ls /pkg shows a package archive"

# --- a program runs from its new home ---
il_assert_grep "$LOG" "running /bin/sysinfo"          "a program runs at its new path"

# --- one location per program: the F3 aliases are gone (F5) ---
#
# spawn() reports the miss.  Asserting on the absence of "reaped '/hello'"
# would NOT work: the shell still creates a thread for the attempt and the
# kernel still reaps it, so that line appears whether or not the program
# exists.  The positive assertion is the honest one.
il_assert_grep "$LOG" "spawn: '/hello' not found"     "the old root alias is gone"
# The root now holds directories, not 43 programs.
il_assert_no_grep "$LOG" "^  gltest  \\("             "ls / does not list programs at the root"

# --- by name, unchanged from F2 ---
il_assert_grep "$LOG" "running /apps/clock"           "by name, resolved to its new directory"

il_assert_no_grep "$LOG" "PANIC"                      "no kernel panic"
# `run /hello` above is EXPECTED to fail, so "not found" is not asserted
# against here — spawn reports it, and that is the point of the check above.

il_summary
