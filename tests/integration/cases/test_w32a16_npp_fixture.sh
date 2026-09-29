#!/usr/bin/env bash
# test_w32a16_npp_fixture.sh — W32APP_PLAN.md phase W32A-16 gate (App gate III:
# Notepad++), CI-automatable half.
#
# One mingw-w64 fixture (w32a16_npp.exe) that imports the exact KERNEL32 /
# USER32 / GDI32 / COMCTL32 / SHELL32 / SHLWAPI / WININET / SENSAPI / WINTRUST
# surface the real notepad++.exe (8.8.9) drives -- by NAME, resolved through
# w32_bind.c -- and walks Notepad++'s single-process personality paths,
# asserting each: the SysTabControl32 open-files tab strip (insert three pages,
# count, switch, read-back, close one -> two), the SHLWAPI Path* filename family
# and the Color* dark-mode arithmetic, the TRACED open path
# (SHCreateItemFromParsingName -> IShellItem -> GetDisplayName; the plan's §2.3
# surprise that notepad++.exe has no GetOpenFileName import), a save+reopen and
# save-as over the VFS, the run-time plugin DLL chain + msvcrt heap, the
# minimise-to-tray Shell_NotifyIconW path, and the offline updater
# (InternetCrackUrlW parse + WinVerifyTrust TRUST_E_NOSIGNATURE + SENSAPI probes
# answering a clean boolean without a fault).
#
# It prints W32A16-NPP-OK and exits 78 on a clean run, or FAIL-<mark> +
# W32A16-NPP-FAIL / exit 1.
#
# The full Notepad++ receipt (Scintilla text render with syntax colours, tab
# switching pixels, the Find/Replace dialog UI, plugin menu entries, drag-drop
# add from the compositor, minimise-to-tray as a live notification) is human-run
# and documented as such in docs/w32app_receipts.md under #notepad++ -- it needs
# a framebuffer and a pixel oracle and is out of this gate's scope by design.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-16 integration: the Notepad++ app gate (fixture twin)"

LOG="$IL_LOGDIR/w32a16_npp.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The fixture is only in the image when the cross-compiler was installed at
# build time.  Skip loudly rather than assert on a file never built.
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32a16_npp; then
    echo "  SKIP: W32A-16 fixture not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/w32a16_npp.exe"
il_send_delay 18
il_send "exit"

il_run_qemu "$LOG" 90

# --- the fixture passed every in-guest check --------------------------------
il_assert_grep "$LOG" "W32A16-NPP-OK" \
    "the Notepad++ app-gate fixture passed in-guest"

# --- and exited 78 ----------------------------------------------------------
il_assert_grep "$LOG" "'/tests/w32a16_npp\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "w32a16_npp.exe exited 78"

# --- no failed check, no fault ----------------------------------------------
il_assert_no_grep "$LOG" "W32A16-NPP-FAIL" \
    "no fixture check failed"
il_assert_no_grep "$LOG" "FAIL-" \
    "no individual Notepad++ assertion failed"
il_assert_no_grep "$LOG" "TODO .* needs W32A" \
    "no import fell through to a loud TODO stub"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic|PANIC" \
    "no fault during the run"

il_summary
