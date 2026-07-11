#!/usr/bin/env bash
# test_smp.sh — SMP subsystem initialises and reports its configuration.
#
# The kernel deliberately runs BSP-only while PIC/PIT interrupt routing is
# not yet per-CPU safe.  The test therefore asserts that the SMP subsystem
# ran and self-tested, but does not require actual AP bring-up.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "SMP bring-up"

LOG="$IL_LOGDIR/smp.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 15 -smp 4

il_assert_grep "$LOG" "\[smp\]"        "SMP subsystem ran"
il_assert_grep "$LOG" "\[smp\] PASS:"  "SMP self-test PASS"

# Either multi-core APs online OR single-core BSP-only — both are valid.
il_assert_grep "$LOG" "(AP wake disabled|AP.*online|CPUs online|single-CPU system|single-core)" \
               "SMP mode logged (BSP-only or multi-AP)"

il_assert_no_grep "$LOG" "panic|triple fault|Double Fault" "no fatal SMP fault"

il_summary
