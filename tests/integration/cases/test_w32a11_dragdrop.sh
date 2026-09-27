#!/usr/bin/env bash
# W32A-11 incremental guest effects: real Win64 IDataObject/IDropTarget callbacks across two
# windows, and a separate native task dropping an on-disk file into a PE HWND.
# This is NOT the full W32A-11 gate: no pinned-app CLSID table or timed theme.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "W32A-11 OLE window drag and compositor file drop"
LOG="$IL_LOGDIR/w32a11_dragdrop.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send "run /apps/w32run /tests/w32a11_drag.exe"
il_send_wait "A11-DRAG-GETDATA-OK" 70
il_send "run /tests/w32a11_native_drop"
il_send_wait "A11-SENDER-OK" 35
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG" 135

il_assert_grep "$LOG" "w32run: .*w32a11_drag\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "OLE drag PE mapped and imports bound"
il_assert_grep "$LOG" "A11-DRAG-GETDATA-OK" "drag crossed HWNDs; IDataObject handed CF_HDROP medium to target"
il_assert_grep "$LOG" "'/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" \
    "OLE PE success status"
il_assert_grep "$LOG" "A11-FILE-READY" "separate PE receiver created ACCEPTFILES HWND"
il_assert_grep "$LOG" "A11-SENDER-DELIVERED-UNICODE" "native task submitted UTF-8 path via compositor"
il_assert_grep "$LOG" "A11-FILE-UNICODE-OK" "PE queried UTF-16 HDROP and read Unicode-named file via CreateFileW"
il_assert_grep "$LOG" "A11-SENDER-OK" "native task reaped PE with status 78"
il_assert_grep "$LOG" "'/tests/w32a11_native_drop' \(tid [0-9]+\) exited \(code=78\)" \
    "sender success status"
il_assert_no_grep "$LOG" "A11-(DRAG|FILE|SENDER)-FAIL|unresolved import" \
    "no fixture failure or unresolved import"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no guest fault"
il_assert_grep "$LOG" "Goodbye!" "shell survived"
il_summary
