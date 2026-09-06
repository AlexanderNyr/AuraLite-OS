#!/usr/bin/env bash
# test_tcp_ordering.sh — RESIDUE2 T5 gate: production TCP ordering +
# throughput on the real wire.
#
# tcpx5test proved a 1 MiB upload survives window-full waits, but its
# server only COUNTED bytes.  This case verifies CONTENT both ways:
# the guest's tcpordtest uploads 512 KiB of position-dependent pattern,
# the host server (tcp_echo_verify.py) verifies every byte and echoes it
# back, and the guest verifies the echoed copy byte-for-byte.  Any
# reorder, duplicate or corruption the sliding-window/retransmit paths
# could introduce fails at a named offset.  The host also prints the
# wall-clock throughput receipt — the plan's "new TCP throughput/
# ordering cases" box.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "TCP ordering + throughput (RESIDUE2 T5)"

LOG="$IL_LOGDIR/tcp_ordering.log"
SRVLOG="$IL_LOGDIR/tcp_echo_verify.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

ORD_PORT="${ORD_PORT:-18098}"

python3 tcp_echo_verify.py "$ORD_PORT" >"$SRVLOG" 2>&1 &
SRV_PID=$!
cleanup() {
    kill "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
    il_dump_on_error
}
trap cleanup EXIT

il_send_delay 12
il_send "run tcpordtest $ORD_PORT"
il_send_delay 45
il_send "echo gate-end"
il_send "exit"

il_run_qemu "$LOG" 100

# ---- host side: the pattern arrived intact and was echoed -------------------
il_assert_grep_fixed "$SRVLOG" "VERIFY-OK 524288 bytes (pattern intact)" \
    "the server verified every uploaded byte against the pattern"
il_assert_grep_fixed "$SRVLOG" "ECHO-SENT 524288 bytes" \
    "the whole stream was echoed back"
il_assert_grep "$SRVLOG" "THROUGHPUT 524288 bytes in [0-9]+\.[0-9]s \([0-9]+ B/s\)" \
    "the throughput receipt was printed"

# ---- guest side: the echo arrived intact -------------------------------------
il_assert_grep_fixed "$LOG" "TCPORD: connected, sending patterned stream" \
    "the guest reached the transfer"
il_assert_grep_fixed "$LOG" "TCPORD PASS: 524288 bytes echoed verbatim — stream ordering intact end to end" \
    "the guest verified the echoed byte stream end to end"
il_assert_no_grep "$LOG" "TCPORD FAIL" "no corruption/truncation marker"
il_assert_no_grep "$LOG" "VERIFY-FAIL" "no server-side verification failure"

# ---- kernel health -----------------------------------------------------------
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived the transfer"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
