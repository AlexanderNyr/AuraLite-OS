#!/usr/bin/env bash
# test_tcp_server.sh — verify TCP server socket API (bind / listen / accept).
# H5 gate criterion: verify tcpserver binds to port 8080, listens, accepts
# simulated connection, receives request, and sends HTTP 200 OK response.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "TCP Server socket API (H5)"

LOG="$IL_LOGDIR/tcp_server.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /tcpserver"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep_fixed "$LOG" "SERVER: bind(port 8080) OK"                   "bind() successful"
il_assert_grep_fixed "$LOG" "SERVER: listen() OK, waiting for connection"  "listen() successful"
il_assert_grep "$LOG" "SERVER: accepted connection from IP"          "accept() successful"
il_assert_grep "$LOG" "SERVER: received .* bytes: GET / HTTP/1.0"    "recv() read incoming request"
il_assert_grep "$LOG" "SERVER: sent .* bytes of HTTP response"       "send() sent HTTP response"
il_assert_grep "$LOG" "SERVER: finished successfully"                "clean teardown and closesocket()"

il_assert_no_grep "$LOG" "PANIC"                                     "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                       "no unhandled exception"

il_summary