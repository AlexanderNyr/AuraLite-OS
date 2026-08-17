#!/usr/bin/env bash
# tests/integration/i386_parity_smoke.sh -- I386_PLAN phase I8 smoke test.
#
# The storage and network gates that moved from I7, executed on the
# real `make iso` image under qemu-system-i386 with an e1000 on SLIRP:
#
#   storage: ATA PIO IDENTIFY, boot-sector read against KNOWN bytes
#     (the 0x55AA our own Stage 1 booted from), and a write/readback/
#     restore cycle on the last sector -- the same sector-level
#     guarantee the 64-bit AHCI self-test gives, on the controller
#     the machine actually boots from;
#   network: DHCP DISCOVER->ACK on SLIRP (lease 10.0.2.15), gateway
#     ARP resolution, and an ICMP echo whose PAYLOAD is verified
#     byte-for-byte in the reply -- "ping got a reply" is the gate
#     I7's text specified, moved here verbatim;
#   the first SHARED source in the 32-bit kernel: drivers/pci/pci.c
#     found the NIC, which is the I6 thesis (portable code compiles at
#     both widths) carrying live traffic;
#   and the whole earlier gauntlet (pmm/vmm/heap/sched/user/init32)
#     still green in the same boot.
#
# Plus the standing x86_64 no-regression pair.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_parity.log"
LOG64="$BUILD/i386_parity_x64.log"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-parity] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
run_qemu() {
    local bin="$1" log="$2"
    rm -f "$log"
    timeout 45 "$bin" \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial file:"$log" -no-reboot \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-parity] OK   %s\n' "$desc"
    else
        printf '  [i386-parity] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-parity] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-parity] OK   %s\n' "$desc"
    fi
}

# ---- the i386 parity boot ----
run_qemu qemu-system-i386 "$LOG32"

# storage
assert_grep    "$LOG32" "\[ata\] primary master: .* sectors"           "i386: ATA IDENTIFY (PIO LBA28)"
assert_grep    "$LOG32" "\[ata\] PASS: boot-sector read"               "i386: read proven against known bytes + write/readback/restore"
# network (the I7-inherited gate, verbatim)
assert_grep    "$LOG32" "\[net\] e1000 82540EM at PCI"                 "i386: NIC found -- shared pci.c carrying live config reads"
assert_grep    "$LOG32" "\[net\] DHCP lease: 10.0.2.15"                "i386: DHCP lease acquired on SLIRP"
assert_grep    "$LOG32" "\[net\] ARP: gateway is"                      "i386: gateway ARP resolved"
assert_grep    "$LOG32" "\[net\] PASS: lease + ARP + echo reply (payload verified)" "i386: ICMP echo round-trip, payload byte-checked"
# the earlier gauntlet, same boot
assert_grep    "$LOG32" "\[pmm\] PASS"                                 "i386: I3 pmm gate still green"
assert_grep    "$LOG32" "\[vmm\] PASS"                                 "i386: I3 vmm gate still green"
assert_grep    "$LOG32" "\[heap\] PASS"                                "i386: I3 heap gate still green"
assert_grep    "$LOG32" "\[sched\] PASS"                               "i386: I4 sched gate still green"
assert_grep    "$LOG32" "\[user\] PASS"                                "i386: I4 user gate still green"
assert_grep    "$LOG32" "\[init\] PASS"                                "i386: I5 init gate still green"
assert_grep    "$LOG32" "auralite# "                                   "i386: I7 shell prompt still reached"
assert_no_grep "$LOG32" "\[ata\] FAIL\|\[net\] FAIL"                   "i386: no storage/network failures"
assert_no_grep "$LOG32" "UNHANDLED EXCEPTION"                          "i386: no unexpected kernel faults"

# ---- the standing x86_64 regression gate ----
run_qemu qemu-system-x86_64 "$LOG64"
assert_grep    "$LOG64" "Hello from AuraLite OS kernel!"               "x86_64: kernel banner unchanged"
assert_no_grep "$LOG64" "(i386)"                                       "x86_64: no 32-bit artefacts in the log"

if [ "$fail" -ne 0 ]; then
    echo "[i386-parity] FAILED"
    exit 1
fi
echo "[i386-parity] all assertions passed"
