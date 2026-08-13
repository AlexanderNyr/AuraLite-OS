#!/usr/bin/env bash
# test_w32_user32.sh — WIN32_PLAN.md phase W32-5 gate.
#
# The plan asks for four things; all four are checked here:
#
#   1. a .exe creates a window, paints, and closes cleanly;
#   2. the WNDPROC is entered with the right hwnd/msg -- the CALLBACK
#      direction of the ABI, which W32-4's test did not cover;
#   3. a window owned by a killed PE process is reaped (gui_cleanup_process);
#   4. hostile: a WNDPROC pointing outside the image is survivable, with no
#      kernel fault.
#
# The compositor draws to a framebuffer nobody is watching in CI, so the guest
# program reports what it observed over serial and encodes overall success in
# its exit status (66), exactly as test_gui already does for native apps.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "W32-5 USER32 + GDI32"

LOG="$IL_LOGDIR/w32_user32.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/w32run /tests/u32test.exe"
il_send_delay 6
il_send "run /apps/w32run /tests/u32bad.exe"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 90

# --- 1. binding and window creation ---------------------------------------
il_assert_grep "$LOG" "w32run: .*u32test\.exe mapped at 0x[0-9a-f]+, 21 import\(s\) bound" \
    "all 21 KERNEL32/USER32/GDI32 imports resolved"
il_assert_grep "$LOG" "WINDOW-CREATED" \
    "CreateWindowExA produced a window through the compositor"

# --- 2. the WNDPROC callback ----------------------------------------------
# WM_CREATE must arrive synchronously, from inside CreateWindowExA, before it
# returns -- a program that allocates state there depends on that ordering.
il_assert_grep "$LOG" "WNDPROC-WM_CREATE" \
    "WM_CREATE delivered synchronously during CreateWindowExA"
il_assert_grep "$LOG" "WNDPROC-WM_PAINT" \
    "WM_PAINT dispatched into the PE image's WNDPROC"
il_assert_grep "$LOG" "WNDPROC-ARGS-OK" \
    "WNDPROC received a valid hwnd across the ms_abi callback boundary"
il_assert_grep "$LOG" "WNDPROC-WM_DESTROY" \
    "DestroyWindow drove WM_DESTROY into the WNDPROC"

# The in-guest program only reaches this after every one of its own checks
# passed; 66 is that success path and 1 is its failure path.
il_assert_grep "$LOG" "W32-USER32-OK" \
    "guest reported the full sequence succeeded"
il_assert_grep "$LOG" "'/apps/w32run' \(tid [0-9]+\) exited \(code=66\)" \
    "windowed PE exited with its success status"

# --- 3 + 4. the hostile WNDPROC -------------------------------------------
# u32bad.exe is u32test.exe with one displacement rewritten, so its WNDPROC
# points outside the image.  It must die on its own and take nothing with it.
il_assert_grep "$LOG" "\[gui\] cleaned [0-9]+ window\(s\) for pid" \
    "compositor reaped the window of the killed PE process"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic" \
    "hostile WNDPROC did not fault the kernel"

# --- the system survived ---------------------------------------------------
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived both runs and exited cleanly"

il_summary
