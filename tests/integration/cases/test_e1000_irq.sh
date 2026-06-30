#!/usr/bin/env bash
# test_e1000_irq.sh — N2.1 e1000 interrupt-capable RX/TX smoke gate.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N2.1 e1000 IRQ-capable RX/TX"

LOG="$IL_LOGDIR/e1000_irq.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 30

il_assert_grep "$LOG" "\[e1000\] found" "e1000 device detected"
il_assert_grep "$LOG" "\[e1000\] IRQ line [0-9]+ enabled" "e1000 INTx IRQ enabled"
il_assert_grep "$LOG" "\[e1000\] TX/RX rings initialised" "e1000 rings initialised"
il_assert_no_grep "$LOG" "\[e1000\] FAIL|TX timeout" "no e1000 failure marker"
il_assert_no_grep "$LOG" "from KERNEL mode|PANIC|UNHANDLED EXCEPTION" "no kernel exception/panic"

il_summary
