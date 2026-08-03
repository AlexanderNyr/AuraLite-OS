#!/usr/bin/env bash
# test_panic_diag.sh — FIX_R0 test gate (FIXES_PLAN.md, phase R0).
#
# "A deliberately triggered kernel fault produces a diagnostic on the serial
#  console rather than silence.  The messages name the CPU."
#
# The fault is triggered through the FIX_R0 /proc/sysrq-trigger hook: writing
# 'c' executes a deliberate kernel NULL-deref.  The serial log must then show
# the lock-free FIX_R0 dump — CPU number, vector, faulting RIP, register
# state, CR2 and a stack trace — and the machine must NOT silently reset
# (a reset would re-run the bootloader and print a second banner).
#
# The same boot also asserts the boot-time IST self-check: pre-FIX_R1 the
# line must report "IST NOT ARMED".  That assert is intentionally explicit
# so it flips (and gets edited to ARMED) the day FIX_R1 lands — exactly the
# visibility R0 exists to provide.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FIX_R0 panic diagnostics"

LOG="$IL_LOGDIR/panic_diag.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Boot to the shell, trigger the deliberate kernel fault, then give the
# halted machine a moment before the runner's timeout kills QEMU.
il_send_delay 8
il_send "write /proc/sysrq-trigger c"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 45

# 1. Normal boot chain reached the shell before the trigger.
il_assert_grep "$LOG" "auralite#"                                   "shell reached before the trigger"

# 2. Boot-time IST self-check ran and reported honestly.  The second assert
#    encodes the PRE-FIX_R1 expectation on purpose: when FIX_R1 arms the
#    IST, this assert goes red and gets flipped to "IST ARMED".
il_assert_grep "$LOG" "\[diag\] IST check:"                         "boot-time IST self-check ran"
il_assert_grep "$LOG" "IST NOT ARMED"                               "pre-R1: IST reported NOT ARMED (flip when FIX_R1 lands)"

# 3. The trigger was honoured.
il_assert_grep "$LOG" "\[sysrq\] trigger 'c'"                       "sysrq crash trigger accepted"

# 4. The deliberate kernel fault produced the R0 serial diagnostic, and the
#    banner NAMES THE CPU (vector 14 = #PF on the deliberate NULL write).
il_assert_grep "$LOG" "KERNEL EXCEPTION cpu#[0-9]+: Page Fault"     "kernel fault diagnostic names the CPU"
il_assert_grep "$LOG" "faulting RIP=0x[0-9a-f]{16}"                 "faulting RIP printed"
il_assert_grep "$LOG" "CR2=0x0000000000000000"                      "CR2 shows the NULL target"
il_assert_grep "$LOG" "RAX=0x[0-9a-f]{16}"                          "register state printed"
il_assert_grep "$LOG" "Stack trace"                                 "stack trace printed"
il_assert_grep "$LOG" "from KERNEL mode \(cpu[0-9]+\)"              "regular exception log also names the CPU"

# 5. No silent reset: exactly one boot means the machine halted with the
#    diagnostic on the wire instead of triple-faulting into a reboot.
boots=$(grep -c "Hello from AuraLite OS kernel!" "$LOG" || true)
if [ "$boots" -eq 1 ]; then
    il_pass "no silent reset (single boot banner; machine halted)"
else
    il_fail "silent reset suspected (boot banner seen $boots times)"
fi

# 6. Sanity: nothing in this run may hit an unexpected panic.
il_assert_no_grep "$LOG" "PANIC" "no unexpected kernel panic"

il_summary
