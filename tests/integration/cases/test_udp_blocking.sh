#!/usr/bin/env bash
# test_udp_blocking.sh — RESIDUE2 T5 gate: recvfrom(2) is fully blocking.
#
# The old socket_recvfrom polled a 10-tick (~100 ms) slice in a pause
# loop and returned raw -1 when it expired — neither blocking (POSIX
# recvfrom sleeps) nor errno-clean (-1 IS EPERM).  Now the caller parks
# on the NIC's RX wait queue until a datagram for its port arrives.
#
# Proof, on the L2 lab (no SLIRP; see l2lab.py mode udpblock): the guest
# app (`run udptest block`) sends a knock to 10.9.9.99:53001 and blocks
# in recvfrom; the lab answers the knock only after a deliberate ~4 s
# delay.  With the old poll slice the call expired at ~100 ms; only a
# true block can still see the reply:
#   guest: "UDPTEST-BLOCK: knock sent" ... "UDPTEST-BLOCK PASS: blocked
#   past the host delay" — and the lab receipt shows the reply really
#   went out after >3 s.  A returned -1 prints "...FAIL: recvfrom
#   returned -1" instead, which the case forbids.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "fully-blocking recvfrom (RESIDUE2 T5)"

LOG="$IL_LOGDIR/udp_blocking.log"
RECEIPT="$IL_LOGDIR/l2lab-udpblock.receipt"
LABLOG="$IL_LOGDIR/l2lab-udpblock.out"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
rm -f "$RECEIPT" "$LABLOG"

# The lab owns the wire; it must be listening before the guest boots.
python3 l2lab.py udpblock "$RECEIPT" >"$LABLOG" 2>&1 &
LAB_PID=$!

il_send_delay 10
il_send "run udptest block"
il_send_delay 12
il_send "echo gate-end"
il_send "exit"

# No SLIRP: replace the whole netdev with the mcast socket the lab is on.
export IL_NETDEV="-netdev socket,id=net0,mcast=230.10.10.10:4711"
export IL_SELFTEST=off

il_run_qemu "$LOG" 60
kill "$LAB_PID" 2>/dev/null || true
wait "$LAB_PID" 2>/dev/null || true

# ---- the lab's side of the story -------------------------------------------
il_assert_grep_fixed "$RECEIPT" "KNOCK_SEEN" \
    "the lab saw the guest's knock (and answered its ARP)"
il_assert_grep "$RECEIPT" "UDP_REPLY_SENT after [3-9]\.[0-9]s" \
    "the reply really went out after the deliberate >3 s delay"
il_assert_grep_fixed "$RECEIPT" "LAB_DONE ok" "the lab ran to completion"

# ---- the guest's side --------------------------------------------------------
il_assert_grep_fixed "$LOG" "UDPTEST-BLOCK: knock sent" \
    "the guest app reached the blocking call"
il_assert_grep_fixed "$LOG" "UDPTEST-BLOCK PASS: blocked past the host delay" \
    "recvfrom SLEPT past the host delay and then delivered (true block)"
il_assert_no_grep "$LOG" "UDPTEST-BLOCK FAIL" \
    "no failure marker (the old poll-slice expiry path)"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
