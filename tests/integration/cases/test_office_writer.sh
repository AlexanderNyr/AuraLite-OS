#!/usr/bin/env bash
# test_office_writer.sh — OFFICE_PLAN.md OF2 gate: the Writer's document core,
# in the guest, through the file system.
#
# What OF2 promised: a document model that survives its own file format, an
# undo stack that cannot drift out of step with it, and a save path whose
# output another program can read.  Three assertions stand for that here, and
# each one is made from a RECEIPT the guest printed, not from the absence of a
# crash:
#
#   1. `aoffice --writer-test` types through the same functions a keystroke
#      calls, selects all, bolds, undoes, redoes, saves, and reopens the file
#      through the VFS.  It prints the sizes it moved and `WRITER GATE: PASS`
#      only if every step agreed.
#   2. The saved file is a real file: the shell's own `cat` prints it, and what
#      comes out is XML with the text and the bold run in it.  A private blob
#      would pass assertion 1 and fail this one.
#   3. A second instance opens what the first one wrote (the writer gate's
#      reopen step, run twice), which is what "reload in a second instance"
#      means when there is no window to look at.
#
# The interactive window (Start > Office > Office: type, Ctrl+A, Ctrl+B,
# Ctrl+S) is exercised live through VNC; this is the case CI can run on every
# push, with no display and no waiting for a screenshot to settle.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office Writer (OF2): model, undo, save, reopen"

LOG="$IL_LOGDIR/office_writer.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aoffice --writer-test"
il_send_delay 5
# The file the Writer just wrote, printed by a program that knows nothing
# about documents.
il_send "cat /documents/writer-gate.aod"
il_send_delay 3
# A second instance: it must reopen the same file and say so again.
il_send "run aoffice --writer-test"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 80

# ---- 1. the writer gate --------------------------------------------------
il_assert_grep "$LOG" "\[writer\] text buffer [0-9]+ bytes" \
    "writer: the model's capacities are reported"
il_assert_grep "$LOG" "\[writer\] typed [0-9]+ bytes, 2 paragraphs" \
    "writer: typing through the keystroke path built two paragraphs"
il_assert_grep "$LOG" "\[writer\] select-all \+ bold" \
    "writer: Ctrl+A and Ctrl+B reached the model"
il_assert_grep "$LOG" "\[writer\] undo/redo: bold removed and restored" \
    "writer: undo cleared the bold and redo restored the document byte for byte"
il_assert_grep "$LOG" "WRITER GATE: PASS" \
    "writer: every gate step agreed"
il_assert_no_grep "$LOG" "WRITER GATE: FAIL" "writer: nothing failed"
il_assert_no_grep "$LOG" "\[writer\] no match" "writer: the search found its text"

# ---- 2. the file is a file ----------------------------------------------
il_assert_grep "$LOG" "\[writer\] saved /documents/writer-gate.aod: [0-9]+ bytes of XML" \
    "save: the document was written to /documents and the size was reported"
il_assert_grep_fixed "$LOG" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
    "save: the file starts with an XML declaration"
il_assert_grep_fixed "$LOG" "<aodoc version=\"1\">" \
    "save: the file declares the native document format and its version"
il_assert_grep "$LOG" "<r style=\"b\"" \
    "save: the bold run is in the file, not only in memory"
il_assert_grep_fixed "$LOG" "Первый абзац" \
    "save: the Cyrillic text is in the file as literal UTF-8"
il_assert_grep_fixed "$LOG" "— 2026" \
    "save: the em dash and the year survived the round trip"
il_assert_grep_fixed "$LOG" "</aodoc>" \
    "save: the document element is closed"

# ---- 3. a second instance ------------------------------------------------
il_assert_count "$LOG" "WRITER GATE: PASS" 2 \
    "reopen: a second instance opened what the first one wrote"
il_assert_count "$LOG" "\[writer\] opened /documents/writer-gate.aod" 2 \
    "reopen: the file was read back twice, each time through the reader"

# ---- nothing caught fire -------------------------------------------------
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary
