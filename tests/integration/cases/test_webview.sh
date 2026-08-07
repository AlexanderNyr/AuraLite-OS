#!/usr/bin/env bash
# test_webview.sh — WEBVIEW_PLAN phase W0 scaffold.
#
# Verifies the deliverable of W0 end to end in QEMU:
#   - the shell launches /apps/webview;
#   - the window opens and the honest limitation statement is drawn;
#   - the blit benchmark reports a real number for the 800x600 page;
#   - the /tmp/webview.frames limit is respected and the program exits
#     cleanly (same convention as /glcube, so CI cannot hang);
#   - no kernel fault anywhere in the path.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "WebView W0 scaffold"

LOG="$IL_LOGDIR/webview.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "write /tmp/webview.frames 10"
il_send_delay 1
il_send "run webview"

il_run_qemu "$LOG" 45

il_assert_grep "$LOG" "running /apps/webview"                  "shell launched webview"
il_assert_grep "$LOG" "\\[webview\\] window created"            "window opened"
il_assert_grep "$LOG" "\\[webview\\] page rendered"             "page built from HTML"
il_assert_grep "$LOG" "\\[webview\\] paint smoke: PASS"         "paint hash matches the stored reference"
il_assert_grep "$LOG" "\\[webview\\] paint scroll smoke: PASS"  "memmove+band scroll equals full repaint"
il_assert_grep "$LOG" "\\[webview\\] blit 800x600: [0-9][0-9]* us/frame" \
    "blit benchmark reported a number"
il_assert_grep "$LOG" "\\[webview\\] tokeniser smoke: PASS"     "W1 tokeniser smoke passed in-guest"
il_assert_grep "$LOG" "\\[webview\\] dom smoke: PASS"           "W2 DOM smoke passed in-guest"
il_assert_grep "$LOG" "\\[webview\\] dom deep test: PASS"       "W2 10k-deep doc hits cap, no stack overflow"
il_assert_grep "$LOG" "\\[webview\\] layout smoke: PASS"         "W3 5000-box layout passed in-guest"
il_assert_grep "$LOG" "\\[webview\\] PASS: 10 frames rendered"  "frame limit respected"
il_assert_grep "$LOG" "\\[webview\\] W0 scaffold complete"       "clean exit"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|triple fault" "no kernel fault"

il_summary
