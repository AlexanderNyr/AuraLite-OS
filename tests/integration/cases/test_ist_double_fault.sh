#!/usr/bin/env bash
# test_ist_double_fault.sh — FIX_R1 test gate (FIXES_PLAN.md, phase R1).
#
# "A test that deliberately overflows the kernel stack produces a
#  DOUBLE-FAULT diagnostic naming the condition — where today (pre-R1) the
#  machine resets silently."
#
# Trigger: /proc/sysrq-trigger command 'o' recurses the caller's kernel stack
# (~2 KiB frames, so the descent cannot leap over the 4 KiB guard page).
# The fatal frame's prologue lands in the guard page with RSP already
# pointing at unmapped memory, the CPU cannot stack the #PF frame, and the
# fault escalates to #DF — on IST1 since FIX_R1.
#
# The second part of the gate ("reverting the IST wiring turns the test back
# into a reset") is exercised by building with -DAURALITE_UNARM_IST: the same
# trigger then produces a triple fault — QEMU restarts the machine, this
# log shows a second boot banner, and every assert below goes red.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FIX_R1 IST double fault"

LOG="$IL_LOGDIR/ist_double_fault.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "write /proc/sysrq-trigger o"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 45

# 1. Normal boot chain reached the shell; the IST self-check shows R1 armed.
il_assert_grep "$LOG" "auralite#"                                   "shell reached before the trigger"
il_assert_grep "$LOG" "IST guard armed -- IST ARMED"                "boot log: #DF gate armed on IST1"

# 2. The overflow trigger was honoured.
il_assert_grep "$LOG" "\[sysrq\] trigger 'o'"                       "sysrq overflow trigger accepted"

# 3. The double fault was caught and NAMED, with the CPU attribution from R0.
il_assert_grep "$LOG" "KERNEL EXCEPTION cpu#[0-9]+: Double Fault"   "double-fault diagnostic names the CPU"
il_assert_grep "$LOG" "faulting RIP=0x[0-9a-f]{16}"                 "faulting RIP printed"
il_assert_grep "$LOG" "Stack trace"                                 "stack trace printed"
il_assert_grep "$LOG" "\[DOUBLE FAULT\] cpu[0-9]+"                  "double-fault handler message names the CPU"
il_assert_grep "$LOG" "running on IST1"                             "handler reports it is on the IST stack"

# 4. No silent reset: before FIX_R1 this trigger produced a triple fault and
#    QEMU would start the boot again — a second banner.  Exactly one banner
#    means the machine halted with the diagnostic on the wire.
boots=$(grep -c "Hello from AuraLite OS kernel!" "$LOG" || true)
if [ "$boots" -eq 1 ]; then
    il_pass "no silent reset (single boot banner; machine halted)"
else
    il_fail "silent reset suspected (boot banner seen $boots times)"
fi

# 5. Sanity: nothing in this run may hit an unexpected panic.
il_assert_no_grep "$LOG" "PANIC" "no unexpected kernel panic"

il_summary
