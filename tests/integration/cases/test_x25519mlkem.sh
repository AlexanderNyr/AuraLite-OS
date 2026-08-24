#!/usr/bin/env bash
# test_x25519mlkem.sh — REALINTERNET2 Y6 guest gate.
#
# HOST runs openssl s_server -groups X25519MLKEM768 (Ed25519 leaf,
# ALPN http/1.1) on :4434.  GUEST runs /tests/tlstest against
# 10.0.2.2:4434.  Receipt: [tls] PASS: X25519MLKEM768

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 openssl

il_section "Y6: X25519MLKEM768 vs local openssl s_server"

LOG="$IL_LOGDIR/x25519mlkem.log"
IL_LAST_LOG="$LOG"

FIXDIR="$IL_LOGDIR/x25519mlkem-fixture"
mkdir -p "$FIXDIR"
S_SERVER_PID=""
PORT="${HYBRID_PORT:-4434}"

cleanup() {
    [ -n "$S_SERVER_PID" ] && kill "$S_SERVER_PID" 2>/dev/null || true
    wait "$S_SERVER_PID" 2>/dev/null || true
    il_dump_on_error
}
trap cleanup EXIT

if ! openssl req -x509 -newkey ed25519 \
        -keyout "$FIXDIR/server.key" -out "$FIXDIR/server.crt" \
        -days 2 -nodes -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1; then
    il_fail "could not generate the Ed25519 test certificate"
    il_summary
    exit 1
fi

openssl s_server -accept "$PORT" \
    -cert "$FIXDIR/server.crt" -key "$FIXDIR/server.key" \
    -groups X25519MLKEM768 \
    -www -alpn http/1.1 -quiet >/dev/null 2>&1 &
S_SERVER_PID=$!
sleep 1
if ! kill -0 "$S_SERVER_PID" 2>/dev/null; then
    il_fail "openssl s_server -groups X25519MLKEM768 did not start"
    il_summary
    exit 1
fi

il_send_delay 12
il_send "run tlstest 10.0.2.2 ${PORT} localhost"
il_send_delay 40
il_send "exit"

il_run_qemu "$LOG" 110

il_assert_grep "$LOG" "auralite#" \
    "shell reached"
il_assert_grep_fixed "$LOG" "[tlstest] connecting to 10.0.2.2:${PORT}" \
    "Y6 probe ran"
il_assert_grep_fixed "$LOG" "[tls] PASS: X25519MLKEM768" \
    "hybrid group negotiated"
il_assert_grep "$LOG" "\\[tlstest\\] PASS: TLS 1.3 handshake verified" \
    "handshake verified"
il_assert_grep "$LOG" "\\[tlstest\\] ALL PASS" \
    "tlstest ALL PASS"
il_assert_no_grep "$LOG" "[tlstest] FAIL" "no tlstest failure"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"

il_summary
