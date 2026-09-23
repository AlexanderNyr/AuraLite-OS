#!/usr/bin/env bash
# test_w32a10_furniture.sh — W32APP_PLAN.md phase W32A-10 gate.
#
# The furniture surface, end to end, through the documented paths:
#   shlwapi Path* + Color* + AssocQueryStringW
#   shell32 known folders (CSIDL), PIDL round-trip, SHCreateItem,
#           Shell_NotifyIcon, Drag trio, SHCreateDirectory
#   comdlg32 PrintDlgW + CommDlgExtendedError
#   version GetFileVersionInfo* + VerQueryValueW (via the embedded PE)
#   wininet InternetCrackUrlW, dbghelp ImageNtHeader,
#   dwmapi pair, sensapi probes, wintrust, crypt32
#
# Markers the fixture prints:
#   A10-PATH-OK, A10-COLOR-OK, A10-FOLDER-OK, A10-PIDL-OK, A10-COMDLG-OK,
#   A10-URL-OK, A10-IMAGE-OK, A10-DWM-OK, A10-SENSAPI-OK, A10-WINTRUST-OK,
#   A10-CRYPT-OK, W32A10-FURNITURE-OK
#
# Exit code 78 is the fixture's own success path; 79 any failed check.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-10 integration: shell furniture"

LOG="$IL_LOGDIR/w32a10_furniture.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/w32run /tests/w32a10_furniture.exe"
il_send_delay 8
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG" 180

il_assert_grep "$LOG" "w32run: /tests/w32a10_furniture.exe .* import\(s\) bound" \
    "the fixture bound"
il_assert_grep "$LOG" "A10-PATH-OK" "Path*"
il_assert_grep "$LOG" "A10-COLOR-OK" "Color*"
il_assert_grep "$LOG" "A10-FOLDER-OK" "known folders"
il_assert_grep "$LOG" "A10-PIDL-OK" "PIDL round-trip"
il_assert_grep "$LOG" "A10-COMDLG-OK" "comdlg32 PrintDlg"
il_assert_grep "$LOG" "A10-URL-OK" "InternetCrackUrlW"
il_assert_grep "$LOG" "A10-IMAGE-OK" "ImageNtHeader"
il_assert_grep "$LOG" "A10-DWM-OK" "Dwm"
il_assert_grep "$LOG" "A10-SENSAPI-OK" "SensApi"
il_assert_grep "$LOG" "A10-WINTRUST-OK" "WinVerifyTrust"
il_assert_grep "$LOG" "A10-CRYPT-OK" "Crypt32"
il_assert_grep "$LOG" "W32A10-FURNITURE-OK" "final marker"
il_assert_grep "$LOG" "'/apps/w32run' .* exited \(code=78\)" "exit 78"
il_assert_no_grep "$LOG" "FAIL" "no FAIL marker"

il_summary
