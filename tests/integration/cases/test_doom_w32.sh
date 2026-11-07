#!/usr/bin/env bash
# test_doom_w32.sh — DOOM_PLAN.md D7, closed by RESIDUE3 C8 box 4.
#
# D7 is not "make DOOM run on the Win32 personality".  It is: take a stranger's
# Win32 program -- upstream's own doomgeneric_win.c, not AuraLite's platform
# layer -- build it with mingw-w64, push it through `w32run`, and find out what
# the personality is missing.  DOOM_PLAN §3 says the gaps are the point.
#
# What the run measures today, all of it asserted below:
#
#   * the C8 streaming loader maps and relocates it: 1661 DIR64 relocations,
#     "no fixed cap" -- the Audacity-scale loader on a 948 KiB binary;
#   * the binder binds every import it can and refuses the image naming what
#     it cannot: 52 symbols across 4 modules;
#   * the refusal is a to-do list, not a count (`w32_gap_at()`), because a
#     number is a feeling and a symbol is a work item.
#
# The gate SKIPS LOUDLY when the Win32 build is absent: producing it fetches
# GPL-2.0 sources into build/ (never committed, see the Makefile) and needs a
# network.  Run `make doom-win` first.
#
# The 52 are pinned exactly.  If the personality grows a symbol the number
# drops and this gate fails on purpose: that is the ratchet telling you to
# move the line, not a test gone flaky.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "DOOM D7: the Win32 build through w32run"

LOG="$IL_LOGDIR/doom_w32.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q doom_w32.exe; then
    echo "${C_YELLOW}SKIP: no Win32 DOOM build in the image${C_RESET}"
    echo "      D7 compiles upstream's doomgeneric_win.c with mingw-w64."
    echo "      That fetches GPL-2.0 sources into build/ (never committed)."
    echo "      Build it with:  make doom-win"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/doom_w32.exe"
il_send_delay 15
il_send "exit"

il_run_qemu "$LOG" 120

# --- it is a Win32 program going through the personality -------------------
il_assert_grep "$LOG" "/tests/doom_w32.exe imports Win32 DLLs; running via /apps/w32run" \
    "the shell routes the Win32 binary to the personality"
il_assert_grep "$LOG" "\\[w32\\] relocations: 1661 applied \\(DIR64\\), delta 0x3ffec0000000, no fixed cap" \
    "the C8 streaming loader relocated it (1661 DIR64, no fixed cap)"

# --- the gap, as a list ----------------------------------------------------
il_assert_grep "$LOG" "w32run: \\[w32\\] gap: 4 module\\(s\\), 52 symbol\\(s\\) missing" \
    "52 symbols missing across 4 modules (the D7 number)"
il_assert_grep "$LOG" "w32run: gap  GDI32.dll                2 symbol\\(s\\)" "GDI32: 2"
il_assert_grep "$LOG" "w32run: gap  KERNEL32.dll             1 symbol\\(s\\)" "KERNEL32: 1"
il_assert_grep "$LOG" "w32run: gap  msvcrt.dll               48 symbol\\(s\\)" "msvcrt: 48"
il_assert_grep "$LOG" "w32run: gap  USER32.dll               1 symbol\\(s\\)" "USER32: 1"

# The names are the deliverable: one line each, no truncation at 52.
il_assert_grep "$LOG" "w32run:   missing GDI32.dll!StretchDIBits" "GDI32!StretchDIBits named"
il_assert_grep "$LOG" "w32run:   missing GDI32.dll!SwapBuffers" "GDI32!SwapBuffers named"
il_assert_grep "$LOG" "w32run:   missing KERNEL32.dll!VirtualQuery" "KERNEL32!VirtualQuery named"
il_assert_grep "$LOG" "w32run:   missing USER32.dll!AdjustWindowRect" "USER32!AdjustWindowRect named"
il_assert_grep "$LOG" "w32run:   missing msvcrt.dll!fopen" "msvcrt!fopen named"
il_assert_grep "$LOG" "w32run:   missing msvcrt.dll!_vsnprintf" "msvcrt!_vsnprintf named"
il_assert_grep "$LOG" "w32run:   missing msvcrt.dll!_strtoi64" "msvcrt!_strtoi64 named"
il_assert_no_grep "$LOG" "more not listed" \
    "all 52 were named (the list cap did not truncate)"

# Exactly 52: `il_assert_count` is "at least", and a list that quietly grows a
# truncation marker would still pass "at least".
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$(grep -c 'w32run:   missing ' "$LOG")" -eq 52 ]; then
    il_pass "the refusal names exactly 52 symbols"
else
    il_fail "the refusal named $(grep -c 'w32run:   missing ' "$LOG") symbols, expected 52"
fi

# --- and it refuses, honestly ----------------------------------------------
il_assert_grep "$LOG" "'/tests/doom_w32.exe' \\(tid [0-9]+\\) exited \\(code=1\\)" \
    "w32run refused the image (exit 1) instead of running it half-bound"
il_assert_no_grep "$LOG" "Page Fault|W32-SEH-UNHANDLED|kernel panic" \
    "no fault: a refusal, not a crash"
il_assert_no_grep "$LOG" "IWAD" \
    "it never reached engine init -- the load-time refusal came first"

il_summary
