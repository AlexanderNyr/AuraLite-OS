#!/usr/bin/env bash
# test_w32a15_7zip_fixture.sh — W32APP_PLAN.md phase W32A-15 gate (App gate II:
# 7-Zip File Manager), CI-automatable half.
#
# One mingw-w64 fixture (w32a15_7zip.exe) that imports the exact KERNEL32 /
# USER32 / GDI32 / COMCTL32 / SHELL32 / ADVAPI32 surface the real 7zFM.exe
# drives -- by NAME, resolved through w32_bind.c -- and carries an embedded
# RT_BITMAP (a 4bpp packed DIB, 7-Zip's toolbar-strip format) so LoadBitmapW
# walks the real PE resource path.  It walks 7-Zip's personality paths and
# asserts each: the W32A-15 LoadBitmapW REAL slice (RT_BITMAP -> device HBITMAP,
# dimensions, 4bpp->32bpp palette expansion, missing-id fail-clean), the
# run-time DLL-chain + msvcrt malloc/free/realloc heap, a report SysListView32
# with three rows read back, the delay-loaded MPR network path failing CLEAN,
# the Reg*W Options round-trip, and SHFileOperationW FO_DELETE over the VFS.
#
# It prints W32A15-7ZIP-OK and exits 78 on a clean run, or FAIL-<mark> +
# W32A15-7ZIP-FAIL / exit 1.
#
# The full 7-Zip receipt (GUI toolbar/listview screenshots, byte-exact
# extract-all of the pinned 7zFM.exe + 7z.dll against harness originals, the
# Options property sheet and ACL-approximation observations) is human-run and
# documented as such in docs/w32app_receipts.md under #7zip -- it is not
# headlessly automatable and is out of this gate's scope by design.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-15 integration: the 7-Zip File Manager app gate (fixture twin)"

LOG="$IL_LOGDIR/w32a15_7zip.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The fixture is only in the image when the cross-compiler was installed at
# build time.  Skip loudly rather than assert on a file never built.
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32a15_7zip; then
    echo "  SKIP: W32A-15 fixture not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/w32a15_7zip.exe"
il_send_delay 18
il_send "exit"

il_run_qemu "$LOG" 90

# --- the fixture passed every in-guest check --------------------------------
il_assert_grep "$LOG" "W32A15-7ZIP-OK" \
    "the 7-Zip app-gate fixture passed in-guest"

# --- and exited 78 ----------------------------------------------------------
il_assert_grep "$LOG" "'/tests/w32a15_7zip\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "w32a15_7zip.exe exited 78"

# --- no failed check, no fault ----------------------------------------------
il_assert_no_grep "$LOG" "W32A15-7ZIP-FAIL" \
    "no fixture check failed"
il_assert_no_grep "$LOG" "FAIL-" \
    "no individual 7-Zip assertion failed"
il_assert_no_grep "$LOG" "TODO .* needs W32A" \
    "no import fell through to a loud TODO stub (LoadBitmapW is REAL now)"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic|PANIC" \
    "no fault during the run"

il_summary
