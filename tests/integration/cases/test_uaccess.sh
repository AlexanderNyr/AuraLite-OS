#!/usr/bin/env bash
# test_uaccess.sh — MATURITY_PLAN M3 gate: hostile user pointers return an
# errno instead of faulting the kernel.
#
# AUDIT_A1 rewrote this file.  The original called il_run, il_assert and
# il_assert_shell_alive and read $IL_SERIAL -- none of which exist in
# tests/integration/lib/lib.sh.  It aborted on line 14 with
# "il_run: command not found" every time it was invoked, so M3's security
# gate had never executed once.  It was also absent from run_all.sh's case
# list until AUDIT_A0, so nothing ever invoked it either.
#
# What this proves: /tests/usertest fires a battery of hostile pointer
# shapes at the syscall layer -- unmapped addresses, ranges that wrap, and
# kernel-space pointers -- and the kernel must answer every one with an
# errno while staying alive.  The decisive assertion is the pair: every
# usertest case passes AND no fault appears anywhere in the log.  A kernel
# that panics on a bad pointer fails the second even if it never reaches
# the first.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Hostile user pointers are rejected, not fatal (M3)"

LOG="$IL_LOGDIR/uaccess.log"
rm -f "$LOG"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The shell prompt appears several seconds into boot; the other cases in
# this suite wait 6-7s before their first command.  Waiting only 3s sent
# "run usertest" into a kernel that was still bringing up drivers, and the
# line was swallowed -- which looked exactly like the program failing.
il_send_delay 8
il_send "run usertest"
il_send_delay 15
il_send "echo UACCESS_SHELL_ALIVE"
il_send_delay 2

il_run_qemu "$LOG" 70

# 1) The program ran at all.  Without this a silent failure to launch would
#    make every later assertion vacuously true.
il_assert_grep "$LOG" "passed ==" \
    "usertest ran and reported a result"

# 2) Every case passed.  usertest prints "== N/M passed ==".
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
line=$(grep -o '== [0-9]*/[0-9]* passed ==' "$LOG" | tail -1)
passed=$(printf '%s' "$line" | sed 's|== \([0-9]*\)/\([0-9]*\) passed ==|\1|')
total=$(printf '%s' "$line" | sed 's|== \([0-9]*\)/\([0-9]*\) passed ==|\2|')
if [ -n "$total" ] && [ "$passed" = "$total" ] && [ "$total" -ge 30 ]; then
    il_pass "usertest $passed/$total passed (>= 30 cases)"
else
    il_fail "usertest reported $line (want all passed, at least 30 cases)"
fi

# 3) The kernel survived.  This is the actual security predicate: a hostile
#    pointer must not be able to take the system down.
il_assert_no_grep "$LOG" "Page Fault|PANIC|panic|triple fault|STACK CORRUPTION" \
    "no kernel fault while handling hostile pointers"

# 4) The shell is still usable afterwards -- the kernel did not merely avoid
#    panicking by wedging the caller.
il_assert_grep_fixed "$LOG" "UACCESS_SHELL_ALIVE" \
    "shell still responsive after the hostile-pointer battery"

il_summary
