#!/usr/bin/env bash
# test_rtl8139.sh — Realtek RTL8139 data path (ARP + ICMP + DNS + TCP).
#
# Background:
#   The netdev layer picks a NIC backend in priority order: e1000, then
#   virtio-net, then the Realtek RTL8139 family.  This test swaps QEMU's
#   NIC to `-device rtl8139` so e1000 and virtio-net are both genuinely
#   absent and the kernel must bring up the Realtek driver, then runs the
#   whole IP stack (DHCP/ICMP/DNS/TCP) over it.
#
#   The shape follows test_virtio_net.sh deliberately: same branch on
#   whether DHCP completed under SLIRP timing, so a slow runner degrades
#   to "the NIC came up and nothing crashed" instead of going red.
#
# The regression this case exists for
# -----------------------------------
#   The first version of the driver used the RX ALLOCATION size
#   (8192+16+1500) as the ring modulus instead of the ring proper that
#   RCR.RBLEN selects (8192).  Everything below passed — DHCP, ping,
#   DNS, sixteen TCP connections — and THEN the receiver died silently
#   after ~128 packets: 23 "ARP timeout" / "resolve/output failed"
#   lines and no error bit set anywhere.  The two assertions on those
#   strings are what pin it; the arithmetic itself is gated on the host
#   by tests/unit/test_rtl8139_ring.c.

set -u

# Drive the IP stack over the Realtek NIC instead of the default e1000.
export IL_NIC="rtl8139"

cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "rtl8139 (data path: ARP + ICMP + DNS + TCP)"

LOG="$IL_LOGDIR/rtl8139.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 25

# The driver must claim the device and become the active NIC — this also
# proves e1000/virtio-net were genuinely absent and the fallback ordering
# in net_init() works.
il_assert_grep "$LOG" "\\[rtl8139\\] found .*10ec:8139"        "RTL8139 found on PCI"
il_assert_grep "$LOG" "\\[rtl8139\\] BAR0 I/O port base"       "BAR0 I/O window programmed"
il_assert_grep "$LOG" "\\[rtl8139\\] MAC "                     "station address read from IDR0"
il_assert_grep "$LOG" "\\[rtl8139\\] ready:.*link=up"          "receiver/transmitter enabled, link up"
il_assert_grep "$LOG" "\\[netdev\\] active NIC: rtl8139"       "rtl8139 is the active NIC"
il_assert_grep "$LOG" "\\[net\\] using NIC: rtl8139"           "stack runs over rtl8139"

# The IRQ receipt: prove the interrupt actually drove a receive rather
# than the polling fallback quietly carrying the boot (the R9/RES-28
# precedent for an IRQ-backed RX path).
il_assert_grep "$LOG" "\\[rtl8139\\] RX via IRQ wake"          "RX was driven by the interrupt"

# The catalog row must report a real data path now, not "known / no data path".
il_assert_grep "$LOG" "10ec:8139 .*driver=rtl8139 status=active" \
                                                               "vmdrv reports the Realtek row as active"

# The ring-modulus regression: the receiver must still be alive at the
# end of the boot.  Both of these were 23 with the allocation-as-modulus
# bug and are 0 with the ring proper.
il_assert_no_grep "$LOG" "\\[net\\] ARP timeout"               "no ARP timeout (RX ring did not stall)"
il_assert_no_grep "$LOG" "resolve/output failed"               "no TX give-up from a stalled receiver"
il_assert_no_grep "$LOG" "\\[rtl8139\\] RX ring desync"        "no ring desynchronisation"

il_assert_no_grep "$LOG" "\\[tcp\\] FAIL"                      "no TCP failure"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                 "no exception in net path"
il_assert_no_grep "$LOG" "PANIC"                               "no panic in net path"

# Branch on whether DHCP actually succeeded over the Realtek NIC.
if grep -qE '\[dhcp\] PASS:' "$LOG"; then
    echo "  ${C_DIM}(DHCP succeeded over rtl8139 → asserting online self-tests)${C_RESET}"
    il_assert_grep "$LOG" "\\[net\\] PASS: ping 10\\.0\\.2\\.2"  "ICMP echo over rtl8139"
    il_assert_grep "$LOG" "(\\[net\\] dns PASS|\\[dns\\] PASS)"  "DNS resolver over rtl8139"
    il_assert_grep    "$LOG" "\\[tcp-x5\\] probing"   "TCP subsystem available over rtl8139 (X5 gate ran)"
    il_assert_no_grep "$LOG" "\\[tcp-x5\\] FAIL"      "X5 boot gate did not fail over rtl8139"
else
    echo "  ${C_DIM}(DHCP didn't complete → using fallback-IP path)${C_RESET}"
    il_assert_grep "$LOG" "our IP: 10\\.0\\.2\\.15"             "static IP assigned over rtl8139"
fi

il_summary
