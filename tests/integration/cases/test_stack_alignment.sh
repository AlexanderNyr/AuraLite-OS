#!/usr/bin/env bash
# test_stack_alignment.sh — RESIDUE3 C2: the 16-byte C ABI, proved in the guest.
#
# The static model (tools/check_abi_align.py, tests/unit/test_abi_align.sh)
# reasons about the phase arithmetic in the assembly.  This case is the other
# half: the kernel executes an fxsave + movaps on a 16-aligned local INSIDE an
# IRQ and INSIDE a syscall (kernel/arch/x86_64/abi_probe.c).  fxsave #GPs on a
# misaligned operand, so a wrong phase is a hard fault in the log, not a
# missing log line.
#
# The two PASS lines are the gate.  The shell round-trip afterwards is the
# control: the probe must not have broken the syscall path it measures.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE3 C2: 16-byte C-ABI stack alignment (IRQ + syscall contexts)"

LOG="$IL_LOGDIR/stack_alignment.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 12
il_send "uname"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 45

# 1. the probe was armed (it runs once, by design, and says so)
il_assert_grep "$LOG" "\[abi\] probe armed" \
    "the ABI probe arms before the shell starts"

# 2. the two contexts that reach C from assembly, each with its own receipt
il_assert_grep "$LOG" "\[abi\] PASS: fxsave\+movaps in IRQ context" \
    "fxsave+movaps from IRQ context: RSP%16==0 held at the C entry"
il_assert_grep "$LOG" "\[abi\] PASS: fxsave\+movaps in syscall context" \
    "fxsave+movaps from syscall context: RSP%16==0 held at the C entry"

# 3. a misaligned entry would have produced exactly these; their absence is
#    the negative half of the receipt
il_assert_no_grep "$LOG" "\[abi\] FAIL"                  "no ABI probe failure"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"           "no exception during the probes"
il_assert_no_grep "$LOG" "#GP"                           "no general-protection fault"

# 4. the control: syscalls still work after the probe ran in the path
il_assert_grep "$LOG" "auralite"                          "the shell banner still appears"
il_assert_grep "$LOG" "uname"                            "a syscall round-trip still completes"

il_summary
