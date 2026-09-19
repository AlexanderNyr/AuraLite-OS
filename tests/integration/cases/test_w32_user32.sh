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
# W32A-7 restructure: the hostile leg runs in its own boot.  Since W32A-4,
# an unhandled exception in a process that owns windows raises the
# MessageBoxA alert, and ag_alert() blocks on the GUI event queue until
# Enter/Esc/OK -- correct interactive UX, but a serial-only boot could
# never dismiss it, so the old single-boot script hung on this leg (it
# had never been machine-run: QEMU was absent when W32-5 shipped).  The
# hostile boot gives QEMU a monitor socket and "presses Enter" with the
# monitor's sendkey -- the same key the A-4 dialog gate sends through
# VNC -- then asserts the process died, the compositor reaped its
# window, and the shell survived.
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
il_send_delay 8
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

# --- 3 + 4. the hostile WNDPROC (own boot, monitor-dismissed alert) --------
# u32bad.exe is u32test.exe with one displacement rewritten, so its WNDPROC
# points outside the image.  It must die on its own and take nothing with it.
LOG2="$IL_LOGDIR/w32_user32_hostile.log"
IL_LAST_LOG="$LOG2"
: > "$LOG2"

MON_PORT=$((44440 + RANDOM % 500))
INPUT_FIFO="/tmp/w32_user32_fifo.$$.pipe"
rm -f "$INPUT_FIFO"
mkfifo "$INPUT_FIFO"
sleep 400 >"$INPUT_FIFO" 2>/dev/null &
SLEEP_PID=$!

( "$IL_QEMU" \
    -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" -m 512M -smp 2 \
    -display none -serial stdio -no-reboot -cpu "$IL_CPU" -boot order=c \
    -netdev user,id=net0 -device "$IL_NIC",netdev=net0 \
    -fw_cfg "name=opt/auralite.selftest,string=fast" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    <"$INPUT_FIFO" >"$LOG2" 2>&1 ) &
QEMU_PID=$!

mon_send() {  # one QEMU monitor command over the TCP control socket
    { printf '%s\n' "$1"; sleep 0.2; } >/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null || true
}

cleanup_hostile() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    kill "$SLEEP_PID" 2>/dev/null || true
    rm -f "$INPUT_FIFO"
}

for _ in $(seq 1 90); do
    grep -aq "auralite#" "$LOG2" 2>/dev/null && break
    sleep 1
done
printf 'run /apps/w32run /tests/u32bad.exe\n' >"$INPUT_FIFO"
for _ in $(seq 1 60); do
    grep -aq "W32-SEH-UNHANDLED" "$LOG2" 2>/dev/null && break
    sleep 1
done
il_assert_grep "$LOG2" "w32run: .*u32bad\.exe mapped at 0x[0-9a-f]+, 21 import\(s\) bound" \
    "hostile PE mapped and imports resolved"
il_assert_grep "$LOG2" "W32-SEH-UNHANDLED" \
    "the bad WNDPROC faulted and reached the unhandled path"

# Dismiss the alert like a user: Enter first, Esc as backup (ag_alert
# breaks on either).  sendkey reaches the guest's PS/2 keyboard, which
# is the queue the alert polls.
for _ in $(seq 1 12); do
    grep -aq "'/apps/w32run' (tid [0-9]*) exited (code=" "$LOG2" && break
    mon_send "sendkey ret"
    sleep 2
    grep -aq "'/apps/w32run' (tid [0-9]*) exited (code=" "$LOG2" && break
    mon_send "sendkey esc"
    sleep 2
done
printf 'exit\n' >"$INPUT_FIFO"
for _ in $(seq 1 30); do
    grep -aq "Goodbye!" "$LOG2" 2>/dev/null && break
    sleep 1
done
cleanup_hostile

il_assert_grep "$LOG2" "'/apps/w32run' \(tid [0-9]*\) exited \(code=-?[1-9][0-9]*\)" \
    "the hostile PE died on its own after the alert was dismissed"
il_assert_grep "$LOG2" "\[gui\] cleaned [0-9]+ window\(s\) for pid" \
    "compositor reaped the window of the killed PE process"
il_assert_no_grep "$LOG2" "UNHANDLED EXCEPTION.*KERNEL|kernel panic" \
    "hostile WNDPROC did not fault the kernel"

# --- the system survived ---------------------------------------------------
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived the first boot and exited cleanly"
il_assert_grep "$LOG2" "Goodbye!" \
    "shell survived the hostile boot and exited cleanly"

il_summary
