#!/usr/bin/env bash
# test_gbrowser.sh — WEBVIEW_PLAN phases W0-W7: the GUI browser.
#
# Verifies the deliverable of W0 end to end in QEMU:
#   - the shell launches /apps/gbrowser;
#   - the window opens and the honest limitation statement is drawn;
#   - the blit benchmark reports a real number for the 800x600 page;
#   - the /tmp/gbrowser.frames limit is respected and the program exits
#     cleanly (same convention as /glcube, so CI cannot hang);
#   - no kernel fault anywhere in the path.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Browser GUI"

LOG="$IL_LOGDIR/gbrowser.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "write /tmp/gbrowser.frames 10"
il_send_delay 1
il_send "run gbrowser"

il_run_qemu "$LOG" 45

il_assert_grep "$LOG" "running /apps/gbrowser"                  "shell launched gbrowser"
il_assert_grep "$LOG" "\\[gbrowser\\] window created"            "window opened"
il_assert_grep "$LOG" "\\[gbrowser\\] page rendered"             "page built from HTML"
il_assert_grep "$LOG" "\\[gbrowser\\] paint smoke: PASS"         "paint hash matches the stored reference"
il_assert_grep "$LOG" "\\[gbrowser\\] paint scroll smoke: PASS"  "memmove+band scroll equals full repaint"
il_assert_grep "$LOG" "\\[gbrowser\\] css smoke: PASS"           "W5 stylesheet changes the output"
il_assert_grep "$LOG" "\\[gbrowser\\] canvas smoke: PASS"        "W7 GL canvas rendered and composited"
il_assert_grep "$LOG" "\\[gbrowser\\] blit 800x600: [0-9][0-9]* us/frame" \
    "blit benchmark reported a number"
il_assert_grep "$LOG" "\\[gbrowser\\] tokeniser smoke: PASS"     "W1 tokeniser smoke passed in-guest"
il_assert_grep "$LOG" "\\[gbrowser\\] dom smoke: PASS"           "W2 DOM smoke passed in-guest"
il_assert_grep "$LOG" "\\[gbrowser\\] dom deep test: PASS"       "W2 10k-deep doc hits cap, no stack overflow"
il_assert_grep "$LOG" "\\[gbrowser\\] layout smoke: PASS"         "W3 5000-box layout passed in-guest"
il_assert_grep "$LOG" "\\[gbrowser\\] PASS: 10 frames rendered"  "frame limit respected"
il_assert_grep "$LOG" "\\[gbrowser\\] W0 scaffold complete"       "clean exit"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|triple fault" "no kernel fault"

il_summary
