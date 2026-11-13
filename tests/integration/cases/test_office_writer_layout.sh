#!/usr/bin/env bash
# test_office_writer_layout.sh — OFFICE_PLAN.md OF3 gate: page layout.
#
# OF3 gives the Writer a page: paragraph styles, lists with numbering, a
# table grid, inline images, pagination with page numbers.  The layout is a
# pure function of the model, so this case grades it from receipts:
#
#   1. `aoffice --layout-test` builds the plan's document (heading, bullets,
#      numbered steps, a 4x4 table, an image), lays it out, saves it,
#      reopens it and lays it out again;
#   2. the two layouts agree in line count, page count and per-line kinds;
#   3. the saved file carries the paragraph styles as XML attributes, so a
#      second program (`cat`) can see them;
#   4. nothing panicked.
#
# The visual half (scrolling the page view) is a VNC pass; see
# docs/office.md §6.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office Writer layout (OF3): styles, lists, tables, images, pages"

LOG="$IL_LOGDIR/office_writer_layout.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aoffice --layout-test"
il_send_delay 5
il_send "cat /documents/layout-gate.aod"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 80

il_assert_grep "$LOG" "\[layout\] built: [0-9]+ lines on [0-9]+ page" \
    "layout: the mixed document was laid out and counted"
il_assert_grep "$LOG" "table rows 4" \
    "layout: the 4-row table is four table lines"
il_assert_grep "$LOG" "image 1" \
    "layout: the image paragraph is present"
il_assert_grep "$LOG" "numbering 1" \
    "layout: numbered list reached marker 2."
il_assert_grep "$LOG" "\[layout\] round trip: [0-9]+ lines, [0-9]+ page" \
    "layout: the reopened document reproduces the layout"
il_assert_grep "$LOG" "LAYOUT GATE: PASS" \
    "layout: every gate step agreed"
il_assert_no_grep "$LOG" "LAYOUT GATE: FAIL" "layout: nothing failed"

il_assert_grep "$LOG" "pstyle=\"h1\"" \
    "save: the heading style is in the file"
il_assert_grep "$LOG" "pstyle=\"table\"" \
    "save: the table rows are in the file"
il_assert_grep "$LOG" "pstyle=\"image\"" \
    "save: the image paragraph is in the file"
il_assert_grep "$LOG" "/lib/chart.png" \
    "save: the image path is in the file"

il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary
