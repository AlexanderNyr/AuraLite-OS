#!/usr/bin/env bash
# test_tcp_x5.sh — REALINTERNET_PLAN phase X5 gate (TCP hardening).
#
# Two gates in one boot:
#   1. Boot self-test [tcp-x5]: with SLIRP up, the kernel opens
#      TCP_MAX_CONNS (16) concurrent connections and the next open()
#      must fail cleanly with -EMFILE and a printed diagnosis —
#      "N concurrent connections are predictable".
#   2. Wire test: /tests/tcpx5test uploads 1 MiB (larger than SLIRP's
#      proxied receive buffer) to a host-side server that drains the
#      socket piecemeal, so SLIRP's proxy window toward the guest opens
#      in small increments and the kernel TCP sender provably spends time
#      in window-full waits — "a slow server that ACKs the window
#      piecemeal completes a transfer".
#
# Network-dependent: keep OUT of CI gating (plan rule D6); run manually
# and record the dated pass in docs/status.md.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "TCP X5 hardening: concurrency + piecemeal-ACK transfer"

LOG="$IL_LOGDIR/tcp_x5.log"
IL_LAST_LOG="$LOG"

X5_PORT="${X5_PORT:-18099}"

# Slow-drain server on the host (SLIRP exposes it as 10.0.2.2:$X5_PORT).
python3 x5_slow_server.py "$X5_PORT" \
        >"$IL_LOGDIR/x5_slow_server.log" 2>&1 &
SRV_PID=$!

cleanup() {
    kill "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
    il_dump_on_error
}
trap cleanup EXIT

# Boot self-test runs DHCP + [tcp-x5] before the shell, give it room;
# then run the upload test (1 MiB against a ~100 KiB/s sink: the upload
# alone takes >10 s plus the piecemeal window-full waits, then the
# verdict lands after the drain finishes).
il_send_delay 15
il_send "run tcpx5test $X5_PORT"
il_send_delay 75
il_send "cat /proc/perf"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 145

# Gate 1: concurrent connections + clean full-table diagnosis.
il_assert_grep_fixed "$LOG" "[tcp-x5]   full table: extra open refused with -EMFILE (diagnosed)" \
    "16 slots held; the 17th open fails cleanly with -EMFILE"
il_assert_grep_fixed "$LOG" "[tcp-x5] PASS: 16 concurrent connections held; table full is diagnosed, not fatal" \
    "boot X5 self-test passes"

# Gate 2: piecemeal-ACK slow server completes the whole transfer.
il_assert_grep_fixed "$LOG" "TCPX5: connected, uploading..." \
    "guest connected through SLIRP"
il_assert_grep "$LOG" "\\[tcp\\] window full: waiting for ACK" \
    "sender provably hit window-full waits (piecemeal ACKs)"
il_assert_grep_fixed "$LOG" "TCPX5: upload complete (1048576 bytes), awaiting server verdict" \
    "1 MiB uploaded through window-full waits"
il_assert_grep "$LOG" "TCPX5 PASS: slow piecemeal-ACK server drained all 1048576 bytes" \
    "slow server drained every byte; verdict echoed"

il_assert_no_grep "$LOG" "PANIC"                 "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"   "no unhandled exception"
il_assert_no_grep "$LOG" "TCPX5 FAIL"            "no tcpx5test failure line"

# Gate 3 (RINET2 Y1): congestion control is ALIVE, not decorative.
# The 1 MiB upload starts inside slow start (IW = 14600 << 1 MiB), so
# the send budget MUST hit the cwnd edge — the counter Y0 reserved at
# zero has to move on this lane, and the RTO/retransmit counters must
# at least print.
il_assert_grep "$LOG" "^tcp_cwnd_limited_sends [1-9][0-9]*" \
    "Y1 receipt: the cwnd edge bounded real sends (slow start ran)"
il_assert_grep "$LOG" "^tcp_retransmits [0-9]+" \
    "transport counters print (retransmits)"
il_assert_grep "$LOG" "^tcp_rto_events [0-9]+" \
    "transport counters print (RTO events)"

il_summary
