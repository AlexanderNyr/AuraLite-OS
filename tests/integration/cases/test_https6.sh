#!/usr/bin/env bash
# test_https6.sh — REALINTERNET2 Y4 guest gate (RES-26).
#
# HOST runs openssl s_server on [::]:8446 (SLIRP maps guest fec0::2
# to the host, same as Y3's :8036 — QEMU 10 guestfwd is IPv4-only).
# GUEST runs /tests/https6 against https://[fec0::2]:8446/ through
# libahttp.  Receipt: [https6] PASS

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 openssl

il_section "HTTPS-over-IPv6 (fec0::2:8446 via libahttp)"

LOG="$IL_LOGDIR/https6.log"
IL_LAST_LOG="$LOG"
FIXDIR="$IL_LOGDIR/https6-fixture"
mkdir -p "$FIXDIR"
HTTPS6_PORT="${HTTPS6_PORT:-8446}"
S_SERVER_PID=""

cleanup() {
    [ -n "$S_SERVER_PID" ] && kill "$S_SERVER_PID" 2>/dev/null || true
    wait "$S_SERVER_PID" 2>/dev/null || true
    il_dump_on_error
}
trap cleanup EXIT

openssl ecparam -name prime256v1 -genkey -noout -out "$FIXDIR/leaf.key" 2>/dev/null
openssl req -new -x509 -key "$FIXDIR/leaf.key" -out "$FIXDIR/leaf.pem" \
    -days 2 -nodes -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:fec0::2" >/dev/null 2>&1 || {
    il_fail "could not generate fixture cert"; il_summary; exit 1
}

# Listen on IPv6; SLIRP's vhost connect lands on ::1 (Y3 measurement).
openssl s_server -accept "[::]:${HTTPS6_PORT}" \
    -cert "$FIXDIR/leaf.pem" -key "$FIXDIR/leaf.key" \
    -www -alpn http/1.1 -quiet >/dev/null 2>&1 &
S_SERVER_PID=$!
sleep 1
if ! kill -0 "$S_SERVER_PID" 2>/dev/null; then
    # Older openssl: -6 -accept PORT
    openssl s_server -6 -accept "${HTTPS6_PORT}" \
        -cert "$FIXDIR/leaf.pem" -key "$FIXDIR/leaf.key" \
        -www -alpn http/1.1 -quiet >/dev/null 2>&1 &
    S_SERVER_PID=$!
    sleep 1
fi
if ! kill -0 "$S_SERVER_PID" 2>/dev/null; then
    il_fail "openssl s_server did not start on :${HTTPS6_PORT}"
    il_summary
    exit 1
fi

il_send_delay 15
il_send "run https6 https://[fec0::2]:${HTTPS6_PORT}/"
il_send_delay 35
il_send "exit"

il_run_qemu "$LOG" 90

il_assert_grep_fixed "$LOG" "[https6] fetching https://[fec0::2]:${HTTPS6_PORT}/" \
    "Y4 probe ran"
il_assert_grep_fixed "$LOG" "[ahttp] dial v6" \
    "libahttp dialled IPv6"
il_assert_grep "$LOG" "\\[https6\\] PASS: status [1-9][0-9]* body [1-9][0-9]* via v6" \
    "HTTPS-over-IPv6 fetch receipt"
il_assert_no_grep "$LOG" "[https6] FAIL" "no https6 failure"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"

il_summary
