#!/usr/bin/env bash
# test_w32a12_winsock.sh — W32APP_PLAN.md phase W32A-12 gate.
#
# One mingw-w64 fixture (w32a12_winsock.exe) linked -lws2_32, so its
# WS2_32 imports are genuine NAME imports resolved through w32_bind.c.
# It runs the WinSock surface REAL over the native socket stack: version
# negotiation, byte order, socket/bind/getsockname/listen/accept (served
# by the kernel's tcp test fallback, peer 10.0.2.2:54321), getpeername,
# recv (the canned "GET /" request) / send, select with the Windows
# fd_set translation, ioctlsocket, the observed setsockopt set (and a
# refuse-by-number), shutdown/closesocket, getaddrinfo (Windows ADDRINFOA
# layout) + inet_ntop/inet_pton, gethostname, per-thread WSAGetLastError,
# and the events family refusing by name.  It prints W32A12-WINSOCK-OK and
# exits 78 on a clean run, or FAIL-<mark> + W32A12-WINSOCK-FAIL / exit 1.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-12 integration: WS2_32 WinSock over the native socket stack"

LOG="$IL_LOGDIR/w32a12_winsock.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The fixture is only in the image when the cross-compiler was installed
# at build time.  Skip loudly rather than assert on a file never built.
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32a12_winsock; then
    echo "  SKIP: W32A-12 fixture not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/w32a12_winsock.exe"
il_send_delay 18
il_send "exit"

il_run_qemu "$LOG" 90

# --- the fixture passed every in-guest check --------------------------------
il_assert_grep "$LOG" "W32A12-WINSOCK-OK" \
    "the WinSock fixture passed in-guest"

# --- and exited 78 through ExitProcess --------------------------------------
il_assert_grep "$LOG" "'/tests/w32a12_winsock\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "w32a12_winsock.exe exited 78"

# --- no failed check, no fault ----------------------------------------------
il_assert_no_grep "$LOG" "W32A12-WINSOCK-FAIL" \
    "no fixture check failed"
il_assert_no_grep "$LOG" "FAIL-" \
    "no individual WinSock assertion failed"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic|PANIC" \
    "no fault during the run"

il_summary
