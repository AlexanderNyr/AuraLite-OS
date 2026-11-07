#!/usr/bin/env bash
# test_wifi_virtual_ap.sh — RESIDUE2 T6 gate: the Wi-Fi MAC layer's
# reference backend, end to end (ledger RES-39).
#
# QEMU has no 802.11 radio, so a chipset backend can never be QEMU-wire
# gated (D2 loud-skip for silicon).  What this gate pins instead is the
# part that must not rot while waiting for hardware: the deterministic
# virtual AP over the wifi_driver_t seam, driven through the FULL
# management flow — scan (probe request → parsed probe response), open
# authentication, association with the AID taken from the wire
# (transmitted 0xC005, masked to 5), and Ethernet frames through the
# LLC/SNAP data path.
#
# RESIDUE3 C6 adds the second half: a DATA PATH.  RESIDUE2 T6 stopped at
# "frames were counted"; now the AP answers at the IP layer — an ARP request
# for its address gets a real ARP reply, an ICMP echo request gets a real echo
# reply, both built from the received bytes — and wifi_netdev.c exposes the
# link as a `struct netdev`, so kernel/net's own `net_ping()` runs over 802.11.
# Case 2 boots with `-nic none`, which makes the virtual link the only NIC;
# the receipt is the stack's own ping line, not a loopback.
#
# The byte layouts themselves are host-pinned by tests/unit/test_wifi_proto.c;
# the host gate runs via `make test-unit`.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Wi-Fi MAC over the virtual AP (RESIDUE2 T6)"

LOG="$IL_LOGDIR/wifi_virtual_ap.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Content-gated, not clock-gated: the C6 boot is longer than T6's was (the
# stack now brings a link up and pings it), and a blind sleep typed the command
# before the shell existed — the polling UART drops it.  Wait for the banner.
il_send_wait "AuraLite OS v0.0.1" 90
il_send "echo gate-end"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 90

il_assert_grep_fixed "$LOG" "[wifi] PASS: virtual AP: scan found 'AuraVirt' ch6 (parsed from the probe response)" \
    "the scan result came from a parsed probe response"
il_assert_grep_fixed "$LOG" "[wifi] PASS: virtual AP: open-auth + assoc, AID=5 from the wire" \
    "auth + assoc completed with the wire AID (0xC005 masked to 5)"
il_assert_grep "$LOG" "\[wifi\] PASS: virtual AP: LLC/SNAP data path carried [0-9]+ frame\(s\), [0-9]+ byte\(s\)" \
    "Ethernet frames rode the 802.11 LLC/SNAP path"
il_assert_no_grep "$LOG" "\\[wifi\\] FAIL" "no Wi-Fi failure marker"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
# --- Case 2: the Wi-Fi link IS the NIC (RESIDUE3 C6 data path) -------------
# `-nic none`: no wired NIC exists, so kernel/net/net.c falls through its whole
# wired chain and attaches the virtual wireless link as the active netdev.
# Everything below is then the IP stack talking over 802.11.
#
# This run drives QEMU directly instead of il_run_qemu(): the library always
# appends `-netdev user -device e1000`, which is exactly what this case must
# NOT have (an e1000 would win the netdev race and the ping would ride SLIRP).
# The receipt needs no shell interaction, only the boot log.
WLOG="$IL_LOGDIR/wifi_virtual_ap_nic.log"
IL_LAST_LOG="$WLOG"
rm -f "$WLOG"

set +e
timeout --foreground 80 "$IL_QEMU" \
    -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
    -m 512M -smp 2 -display none \
    -serial "file:$WLOG" \
    -no-reboot -cpu qemu64 -boot order=c \
    -nic none >/dev/null 2>&1
set -e

il_assert_grep_fixed "$WLOG" "[netdev] active NIC: wifi-virt" \
    "the virtual wireless link became the active network device"
il_assert_grep_fixed "$WLOG" "[net] using NIC: wifi-virt" \
    "the IP stack selected it as its NIC"
il_assert_grep_fixed "$WLOG" "[net] wifi: virtual AP has no DHCP server; static 10.0.2.15/24, AP 10.0.2.2" \
    "static addressing, with the reason stated (a virtual AP runs no DHCP server)"
il_assert_grep_fixed "$WLOG" "[net] ARP request sent for 10.0.2.2" \
    "the stack issued a real ARP request over 802.11"
il_assert_grep_fixed "$WLOG" "[net] ARP reply: 10.0.2.2 is 02:11:22:33:44:55" \
    "the AP answered it with an ARP reply carrying its own MAC"
il_assert_grep_fixed "$WLOG" "[net] ICMP echo reply received from 10.0.2.2 (seq 1)" \
    "an ICMP echo reply came back through the 802.11 data path"
il_assert_grep_fixed "$WLOG" "[net] PASS: ping 10.0.2.2 successful (ICMP echo reply received)" \
    "the stack's own ping succeeded over the wireless link"
il_assert_grep "$WLOG" "\[wifi\] PASS: virtual AP answered [0-9]+ ARP request\(s\) and [0-9]+ ICMP echo request\(s\) with real replies" \
    "the AP's own counters agree: it produced the replies from received frames"
il_assert_grep_fixed "$WLOG" "auralite#" "boot reached the shell with no wired NIC"
il_assert_no_grep_fixed "$WLOG" "[wifi] FAIL" "no Wi-Fi failure marker"
il_assert_no_grep_fixed "$WLOG" "PANIC" "no panic"

il_summary
