#!/usr/bin/env bash
# test_tls_errno.sh — FIX_R3 gate: errno is a per-thread cell.
#
# Boots AuraLite and runs /tests/errnotest, which has two threads provoke
# DIFFERENT errnos (ENOENT vs EBADF) in a loop and asserts each still reads
# back only its own — impossible with the pre-R3 global cell shared by all
# threads.  The case additionally asserts the single-threaded contract
# (main thread's errno still works end to end) and that the per-thread
# cells are distinct addresses.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "thread-local errno (FIX_R3)"

LOG="$IL_LOGDIR/tls_errno.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run errnotest"
il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 40

# Single-threaded contract (main thread, incl. the new TLS install path).
il_assert_grep_fixed "$LOG" "ERRNOTEST PASS single-thread open(missing)=ENOENT" \
    "main thread: open(missing) reports ENOENT"
il_assert_grep_fixed "$LOG" "ERRNOTEST PASS single-thread read(badfd)=EBADF" \
    "main thread: read(badfd) reports EBADF"

# Core TLS property: one cell per thread, distinct addresses.
il_assert_grep_fixed "$LOG" "distinct=1" \
    "__errno_location() yields a distinct cell per thread"

# D2 gate: neither thread ever observes the other's errno.
il_assert_grep_fixed "$LOG" "ERRNOTEST A: foreign-observed=0 other-mismatch=0 of 500" \
    "thread A never sees thread B's errno"
il_assert_grep_fixed "$LOG" "ERRNOTEST B: foreign-observed=0 other-mismatch=0 of 500" \
    "thread B never sees thread A's errno"
il_assert_grep_fixed "$LOG" "ERRNOTEST PASS per-thread isolation (500 iterations x2)" \
    "per-thread isolation verdict"

# No regressions / faults (pre-fix this panicked the kernel with #UD in
# context_switch and SIGSEGV'd both clone children).
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "Invalid Opcode" "no #UD (wrfsbase regression)"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary
