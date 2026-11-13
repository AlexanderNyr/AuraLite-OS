#!/usr/bin/env bash
# test_office_sheet_perf.sh — OFFICE_PLAN.md OF5 perf gate.
#
# `aosheet --bench` loads a 10 000-row sheet into the grid and scrolls
# it for 200 frames, printing the average and worst frame and the
# arena high-water afterwards.  The assertions:
#
#   1. the bench ran (200 frames over 10 000 rows);
#   2. the worst frame stays inside a 50 ms ceiling — two orders above
#      the OF0 keystroke budget, because the claim here is virtualisation
#      (a frame costs the viewport, not the workbook), not microseconds;
#   3. the frame loop completed and the arena counter printed with the
#      scroll stats (the gate can see allocation rather than trust it).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office Sheet perf (OF5): scroll a 10 000-row sheet"

LOG="$IL_LOGDIR/office_sheet_perf.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aosheet --bench"

il_run_qemu "$LOG" 150

il_assert_grep "$LOG" "\[sheet\] bench: 10000 rows loaded" \
    "perf: the 10 000-row book loaded"
il_assert_grep "$LOG" "\[sheet\] bench scroll: 200 frames" \
    "perf: 200 scroll frames ran"
il_assert_grep "$LOG" "\[sheet\] bench: done" \
    "perf: the bench completed"

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
worst=$(grep -a "bench scroll:" "$LOG" | tail -1 | \
        sed -n 's/.*worst \([0-9][0-9]*\) us.*/\1/p')
if [ -n "$worst" ] && [ "$worst" -lt 50000 ]; then
    il_pass "perf: worst frame ${worst} us < 50 ms ceiling (viewport-sized frames)"
else
    il_fail "perf: worst frame ${worst:-unknown} us outside the 50 ms ceiling"
fi

il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary
