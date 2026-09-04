#!/usr/bin/env bash
# test_irq_ap_wake.sh — RESIDUE2 T2 gate: ledger RES-16, a device IRQ waking
# a hlt-ed AP.  The kernel selftest (SYS_IRQ_AP_WAKE, triggered by
# /tests/irqapwake) parks a hlt looper on cpu 1, aims the RTC's I/O APIC
# redirection (GSI8) at that AP's id, and counts the deliveries.  A pre-T2
# tree has no runtime routing and no receipt line.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE2 T2 device-IRQ-wakes-hlt-ed-AP receipt (-smp 4)"

LOG="$IL_LOGDIR/irq_ap_wake.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run irqapwake"
il_send_delay 15
il_send "exit"

il_run_qemu "$LOG" 90 -smp 4

il_assert_grep "$LOG" "\[smpwake\] PASS.*RES-16 receipt" \
    "RTC device IRQs delivered to cpu1 and woke its hlt loop"
il_assert_grep "$LOG" "IRQAPWAKE PASS" \
    "userspace trigger reports kernel receipt success"
il_assert_no_grep "$LOG" "panic|triple fault|Double Fault" \
    "no fatal fault during the retarget"

il_summary
