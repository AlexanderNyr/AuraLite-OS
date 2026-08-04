#!/usr/bin/env bash
# test_socket_errno.sh — FIX_R7 gate: socket syscalls say WHY they failed.
#
# One boot, one program: /tests/socktest asserts the SPECIFIC errno for a
# closed-port connect (ECONNREFUSED via SLIRP RST), an unroutable address
# (EHOSTUNREACH via unanswered ARP), operations on a closed socket (EBADF),
# socket-table exhaustion (EMFILE), and an unsupported family (EAFNOSUPPORT);
# it also prints a real perror() line, which this script then greps for the
# human-readable cause.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "socket errno propagation (FIX_R7)"

LOG="$IL_LOGDIR/socket_errno.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run socktest"
il_send_delay 35
il_send "echo gate-end"

il_run_qemu "$LOG" 110

il_assert_grep_fixed "$LOG" "SOCKTEST PASS connect() to a closed port yields ECONNREFUSED" \
    "a closed port yields ECONNREFUSED, not -1"
il_assert_grep_fixed "$LOG" "SOCKTEST PASS connect() to an unreachable host yields EHOSTUNREACH" \
    "an unreachable host yields EHOSTUNREACH, not a hang or a refusal"
il_assert_grep_fixed "$LOG" "SOCKTEST PASS send() on a closed socket yields EBADF" \
    "send() on a closed socket yields EBADF"
il_assert_grep_fixed "$LOG" "SOCKTEST PASS recv() on a closed socket yields EBADF" \
    "recv() on a closed socket yields EBADF"
il_assert_grep_fixed "$LOG" "SOCKTEST PASS the 33rd socket() of a process fails with EMFILE" \
    "socket table exhaustion yields EMFILE"
il_assert_grep_fixed "$LOG" "SOCKTEST PASS socket(AF_INET6) yields EAFNOSUPPORT" \
    "an unsupported address family yields EAFNOSUPPORT"
il_assert_grep_fixed "$LOG" "SOCKTEST PASS strerror names the network causes" \
    "strerror() names the cause"
il_assert_grep_fixed "$LOG" "connect: Connection refused" \
    "perror() output for a failed connection names the cause"
il_assert_grep_fixed "$LOG" "SOCKTEST ALL PASS" "in-program gate green"
il_assert_grep_fixed "$LOG" "gate-end" "shell survives the gate"

il_assert_no_grep_fixed "$LOG" "SOCKTEST FAIL" "no gate failures"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary
