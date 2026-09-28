#!/usr/bin/env bash
# W32A-11 PHASE GATE — OLE32-lite, drag-and-drop, OLEAUT32, IMM32, UxTheme.
#
# This is the single named deliverable gate for the whole phase (the earlier
# test_w32a11_{core_subset,dragdrop,theme,tokens}.sh cases each pin one slice;
# this one boots ONCE and drives every fixture so the phase's five task groups
# are proven together, mapped one-to-one onto the plan's checklist):
#
#   COM-lite core   -> w32a11_core.exe (COM init/nesting, task memory) +
#                      w32a11_probe.exe (CLSIDFromProgID over the hive,
#                      CoCreateInstance against the committed EMPTY activation
#                      table with typed refusals, whole-request probe lines).
#   Drag-and-drop   -> w32a11_drag.exe (in-process IDataObject/IDropTarget,
#                      CF_HDROP STGMEDIUM) + w32a11_native_drop (a native task
#                      dropping a Unicode-named on-disk file into a PE HWND,
#                      DragQueryFileW yields the real path, contents checked).
#   OLEAUT32        -> w32a11_core.exe (BSTR/VARIANT vectors + the eight
#                      documented ordinals #2/#4/#6/#7/#9/#10/#149/#150).
#   IMM32           -> w32a11_core.exe (nine typed FAIL-CLEAN stubs).
#   UxTheme         -> w32a11_theme_v5.exe / w32a11_theme_v6.exe (v5 fallback
#                      vs v6 GDI pixels, part refusals, zero-duration animation;
#                      timed animation is refused, not faked).
#
# Documented non-goals asserted elsewhere and NOT claimed here: cross-process
# COM marshalling, OLE drag sources outside the personality, timed UxTheme
# animation, and non-empty activation (no pinned-app CLSID/IID pair observed).
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-11 phase gate: OLE32-lite / drag-drop / OLEAUT32 / IMM32 / UxTheme"
LOG="$IL_LOGDIR/w32a11_ole.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
# COM-lite core, OLEAUT32, IMM32, clipboard formats, BUTTON theme pixels.
il_send "run /apps/w32run /tests/w32a11_core.exe"
il_send_wait "W32A11-CORE-SUBSET-OK" 75
# REAL activation surface: hive-backed ProgID + empty table typed refusals.
il_send "run /apps/w32run /tests/w32a11_probe.exe"
il_send_wait "A11-PROBE-FIXTURE-OK" 75
# In-process OLE drag between two compositor HWNDs.
il_send "run /apps/w32run /tests/w32a11_drag.exe"
il_send_wait "A11-DRAG-GETDATA-OK" 70
# Cross-task file drop of a Unicode-named on-disk file into a PE HWND.
il_send "run /tests/w32a11_native_drop"
il_send_wait "A11-SENDER-OK" 45
# UxTheme v5 fallback vs v6 GDI pixels and zero-duration buffered animation.
il_send "run /apps/w32run /tests/w32a11_theme_v5.exe"
il_send_wait "A11-THEME-V5-OK" 45
il_send "run /apps/w32run /tests/w32a11_theme_v6.exe"
il_send_wait "A11-ANIMATION-OK" 45
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG" 300

# ---- COM-lite core + OLEAUT32 + IMM32 (w32a11_core.exe) ----
il_assert_grep "$LOG" "w32run: .*w32a11_core\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "core PE mapped and imports resolved"
for m in COM VARIANT ORDINALS IMM CLIPFMT THEME; do
    il_assert_grep "$LOG" "A11-$m-OK" "guest $m effects checked"
done
il_assert_grep "$LOG" "W32A11-CORE-SUBSET-OK" "core section finished (COM/VARIANT/ORDINALS/IMM/CLIPFMT/THEME)"

# ---- COM-lite activation table (w32a11_probe.exe) ----
il_assert_grep "$LOG" "w32run: .*w32a11_probe\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "class-probe PE mapped through the real binder"
il_assert_grep_fixed "$LOG" "w32a11-clsid-probe: CLSID=12345678-9abc-def0-1122-334455667788 IID=00000000-0000-0000-c000-000000000046 CLSCTX=1" \
    "full GUID pair logged in canonical order"
il_assert_grep_fixed "$LOG" "w32a11-progid-probe: UTF16=AuraW32A11.Probe truncated=0 result=0x00000000" \
    "hive-seeded ProgID resolved in-guest, outcome stamped"
il_assert_grep_fixed "$LOG" "w32a11-progid-probe: UTF16=AuraW32A11.Bad truncated=0 result=0x800401f3" \
    "malformed registered CLSID refused (CO_E_CLASSSTRING) with its outcome stamped"
il_assert_grep "$LOG" "A11-PROBE-FIXTURE-OK" \
    "real lookup round-trips the hive and typed refusals cleared outputs"

# ---- Drag-and-drop (w32a11_drag.exe + w32a11_native_drop) ----
il_assert_grep "$LOG" "w32run: .*w32a11_drag\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "OLE drag PE mapped and imports bound"
il_assert_grep "$LOG" "A11-DRAG-GETDATA-OK" "drag crossed HWNDs; IDataObject handed CF_HDROP medium to target"
il_assert_grep "$LOG" "A11-FILE-READY" "separate PE receiver created ACCEPTFILES HWND"
il_assert_grep "$LOG" "A11-SENDER-DELIVERED-UNICODE" "native task submitted UTF-8 path via compositor"
il_assert_grep "$LOG" "A11-FILE-UNICODE-OK" "PE queried UTF-16 HDROP and read Unicode-named file via CreateFileW"
il_assert_grep "$LOG" "A11-SENDER-OK" "native task reaped PE receiver with status 78"
il_assert_grep "$LOG" "'/tests/w32a11_native_drop' \(tid [0-9]+\) exited \(code=78\)" \
    "native sender success status"

# ---- UxTheme (w32a11_theme_v5.exe + w32a11_theme_v6.exe) ----
for version in v5 v6; do
    il_assert_grep "$LOG" "w32run: .*w32a11_theme_$version\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
        "$version PE mapped and all theme/GDI imports resolved"
done
il_assert_grep "$LOG" "A11-THEME-V5-OK" "v5 rejects theming and keeps GDI fallback pixel"
il_assert_grep "$LOG" "A11-THEME-V6-OK" "v6 pixel changes, known part, size and zero transition"
il_assert_grep "$LOG" "A11-ANIMATION-OK" \
    "zero-duration frame copied pixels; timed request refused; stop invalidated buffer"

# ---- every PE fixture exited with the fixture success status 78 ----
il_assert_count "$LOG" "\[shell\] '/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" 5 \
    "all five w32run PE fixtures exited 78 (core, probe, drag, theme v5, theme v6)"

# ---- global invariants ----
il_assert_no_grep "$LOG" "A11-[A-Z]+-FAIL|W32A11-CORE-SUBSET-FAIL|A11-THEME-MATRIX-FAIL" "no fixture failure"
il_assert_no_grep "$LOG" "unresolved import" "no unresolved imports"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" "no guest fault"
il_assert_grep "$LOG" "Goodbye!" "shell survived and exited"
il_summary
