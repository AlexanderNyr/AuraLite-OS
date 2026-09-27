#!/usr/bin/env bash
# W32A-11: same Win64 APIs on v5 and v6 manifest peers, comparing actual
# GDI pixels; v6 zero-duration buffered animation updates a real HDC.
# Timed animations are explicitly refused, not claimed as implemented.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "W32A-11 guest v5/v6 UxTheme pixel/animation matrix"
LOG="$IL_LOGDIR/w32a11_theme.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send "run /apps/w32run /tests/w32a11_theme_v5.exe"
il_send_wait "A11-THEME-V5-OK" 45
il_send "run /apps/w32run /tests/w32a11_theme_v6.exe"
il_send_wait "A11-ANIMATION-OK" 45
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG" 80

for version in v5 v6; do
    il_assert_grep "$LOG" "w32run: .*w32a11_theme_$version\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\(s\) bound" \
        "$version PE mapped and all theme/GDI imports resolved"
done
il_assert_grep "$LOG" "A11-THEME-V5-OK" "v5 rejects theming and keeps GDI fallback pixel"
il_assert_grep "$LOG" "A11-THEME-V6-OK" "v6 pixel changes, known part, size and zero transition"
il_assert_grep "$LOG" "A11-ANIMATION-OK" \
    "zero-duration frame copied pixels; timed request refused; stop invalidated buffer"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$(grep -cE "^\[shell\] '/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" "$LOG")" -eq 2 ]; then
    il_pass "both independent PE runs exited with status 78"
else
    il_fail "v5 and v6 PEs did not both exit 78"
fi
il_assert_no_grep "$LOG" "A11-THEME-MATRIX-FAIL|unresolved import" \
    "no failed guest assertion or import"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no guest fault"
il_assert_grep "$LOG" "Goodbye!" "shell survived"
il_summary
