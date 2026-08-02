#!/usr/bin/env bash
# test_install_dirs.sh — enforced installation directories (FSLAYOUT_PLAN F1).
#
# tests/unit/test_execpolicy.c covers the predicate exhaustively on the host.
# This case covers what a pure predicate cannot: that the rule is wired into
# open() and chmod() on a running kernel, that /opt exists and is writable,
# and that apm installs there and the installed program runs.
#
# It deliberately also asserts the things that must STILL work. A restriction
# that breaks ordinary file creation is not a working restriction.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "enforced installation directories"

LOG="$IL_LOGDIR/install_dirs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
# The in-OS probe: allowlist, refusals, traversal, chmod, and the negatives.
il_send "run insttest"
il_send_delay 4
# The package manager end to end.
il_send "apm install matrix"
il_send_delay 3
il_send "ls /opt"
il_send_delay 1
il_send "apm list"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 45

# --- /opt exists as a real mount ---
il_assert_grep "$LOG" "\[vfs\] mounted '/opt'"          "/opt is mounted"

# --- the probe ---
il_assert_grep "$LOG" "INSTTEST: begin"                  "probe ran"
il_assert_grep "$LOG" "INSTTEST: ALL PASS"               "every probe check passed"
il_assert_no_grep "$LOG" "INSTTEST FAIL"                 "no probe failure"

# --- the package manager installs into /opt, not /tmp ---
il_assert_grep "$LOG" "Unpacked .* to /opt/matrix"       "apm installs into /opt"
il_assert_grep "$LOG" "matrix"                           "ls /opt shows the installed program"
il_assert_no_grep "$LOG" "to /tmp/matrix"                "apm no longer installs into /tmp"

# --- a refused install says why, rather than failing mysteriously ---
il_assert_grep "$LOG" "refused to create executable"     "a refusal is logged with a reason"

il_assert_no_grep "$LOG" "PANIC"                         "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"           "no unhandled exception"

il_summary
