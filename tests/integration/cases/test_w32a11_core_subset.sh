#!/usr/bin/env bash
# Incremental W32A-11 guest gate: OLE init/BSTR/VARIANT ordinals, IMM32
# fail-clean, clipboard-format IDs and pixel-backed BUTTON theme rendering.
# This is NOT the full W32A-11 gate: no observed CLSID/IID table, real
# compositor-delivered WM_DROPFILES, or window-to-window OLE drag is covered.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-11 incremental guest subset"
LOG="$IL_LOGDIR/w32a11_core_subset.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/w32run /tests/w32a11_core.exe"
il_send_delay 10
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG" 180

il_assert_grep "$LOG" "w32run: .*w32a11_core\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "fixture PE mapped and imports resolved"
for m in COM VARIANT ORDINALS IMM CLIPFMT THEME; do
    il_assert_grep "$LOG" "A11-$m-OK" "guest $m effects checked"
done
il_assert_grep "$LOG" "W32A11-CORE-SUBSET-OK" "all guest subset sections finished"
il_assert_grep "$LOG" "'/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" \
    "fixture exited with its success code (78)"
il_assert_no_grep "$LOG" "W32A11-CORE-SUBSET-FAIL" "no failed fixture assertion"
il_assert_no_grep "$LOG" "unresolved import" "no unresolved imports"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" "no guest fault"
il_assert_grep "$LOG" "Goodbye!" "shell survived and exited"
il_summary
