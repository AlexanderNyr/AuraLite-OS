#!/usr/bin/env bash
# test_e1000_idle_drain.sh — RESIDUE2 T5 gate: the e1000 RX ring drains
# while idle.
#
# One boot, no SLIRP: the NIC rides a QEMU mcast-socket netdev shared
# with tests/integration/l2lab.py, a deterministic L2 lab that
#   (1) plays DHCP server (OFFER/ACK for 10.9.9.15) so the guest owns an
#       address with no SLIRP in the middle,
#   (2) ARP-probes the guest while it is IDLE — before T5 the kernel
#       never answered inbound ARP unless a syscall happened to own the
#       NIC; now the idle drain's passive input must reply,
#   (3) blasts 300 unsolicited frames (unclaimed ethertype 0x8899),
#       paced at 25 ms — the drain must consume them silently,
#   (4) ARP-probes AGAIN through the tail of the load.
#
# The gate (the TODO box, verbatim): "consume and discard frames that no
# socket claims ... rather than silence the message" — asserted as:
#   - the guest log carries the idle-drain receipt line,
#   - ZERO "[e1000] RX overrun" lines (the queue never clogs, so the old
#     per-interrupt overrun print has nothing to say),
#   - both ARP probes were answered from inside the guest (receipt file),
#   - the shell survives and no panic.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "e1000 idle RX drain (RESIDUE2 T5)"

LOG="$IL_LOGDIR/e1000_idle_drain.log"
RECEIPT="$IL_LOGDIR/l2lab.receipt"
LABLOG="$IL_LOGDIR/l2lab.out"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
rm -f "$RECEIPT" "$LABLOG"

# The L2 lab owns the wire; it must be listening before the guest boots.
# (argv is <mode> <receipt-file> since the RESIDUE2 T5 udpblock refactor.)
python3 l2lab.py idledrain "$RECEIPT" >"$LABLOG" 2>&1 &
LAB_PID=$!

# Boot needs nothing from the serial side: DHCP, both probes and the
# blast are all driven from the lab.  Park at the shell, then leave.
il_send_delay 10
il_send "echo gate-end"
il_send "exit"

# No SLIRP: replace the whole netdev with the mcast socket the lab is
# bound to (lib.sh pairs it with -device $IL_NIC,netdev=net0).
export IL_NETDEV="-netdev socket,id=net0,mcast=230.10.10.10:4711"
export IL_SELFTEST=off

il_run_qemu "$LOG" 75
kill "$LAB_PID" 2>/dev/null || true    # the lab usually finishes first
wait "$LAB_PID" 2>/dev/null || true

# ---- the lab finished its script -------------------------------------------
il_assert_grep_fixed "$RECEIPT" "DHCP_ACK_SENT" \
    "the L2 lab served DHCP (no SLIRP involved)"
il_assert_grep_fixed "$RECEIPT" "ARP_REPLY_OK" \
    "idle ARP probe #1 answered from inside the guest (the drain's passive input)"
il_assert_grep_fixed "$RECEIPT" "JUNK_SENT 300" \
    "the unsolicited blast was delivered (300 frames)"
il_assert_grep_fixed "$RECEIPT" "ARP_REPLY2_OK" \
    "idle ARP probe #2 answered through the tail of the load"
il_assert_grep_fixed "$RECEIPT" "LAB_DONE ok" "the lab ran to completion"

# ---- the guest side ---------------------------------------------------------
il_assert_grep "$LOG" "\[dhcp\] PASS: IP 10\.9\.9\.15" \
    "the guest took the lab's DHCP lease"
il_assert_grep "$LOG" "\[e1000\] idle drain: [0-9]+ unsolicited frame" \
    "the idle drain consumed unsolicited frames (receipt line present)"
il_assert_no_grep "$LOG" "RX overrun" \
    "the software queue never clogged: zero overrun lines"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived the blast"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
