#!/usr/bin/env bash
# Incremental W32A-11 guest gate: OLE init/BSTR/VARIANT ordinals, IMM32,
# clipboard-format IDs and BUTTON theme pixels. A SECOND synthetic PE checks
# that TODO class/ProgID probes log precisely and fail cleanly. Neither PE
# supplies a pinned-app CLSID/IID observation or a class activation table.
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
il_send_wait "W32A11-CORE-SUBSET-OK" 75
il_send "run /apps/w32run /tests/w32a11_probe.exe"
il_send_wait "A11-PROBE-FIXTURE-OK" 75
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG" 180

il_assert_grep "$LOG" "w32run: .*w32a11_core\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "fixture PE mapped and imports resolved"
for m in COM VARIANT ORDINALS IMM CLIPFMT THEME; do
    il_assert_grep "$LOG" "A11-$m-OK" "guest $m effects checked"
done
il_assert_grep "$LOG" "W32A11-CORE-SUBSET-OK" "all guest subset sections finished"
il_assert_grep "$LOG" "w32run: .*w32a11_probe\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
    "synthetic class-probe PE mapped through the real binder"
il_assert_grep_fixed "$LOG" "w32a11-clsid-probe: CLSID=12345678-9abc-def0-1122-334455667788 IID=00000000-0000-0000-c000-000000000046 CLSCTX=1" \
    "full GUID pair logged in canonical order"
il_assert_grep_fixed "$LOG" "w32a11-clsid-probe: CLSID=12345678-9abc-def0-1122-334455667788 IID=00000000-0000-0000-0000-000000000000 CLSCTX=4" \
    "second pair logged instead of suppressed by TODO-once"
il_assert_grep_fixed "$LOG" "w32a11-progid-probe: UTF16=NPP.\\u00e9\\ud840\\udc00\\u005c\\u000a\\u003d\\u0020 truncated=0" \
    "Unicode ProgID logged losslessly on a single line"
il_assert_grep "$LOG" "A11-PROBE-FIXTURE-OK" \
    "unknown classes refused and output parameters cleared"
il_assert_count "$LOG" "\[shell\] '/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" 2 \
    "both independent PE fixtures exited 78"
il_assert_no_grep "$LOG" "W32A11-CORE-SUBSET-FAIL|A11-PROBE-FIXTURE-FAIL" "no fixture failure"
il_assert_no_grep "$LOG" "unresolved import" "no unresolved imports"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" "no guest fault"
il_assert_grep "$LOG" "Goodbye!" "shell survived and exited"
il_summary
