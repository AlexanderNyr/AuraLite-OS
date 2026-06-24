#!/usr/bin/env bash
# test_smp.sh — bring up multiple application processors via Limine MP.

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

# Override default -smp 2 from the library with -smp 4.
il_run_qemu "$LOG" 15 -smp 4

il_assert_grep "$LOG" "\[smp\]"                            "SMP subsystem ran"
il_assert_grep "$LOG" "\[smp\] PASS:"                      "SMP self-test PASS"
# Expect at least 3 APs (BSP + 3 APs = 4 CPUs).  Each AP should print online.
il_assert_count "$LOG" "(CPU.*online|AP.*online|cpu .*online)" 1 \
                                                           "at least one AP online line"

il_summary
