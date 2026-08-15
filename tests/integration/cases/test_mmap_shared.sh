#!/usr/bin/env bash
# test_mmap_shared.sh — MATURITY_PLAN M4 gate: MAP_SHARED|MAP_ANONYMOUS is
# genuinely shared across fork, and MAP_PRIVATE genuinely is not.
#
# AUDIT_A1 rewrote this file.  The original called il_run, il_assert and
# il_assert_shell_alive and read $IL_SERIAL -- none of which exist in
# lib.sh -- so it aborted on line 14 and had never executed.  It also only
# ever asked the shell to run the generic `selftest`, which does not touch
# MAP_SHARED at all: even had it run, it would have asserted nothing about
# M4.  /tests/mmapshare was added alongside this rewrite to give it
# something real to measure.
#
# The MAP_PRIVATE half is the control.  A mapping implementation that
# shared everything would satisfy "the child sees the parent's write" while
# being badly wrong, so the same sequence over MAP_PRIVATE must show
# copy-on-write isolation instead.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "MAP_SHARED across fork (M4)"

LOG="$IL_LOGDIR/mmap_shared.log"
rm -f "$LOG"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run mmapshare"
il_send_delay 10
il_send "echo MMAP_SHELL_ALIVE"
il_send_delay 2

il_run_qemu "$LOG" 70

il_assert_grep "$LOG" "mmapshare: M4 MAP_SHARED across fork" \
    "the M4 test program ran"

il_assert_grep_fixed "$LOG" "mmap(MAP_SHARED|MAP_ANONYMOUS) PASS" \
    "MAP_SHARED|MAP_ANONYMOUS mapping succeeds"

# The decisive pair: shared really shares, private really does not.
il_assert_grep "$LOG" "child observed the parent's write through MAP_SHARED PASS" \
    "the child sees the parent's write through the shared page"
il_assert_grep "$LOG" "MAP_PRIVATE stayed private across fork \(control\) PASS" \
    "MAP_PRIVATE is copy-on-write, not shared (control)"

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
line=$(grep -o '== [0-9]*/[0-9]* passed ==' "$LOG" | tail -1)
passed=$(printf '%s' "$line" | sed 's|== \([0-9]*\)/\([0-9]*\) passed ==|\1|')
total=$(printf '%s' "$line" | sed 's|== \([0-9]*\)/\([0-9]*\) passed ==|\2|')
if [ -n "$total" ] && [ "$passed" = "$total" ]; then
    il_pass "mmapshare $passed/$total passed"
else
    il_fail "mmapshare reported $line"
fi

il_assert_no_grep "$LOG" "Page Fault|PANIC|panic|triple fault" \
    "no kernel fault"
il_assert_grep_fixed "$LOG" "MMAP_SHELL_ALIVE" \
    "shell still responsive afterwards"

il_summary
