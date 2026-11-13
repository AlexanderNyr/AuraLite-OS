#!/usr/bin/env bash
# test_office_slide.sh — OFFICE_PLAN.md OF6 gate: create, reload and present.
#
# The three-slide demo is generated and saved in the guest filesystem, then
# reopened in the GUI editor.  F5 starts the real full-screen presenter; paced
# Space keys advance the deck, and the final Esc closes the show.  A second
# Esc returns to the shell, where the headless show verifier reloads the saved
# XML and emits one receipt per slide.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh

il_section "Office Slide presentation (OF6): save, reload and full-screen show"

GL_LOG="$IL_LOGDIR/office_slide.log"

wait_key() {
    local pattern="$1" key="$2" timeout="${3:-35}"
    gl_wait "$pattern" "$timeout" &
    local waiter=$!
    sleep 0.4
    gl_key "$key"
    wait "$waiter"
}

if gl_boot_bios 'auralite#'; then
    gl_cmd "run aoslide --demo /documents/Untitled.aoslide" \
        'SLIDE DEMO: self-check OK' 60 || true
    gl_cmd "run aoslide /documents/Untitled.aoslide" \
        '\[slide\] gui: deck ready \(3 slides\)' 60 || true

    wait_key 'SLIDE-SHOW: slide 1/3' f5 30 || true
    gl_shot "office_slide_1" || true
    wait_key 'SLIDE-SHOW: slide 2/3' spc 30 || true
    gl_shot "office_slide_2" || true
    wait_key 'SLIDE-SHOW: slide 3/3' spc 30 || true
    gl_shot "office_slide_3" || true
    wait_key 'SLIDE-SHOW: end' spc 30 || true

    # Re-enter once and verify the presenter-specific Esc exit path as well.
    wait_key 'SLIDE-SHOW: slide 1/3' f5 30 || true
    wait_key 'SLIDE-SHOW: end \(Esc\)' esc 30 || true

    # Esc now closes the editor to the shell.
    gl_key esc
    sleep 2
    gl_cmd "run aoslide --show /documents/Untitled.aoslide" \
        'SLIDE-SHOW: PASS \(3 slides, validated\)' 60 || true
    gl_stop
fi

il_assert_grep "$GL_LOG" 'SLIDE DEMO: wrote /documents/Untitled.aoslide' \
    "demo: wrote a three-slide .aoslide file"
il_assert_grep "$GL_LOG" 'SLIDE DEMO: self-check OK' \
    "demo: serialize/parse self-check passed"
il_assert_grep "$GL_LOG" '\[slide\] gui: deck ready \(3 slides\)' \
    "editor: reloaded the saved three-slide deck"
il_assert_grep "$GL_LOG" 'SLIDE-SHOW: slide 1/3' "presenter: slide 1 receipt"
il_assert_grep "$GL_LOG" 'SLIDE-SHOW: slide 2/3' "presenter: Space advanced to slide 2"
il_assert_grep "$GL_LOG" 'SLIDE-SHOW: slide 3/3' "presenter: Space advanced to slide 3"
il_assert_grep "$GL_LOG" 'SLIDE-SHOW: end' "presenter: final advance ended the show"
il_assert_grep "$GL_LOG" 'SLIDE-SHOW: end \(Esc\)' "presenter: Esc exits the show"
il_assert_grep "$GL_LOG" 'SLIDE-SHOW: PASS \(3 slides, validated\)' \
    "reload: headless verifier saw all three saved slides"
il_assert_no_grep "$GL_LOG" 'PANIC' "no kernel panic"
il_assert_no_grep "$GL_LOG" 'UNHANDLED EXCEPTION' "no kernel exception"

il_summary
