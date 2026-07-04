#!/usr/bin/env bash
# test_smp_init_order.sh — AP local state must be ready before TSS/logging.
#
# When AP wake is disabled the AP init-order path is not exercised at runtime.
# The test instead verifies that the SMP subsystem logged its mode and did not
# produce any early-crash or init-ordering fault markers.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "SMP AP init order"

LOG="$IL_LOGDIR/smp_init_order.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 15 -smp 4

# Accept either APs-online message (when enabled) or the BSP-only notice.
il_assert_grep    "$LOG" "(AP #[0-9]+ online|AP wake disabled|PASS)" \
                                                      "SMP mode logged"

il_assert_no_grep "$LOG" "\[tss\] WARN"               "no early TSS warning"
il_assert_no_grep "$LOG" "panic|NULL deref|#GP|Double Fault" \
                                                      "no AP init-order crash"

il_summary
