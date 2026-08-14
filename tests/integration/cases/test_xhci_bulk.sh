#!/usr/bin/env bash
# test_xhci_bulk.sh — USB_PLAN U5 gate: real bulk transfers and MSC.
#
# This is the assertion no forgery can pass.
#
# The deleted stub answered a 512-byte bulk IN with a fabricated "sector"
# reading AURALUSB, a 36-byte one with an INQUIRY naming QEMU HARDDISK, and
# an 8-byte one with a READ CAPACITY of 16384 sectors -- all independent of
# whatever disk QEMU was actually given.  test_usb_xhci.sh asserted
# "READ(10) works" against exactly that and reported 8/8 green.
#
# So this case writes a known pattern into the backing image with dd and
# demands the OS read those same bytes back.  It also deliberately uses a
# capacity that is NOT the fabricated 16384 sectors, so a stale stub would
# be caught by the geometry too.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "xHCI bulk transfers + MSC (USB_PLAN U5)"

LOG="$IL_LOGDIR/xhci_bulk.log"
DISK="$IL_LOGDIR/xhci_bulk_disk.img"
rm -f "$LOG" "$DISK"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# 12 MiB = 24576 sectors: deliberately different from the stub's 16384.
dd if=/dev/zero of="$DISK" bs=1M count=12 status=none

# A recognisable, non-fabricable pattern in sector 0.  "U5-REAL-BULK-" plus
# a counter, filling the sector, with a 0x55AA signature at the end.
python3 - "$DISK" <<'PY'
import sys
sec = bytearray(512)
tag = b'U5-REAL-BULK-'
off = 0
n = 0
while off + len(tag) + 4 <= 510:
    sec[off:off+len(tag)] = tag
    off += len(tag)
    num = b'%04d' % n
    sec[off:off+4] = num
    off += 4
    n += 1
sec[510] = 0x55
sec[511] = 0xAA
with open(sys.argv[1], 'r+b') as f:
    f.write(bytes(sec))
PY

il_run_qemu "$LOG" 50 \
    -device "qemu-xhci,id=xhci" \
    -drive "if=none,id=xdisk,file=$DISK,format=raw" \
    -device "usb-storage,bus=xhci.0,drive=xdisk"

# The BOT command path must work at all.
il_assert_grep "$LOG" "\[msc\] mass storage candidate: addr=. .*bulk_in=0x81 bulk_out=0x02" \
    "MSC bound to real bulk endpoints"

# INQUIRY comes from the device.
#
# Note: "QEMU HARDDISK" is genuinely what QEMU's usb-storage reports -- the
# deleted stub hardcoded it *because* that is the real answer, so its
# presence proves nothing either way and asserting its absence was wrong.
# The capacity and sector-content checks below are what actually
# distinguish real data from fabricated data.
il_assert_grep_fixed "$LOG" "[msc] INQUIRY: vendor 'QEMU' product 'QEMU HARDDISK'" \
    "INQUIRY strings parsed and trimmed correctly"

# READ CAPACITY must match the image we actually created (24576 sectors),
# not the stub's fixed 16384.
il_assert_grep_fixed "$LOG" "[msc] capacity: 24576 sectors" \
    "READ CAPACITY reports the real disk geometry"
il_assert_no_grep_fixed "$LOG" "capacity: 16384 sectors" \
    "not the stub's fabricated capacity"

# The decisive one: the bytes read must be the bytes written.
il_assert_grep_fixed "$LOG" "[msc] sector 0 first bytes: 55 35 2d 52 45 41 4c 2d 42 55 4c 4b 2d 30 30 30" \
    "sector 0 matches the pattern dd wrote ('U5-REAL-BULK-000')"
il_assert_no_grep_fixed "$LOG" "41 55 52 41 4c 55 53 42" \
    "not the fabricated AURALUSB sector"

il_assert_grep_fixed "$LOG" "[msc] PASS: USB mass storage READ(10) works" \
    "READ(10) verdict is now earned, not asserted"

il_assert_no_grep "$LOG" "transfer timeout|Page Fault|PANIC|\[msc\] FAIL" \
    "no timeouts, faults or MSC failures"

il_summary
