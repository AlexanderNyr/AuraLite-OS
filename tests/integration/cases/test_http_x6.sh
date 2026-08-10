#!/usr/bin/env bash
# test_http_x6.sh — REALINTERNET_PLAN X6: "HTTP completeness".
#
#   "Keep-alive + POST/PUT over the BSD-socket API; http→https redirects
#    followed with certificate validation; gbrowser speaks https:// by
#    default.  One TCP connection serves several requests (same port,
#    Connection: keep-alive).""
#
#   HOST runs x6_server.py: a keep-alive HTTP server that counts sockets
#   and requests on the wire, plus a TLS marker server with a fresh
#   self-signed Ed25519 certificate (ALPN http/1.1).
#   GUEST runs /tests/httpx6 which, on ONE cached connection, does:
#     GET /first, GET /second (reuse), POST /echo (body round-trip),
#     GET /redirect (301 → https://10.0.2.2:<tls_port>/ marker page).
#
# Requires: openssl + python3 (ssl) on the host.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3 openssl

il_section "X6: HTTP completeness (keep-alive, POST, http→https redirect)"

LOG="$IL_LOGDIR/http_x6.log"
IL_LAST_LOG="$LOG"

FIXDIR="$IL_LOGDIR/http_x6-fixture"
mkdir -p "$FIXDIR"
X6_HTTP_PORT="${X6_HTTP_PORT:-18081}"
X6_TLS_PORT="${X6_TLS_PORT:-18443}"
STATS="$FIXDIR/stats.txt"
SRV_PID=""

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    wait 2>/dev/null
}
trap 'cleanup; il_dump_on_error' EXIT

# Fresh self-signed Ed25519 certificate for the TLS marker endpoint.
if ! openssl req -x509 -newkey ed25519 \
        -keyout "$FIXDIR/server.key" -out "$FIXDIR/server.crt" \
        -days 2 -nodes -subj "/CN=10.0.2.2" \
        -addext "subjectAltName=IP:10.0.2.2" >/dev/null 2>&1; then
    il_fail "could not generate the Ed25519 test certificate"
    il_summary
    exit 1
fi

python3 "$PWD/x6_server.py" 10.0.2.2 "$X6_HTTP_PORT" "$X6_TLS_PORT" \
        "$FIXDIR/server.crt" "$FIXDIR/server.key" "$STATS" \
        >"$IL_LOGDIR/x6_server.log" 2>&1 &
SRV_PID=$!
sleep 1
if ! kill -0 "$SRV_PID" 2>/dev/null; then
    il_fail "x6_server.py did not start"
    il_summary
    exit 1
fi

# Boot runs DHCP + the [tcp-x5] 16-connection probe before the shell —
# on hosts where SLIRP's DNS alias answers slowly that probe alone can
# take a minute, so give boot and the gate itself generous room (same
# convention as test_tcp_x5).  Typed input queues safely before the
# shell appears.
il_send_delay 15
il_send "run httpx6 10.0.2.2 $X6_HTTP_PORT $X6_TLS_PORT"
il_send_delay 60                   # TLS handshake is the slow part
il_send "exit"

il_run_qemu "$LOG" 150

# --- guest-side gates ----------------------------------------------------
il_assert_grep "$LOG" "auralite#"                            "shell reached"
il_assert_grep "$LOG" "\[httpx6\] X6 HTTP completeness gate" "httpx6 launched"
il_assert_grep "$LOG" "\[httpx6\] PASS get-first"            "GET /first 200"
il_assert_grep "$LOG" "\[httpx6\] PASS get-second-reuses"    "GET /second reuses the socket"
il_assert_grep "$LOG" "\[httpx6\] PASS post-echo-reuses"     "POST /echo body round-trip on same socket"
il_assert_grep "$LOG" "\[httpx6\] PASS redirect-http-to-https" "http→https redirect followed, marker body"
il_assert_grep "$LOG" "\[httpx6\] ALL PASS"                  "httpx6 ALL PASS"
il_assert_no_grep "$LOG" "\[httpx6\] FAIL"                   "no in-guest X6 failure"
il_assert_no_grep "$LOG" "\[httpx6\] dbg"                    "no guest-side mismatch diagnostics"
il_assert_no_grep "$LOG" "\[ahttp\] TLS handshake failed"    "no TLS failure on the https hop"

# --- keep-alive evidence on the serial log -------------------------------
il_assert_grep "$LOG" "\[ahttp\] keep-alive: reusing connection" \
    "client logged socket reuse"

# Exactly one redirect hop, voiced by the client.
redir_count="$(grep -c "\[ahttp\] redirect:" "$LOG" || true)"
if [ "$redir_count" -eq 1 ]; then
    il_pass "exactly one redirect hop logged"
else
    il_fail "expected exactly one redirect hop, saw $redir_count"
fi
il_assert_grep "$LOG" "\[ahttp\] redirect: http://10\.0\.2\.2:$X6_HTTP_PORT/redirect -> https://10\.0\.2\.2:$X6_TLS_PORT/" \
    "redirect hop was http→https"

# --- host-side wire gate -------------------------------------------------
# The server's own socket counters: all four request steps must have
# travelled over ONE HTTP connection; the TLS side saw exactly one
# connection serving the marker page.
deadline=$((SECONDS + 10))
stats_line=""
while [ $SECONDS -lt $deadline ]; do
    [ -f "$STATS" ] && stats_line="$(cat "$STATS")"
    case "$stats_line" in
        *"requests=4"*) break ;;
    esac
    sleep 0.5
done
case "$stats_line" in
    connections=1\ requests=4\ https_connections=1\ https_requests=1*)
        il_pass "server saw 1 conn / 4 requests + 1 TLS conn (keep-alive proven on the wire)"
        ;;
    *)
        il_fail "unexpected wire counters: '${stats_line:-<none>}' (want: connections=1 requests=4 https_connections=1 https_requests=1)"
        ;;
esac

il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"   "no exception during X6 run"
il_assert_no_grep "$LOG" "PANIC"                 "no kernel panic"

il_summary
