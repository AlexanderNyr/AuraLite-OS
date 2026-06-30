#!/usr/bin/env bash
# test_udp_sockets.sh — N2.3b UDP user socket sendto/recvfrom gate.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N2.3b UDP user sockets"

LOG="$IL_LOGDIR/udp_sockets.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run /udptest"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 40

il_assert_grep_fixed "$LOG" "UDPTEST: starting SOCK_DGRAM DNS probe" "UDP socket app started"
il_assert_grep_fixed "$LOG" "UDPTEST: DNS query sent via sendto" "sendto() sent UDP DNS query"
il_assert_grep "$LOG" "UDPTEST PASS: example\.com A .* from 10\.0\.2\.3:53" "recvfrom() received DNS response"
il_assert_no_grep "$LOG" "UDPTEST FAIL" "no UDP test failure marker"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no unhandled exception"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"

il_summary
