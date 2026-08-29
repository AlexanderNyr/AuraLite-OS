#!/usr/bin/env bash
# test_selfhost_script.sh — SELFHOST_PLAN.md SH6a: the shell runs scripts.
#
# SH6's end state is `sh build.sh kernel` building the OS in-guest.  That needs
# a shell that can read a file of commands, pass it arguments, and stop when a
# step fails.  None of that existed: the shell had no script runner, every
# builtin returned void, and cmd_run_argv computed the child's wait status and
# then discarded it -- so a failing compile was indistinguishable from a
# succeeding one.  SH6a adds the exit-status spine and the runner; SH6b-6f add
# redirects, pipes, control flow, shmake and build.sh on top of it.
#
# Three scripts are staged into the initrd at /tests (tools/selfhost/sh6a_*.sh)
# and each proves one thing that could silently be wrong:
#
#   sh6a_probe.sh   $0/$1/$#, $?, $$, an untouched $PATH, and a nested `sh`
#                   whose own arguments survive the nesting
#   sh6a_fail.sh    a failing line stops the script AND names its line number
#   sh6a_exit.sh    `exit 3` stops the SCRIPT -- init is PID 1, so the old
#                   behaviour would have halted the machine
#
# The last check is the one that matters most: after sh6a_exit.sh the host sends
# one more command and requires it to round-trip.  A shell that reports a script
# failure by powering off the system would pass every other assertion here.
#
# Needs no guest toolchain, so unlike the SH1-SH5 cases it does not skip.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host scripting (SH6a): sh runs a file, with args and statuses"

LOG="$IL_LOGDIR/selfhost_script.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh6a_probe.sh kernel"
il_send_delay 3
il_send "sh /tests/sh6a_fail.sh"
il_send_delay 3
il_send "sh /tests/sh6a_exit.sh"
il_send_delay 3
il_send "echo SH6A_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] script PASS: 7 lines ran in-guest" \
    "script ran to completion and printed the receipt"

# ---- positional parameters and status ----
il_assert_grep "$LOG" "sh6a: script=/tests/sh6a_probe.sh target=kernel args=1" \
    "\$0 is the script name, \$1 the argument, \$# the argument count"
il_assert_grep "$LOG" "sh6a: pwd-status=0" \
    "\$? carries the previous line's exit status"

# ---- $$ is literal and unknown names are left alone for SH6b ----
# SH6b changed this: an unquoted $PATH is now an unset variable and expands to
# nothing (POSIX), so the "passes through untouched" property moved behind
# single quotes, which SH6b made suppress expansion.  Updated here rather than
# left asserting behaviour the tree no longer has.
il_assert_grep "$LOG" 'sh6a: dollar=\$ quoted=\$PATH' \
    "\$\$ yields one literal dollar; a single-quoted \$PATH survives verbatim"

# ---- nesting: each frame keeps its own positional parameters ----
il_assert_grep "$LOG" "sh6a: nested script=/tests/sh6a_nested.sh target=kernel depth-ok" \
    "a nested sh call received its argument"
il_assert_grep "$LOG" "sh6a: nested-status=0" \
    "the nested sh returned success to its caller"

# ---- failure path: stop, and say where ----
il_assert_grep "$LOG" "sh6a: fail probe start" \
    "the line before the failure did run"
il_assert_grep "$LOG" "sh: /tests/sh6a_fail.sh:8: command failed with status 127" \
    "the failing line is reported with its line number and status"
il_assert_no_grep "$LOG" "UNREACHABLE-AFTER-FAILURE" \
    "execution stopped at the failing line"

# ---- exit path: the script stops, the machine does not ----
il_assert_grep "$LOG" "sh6a: exit probe" \
    "the exit-probe script started"
il_assert_no_grep "$LOG" "UNREACHABLE-AFTER-EXIT" \
    "exit stopped the script before the next line"
il_assert_grep "$LOG" "^SH6A_STILL_ALIVE$" \
    "init survived the script's exit 3 and still takes commands"

il_summary
