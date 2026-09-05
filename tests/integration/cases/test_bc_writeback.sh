#!/usr/bin/env bash
# test_bc_writeback.sh — RESIDUE2 T3: buffer-cache writeback adoption.
#
# Two lanes the write-THROUGH cache could not prove:
#
#   Lane 1 (hard-kill durability): files are written in boot 1 and the
#   VM is killed by the test's timeout — no `exit`, no kernel_halt, no
#   .sync call.  The ONLY flush path that can save the data is the 1 Hz
#   bc_tick() drain wired into the PIT.  Boot 2 must read every file
#   back from the SAME disk image.
#
#   Lane 2 (coalescing receipt): a burst of small file creates rewrites
#   the same FAT directory/FAT/FSInfo sectors again and again.  The last
#   `[bc] writeback` receipt must show deferred > flushed, i.e. the
#   cache absorbed repeated stores instead of writing through each one.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "buffer-cache writeback (RESIDUE2 T3)"

BUILD="${IL_BUILD:-build}"
DISK="$BUILD/bc-writeback.img"
rm -f "$DISK"
il_make_disk "$DISK" 32 "AURALBC0"

QARGS=(
    -drive "file=$DISK,format=raw,if=none,id=bc0"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=bc0,bus=ahci0.0"
)

# ----------------------------------------------------------------------------
# Lane 1 — write, then let the timeout kill the VM (no clean shutdown).
# ----------------------------------------------------------------------------
LOG1="$IL_LOGDIR/bc_wb_kill.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "write /fat/WB1.TXT hardkill_alpha"
il_send_delay 1
il_send "write /fat/WB2.TXT hardkill_beta"
il_send_delay 1
il_send "cat /fat/WB1.TXT"
il_send_delay 1
il_send "cat /fat/WB2.TXT"
# No "exit" here: the case deliberately lets il_run_qemu's timeout send
# SIGTERM while the guest is idle at the prompt.  Anything reaching the
# disk after the last write got there via bc_tick().
il_send_delay 4

IL_SELFTEST=off il_run_qemu "$LOG1" 20 "${QARGS[@]}"

il_assert_grep "$LOG1" "hardkill_alpha" "lane 1: file 1 served from boot 1"
il_assert_grep "$LOG1" "hardkill_beta"  "lane 1: file 2 served from boot 1"
il_assert_grep "$LOG1" "\[bc\] writeback stores=" \
    "lane 1: writeback receipt printed (tick or sync)"

# ----------------------------------------------------------------------------
# Lane 1b — boot 2 on the SAME image: the hard-killed data must be back.
# ----------------------------------------------------------------------------
LOG2="$IL_LOGDIR/bc_wb_reread.log"
IL_LAST_LOG="$LOG2"

il_send_delay 6
il_send "cat /fat/WB1.TXT"
il_send_delay 1
il_send "cat /fat/WB2.TXT"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOG2" 20 "${QARGS[@]}"

il_assert_grep "$LOG2" "hardkill_alpha" "lane 1: data survived the hard kill (1 Hz drain)"
il_assert_grep "$LOG2" "hardkill_beta"  "lane 1: second file survived too"

# ----------------------------------------------------------------------------
# Lane 2 — burst of creates on the SAME volume; parse the last receipt.
# ----------------------------------------------------------------------------
LOG3="$IL_LOGDIR/bc_wb_burst.log"
IL_LAST_LOG="$LOG3"

il_send_delay 6
for i in 1 2 3 4 5 6 7 8; do
    il_send "write /fat/BST${i}.TXT burst_payload_${i}"
    il_send_delay 0.2
done
il_send_delay 1
il_send "ls /fat"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOG3" 25 "${QARGS[@]}"

il_assert_grep "$LOG3" "burst_payload_8" "lane 2: burst files written"

# Last receipt line: deferred=N flushed=M coalesced=C with N > M (i.e. C>0).
RECEIPT="$(grep "\[bc\] writeback stores=" "$LOG3" | tail -1)"
echo "    receipt: ${RECEIPT}"
DEFERRED="$(echo "$RECEIPT" | sed -n 's/.*stores=\([0-9]*\).*/\1/p')"
FLUSHED="$(echo "$RECEIPT"  | sed -n 's/.*flushed=\([0-9]*\).*/\1/p')"
COALESCED="$(echo "$RECEIPT" | sed -n 's/.*coalesced=\([0-9]*\).*/\1/p')"

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -n "$DEFERRED" ] && [ -n "$FLUSHED" ] && [ "$FLUSHED" -ge 1 ] \
   && [ "$DEFERRED" -gt "$FLUSHED" ]; then
    il_pass "lane 2: stores ($DEFERRED) > flushed ($FLUSHED) — writes coalesced"
else
    il_fail "lane 2: stores ($DEFERRED) > flushed ($FLUSHED) — writes coalesced"
fi
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -n "$COALESCED" ] && [ "$COALESCED" -ge 1 ] \
   && [ "$DEFERRED" -eq $((FLUSHED + COALESCED)) ]; then
    il_pass "lane 2: receipt arithmetic holds (stores = flushed + coalesced)"
else
    il_fail "lane 2: receipt arithmetic holds (stores = flushed + coalesced)"
fi

il_summary
