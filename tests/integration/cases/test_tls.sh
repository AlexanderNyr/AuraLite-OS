#!/usr/bin/env bash
# test_tls.sh — INTERNET_PLAN.md phase N3 primary gate.
#
# "A handshake against a local openssl s_server in the integration
# harness, so the test does not depend on the internet."
#
# HOST runs openssl s_server with a fresh self-signed Ed25519 certificate
# (SAN localhost, ALPN http/1.1).  GUEST runs /tests/tlstest against
# 10.0.2.2:4433 — QEMU user networking's alias for the host.
#
# Requires: openssl on the host.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 openssl

il_section "N3: TLS 1.3 handshake vs local openssl s_server"

LOG="$IL_LOGDIR/tls.log"
IL_LAST_LOG="$LOG"

FIXDIR="$IL_LOGDIR/tls-fixture"
mkdir -p "$FIXDIR"
S_SERVER_PID=""

cleanup() {
    [ -n "$S_SERVER_PID" ] && kill "$S_SERVER_PID" 2>/dev/null
    wait 2>/dev/null
}
trap 'cleanup; il_dump_on_error' EXIT

# Ed25519 certificate.
if ! openssl req -x509 -newkey ed25519 \
        -keyout "$FIXDIR/server.key" -out "$FIXDIR/server.crt" \
        -days 2 -nodes -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1; then
    il_fail "could not generate the Ed25519 test certificate"
    il_summary
    exit 1
fi

openssl s_server -accept 4433 \
    -cert "$FIXDIR/server.crt" -key "$FIXDIR/server.key" \
    -www -alpn http/1.1 -quiet >/dev/null 2>&1 &
S_SERVER_PID=$!
sleep 1
if ! kill -0 "$S_SERVER_PID" 2>/dev/null; then
    il_fail "openssl s_server did not start"
    il_summary
    exit 1
fi

il_send_delay 8
il_send "run tlstest 10.0.2.2 4433 localhost"
il_send_delay 25
il_send "exit"

il_run_qemu "$LOG" 90

il_assert_grep "$LOG" "auralite#"                          "shell reached"
il_assert_grep "$LOG" "\[tlstest\] PASS: socket"           "guest socket created"
il_assert_grep "$LOG" "\[tlstest\] PASS: connect"          "guest connected to host s_server"
il_assert_grep "$LOG" "\[tlstest\] PASS: TLS 1.3 handshake verified" "handshake verified"
il_assert_grep "$LOG" "\[tlstest\] PASS: ALPN"             "ALPN negotiated"
il_assert_grep "$LOG" "\[tlstest\] PASS: peer leaf certificate captured" "leaf cert surfaced"
il_assert_grep "$LOG" "\[tlstest\] PASS: write GET over TLS" "application write"
il_assert_grep "$LOG" "\[tlstest\] PASS: application data received" "application data round trip"
il_assert_grep "$LOG" "\[tlstest\] PASS: close_notify sent" "close_notify on shutdown"
il_assert_grep "$LOG" "\[tlstest\] ALL PASS"               "tlstest reports ALL PASS"

il_assert_no_grep "$LOG" "\[tlstest\] FAIL"    "no in-guest TLS failure"
il_assert_no_grep "$LOG" "KERNEL EXCEPTION"    "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"

il_summary
