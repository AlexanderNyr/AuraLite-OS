#!/usr/bin/env bash
# test_init_array.sh — FIX_R5 gate: .init_array/.fini_array actually run.
#
# Boots AuraLite and runs /tests/ctortest, which links three constructors
# and three destructors and judges the order end to end from a log buffer:
# constructors must run before main() in link order, destructors after
# main() returns in reverse.  Also asserts the plan's first gate: gusb's
# EXISTING constructor prints "[gusb] ctor" — it silently never did before
# R5 because nothing walked the arrays at runtime.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section ".init_array / .fini_array runtime walk (FIX_R5)"

LOG="$IL_LOGDIR/init_array.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run ctortest"
il_send_delay 8
il_send "run gusb"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 40

# The in-program end-to-end order verdicts.
il_assert_grep_fixed "$LOG" "CTORTEST PASS ctors before main, link order (log=123)" \
    "constructors run before main, in link order"
il_assert_grep_fixed "$LOG" "CTORTEST PASS dtors after main, reverse order (log=123m321)" \
    "destructors run after main, in reverse order"

# The plan's gate no. 1: gusb's pre-existing constructor finally prints.
il_assert_grep_fixed "$LOG" "[gusb] ctor" \
    "gusb's constructor runs when the program starts"

il_assert_no_grep_fixed "$LOG" "CTORTEST FAIL" "no constructor/destructor order failure"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary
