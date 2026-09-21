#!/usr/bin/env bash
# test_w32a8_comctl32.sh — W32APP_PLAN.md phase W32A-8 gate.
#
# The phase's claim, in guest: comctl32.dll is a real control engine.
# One NASM PE (w32a8_comctl32.asm) drives every control end to end --
# InitCommonControlsEx (the documented ICC gates) and the WS_CHILD
# class gate, a toolbar built with CreateToolbarEx (button states,
# click -> WM_COMMAND id), a status window, a listview (items, labels,
# state mask, NM_CLICK/LVN_ITEMCHANGED order, LVM_HITTEST, deletes),
# a treeview on the real ag tree widget (handles, TVGN walks,
# TVN_SELCHANGED, expand order, TVM_GETITEMRECT, the gutter click,
# subtree delete), a tab (TCM_SETCURSEL silent vs. strip click ->
# TCN_SELCHANGE), a progress bar, a tooltip (TTM_RELAYEVENT ->
# TTM_POP), a header (HDN_ITEMCLICKW), ImageList (AddMasked mask
# readback, ReplaceIcon, Draw with GetPixel receipts, drag frames),
# SetWindowSubclass/RemoveWindowSubclass (install order, reference
# data, double-remove refused), PropertySheetW over in-memory
# DLGTEMPLATE word streams (PSN_APPLY both directions, IDOK), and the
# refusal contracts (LoadIconWithScaleDown HRESULTs, TB_CUSTOMIZE
# ERROR_CALL_NOT_IMPLEMENTED, _TrackMouseEvent).  A marker appears
# only after every check in that section passed; a failure prints
# A8-<SECTION>-FAIL and exits 79.
#
# Markers the fixture prints (14 sections):
#   A8-INIT-OK       InitCommonControlsEx; the WS_CHILD class gate
#   A8-WSCHILD-OK    a non-comctl WS_CHILD create refused, comctl ones
#                    accepted
#   A8-TOOLBAR-OK    CreateToolbarEx; button count/state; click
#                    delivers WM_COMMAND
#   A8-STATUS-OK     CreateStatusWindowW; SB_SETPARTS; GetTextLength
#   A8-LISTVIEW-OK   items/labels/state; hit test; the notify order;
#                    deletes replay to the widget
#   A8-TREEVIEW-OK   insert/expand/select; TVGN walks; GETITEMRECT;
#                    the gutter collapse; subtree delete
#   A8-TAB-OK        SETCURSEL silent; strip click notifies
#   A8-PROGRESS-OK   PBM_SETRANGE/SETPOS/SETSTEP/STEPIT/DeltaPos;
#                    PBRANGE readback
#   A8-TOOLTIP-OK    TTM_ADDTOOL/RELAYEVENT show, TTM_POP hides
#   A8-IMAGELIST-OK  AddMasked mask readback; ReplaceIcon; Draw
#                    pixels; drag frame lifetime
#   A8-SUBCLASS-OK   install order; reference data; remove restores;
#                    double-remove refused
#   A8-PSHEET-OK     PropertySheetW pages; PSN_APPLY both directions;
#                    IDOK path
#   A8-REFUSE-OK     LoadIconWithScaleDown HRESULTs; TB_CUSTOMIZE
#                    120; _TrackMouseEvent contracts
#   A8-HEADER-OK     HDM_INSERTITEM; HDN_ITEMCLICKW on click
#   W32A8-COMCTL-OK  final marker
#
# One boot: the v5/v6 selection contract is host-gated (the a8 unit
# suite pins the two renderings against the palette seam); the guest
# gate walks the functional surface once.  Exit code 78 is the
# fixture's own success path; 79 is any failed check; 1 means the
# image never loaded.  nasm is an unconditional build requirement, so
# a missing fixture fails at build time.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-8 integration: COMCTL32 control engine"

a8_pass() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_pass "$@"; }
a8_fail() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_fail "$@"; }

LOG1="$IL_LOGDIR/w32a8_comctl32.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/w32run /tests/w32a8_comctl32.exe"
il_send_delay 16
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG1" 240

# --- the image loaded and imports were bound -----------------------------------
il_assert_grep "$LOG1" "w32run: .*w32a8_comctl32\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "fixture PE mapped and imports resolved"

# --- one marker per section ----------------------------------------------------
for m in INIT WSCHILD TOOLBAR STATUS LISTVIEW TREEVIEW TAB PROGRESS \
         TOOLTIP IMAGELIST SUBCLASS PSHEET REFUSE HEADER; do
    il_assert_grep "$LOG1" "A8-$m-OK" \
        "the fixture passed its $m section"
done

il_assert_no_grep "$LOG1" "A8-[A-Z-]*-FAIL" \
    "no section reported a failure"
il_assert_grep "$LOG1" "W32A8-COMCTL-OK" \
    "the fixture reported the full sequence succeeded"
il_assert_grep "$LOG1" "'/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" \
    "the PE exited with its success status (78)"

il_assert_no_grep "$LOG1" "unresolved import" \
    "no import was left unbound"
il_assert_no_grep "$LOG1" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no kernel fault"
il_assert_grep "$LOG1" "Goodbye!" \
    "shell survived and exited cleanly"

il_summary
