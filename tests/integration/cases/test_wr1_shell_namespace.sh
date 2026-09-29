#!/usr/bin/env bash
# test_wr1_shell_namespace.sh — W32RUN_PLAN.md phase WR-1 gate (SHELL32
# namespace: IShellFolder / IEnumIDList / PIDL, closing plan §7),
# CI-automatable half.
#
# One mingw-w64 fixture (wr1_shellns.exe) that drives the exact IShellFolder /
# IEnumIDList / PIDL surface the pinned 7zFM.exe imports (SHGetDesktopFolder +
# the PIDL info calls) through the documented COM vtable -- the same call path
# a real consumer takes.  It walks the namespace the implementation builds,
# Desktop -> My Computer -> C: -> the filesystem (CFSFolder over the REAL
# FindFirstFileW), and asserts each step: a live desktop folder with working
# QueryInterface, EnumObjects yielding a browsable My Computer, BindToObject
# descending to the drive and then the VFS root, GetDisplayNameOf /
# GetAttributesOf on real entries, ParseDisplayName round-tripping an existing
# path and failing clean on a missing one, and CreateViewObject (a GUI object,
# a documented non-goal on this layer) returning E_NOTIMPL by name.
#
# It prints WR1-SHELLNS-OK and exits 78 on a clean run, or FAIL-<mark> +
# WR1-SHELLNS-FAIL / exit 1.
#
# The host twin (tests/unit/test_shell_ns.c, ASan/UBSan) pins the same object
# graph against a scripted VFS and is the always-on gate; this integration case
# proves a compiler-emitted PE consumer drives the SAME ABI through w32_bind.c
# in the booted OS.  It runs only when the cross-compiler was present at build
# time, skipping loudly otherwise.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "WR-1 integration: the SHELL32 namespace app gate (fixture twin)"

LOG="$IL_LOGDIR/wr1_shellns.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The fixture is only in the image when the cross-compiler was installed at
# build time.  Skip loudly rather than assert on a file never built.
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q wr1_shellns; then
    echo "  SKIP: WR-1 fixture not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/wr1_shellns.exe"
il_send_delay 12
il_send "exit"

il_run_qemu "$LOG" 90

# --- the fixture passed every in-guest check --------------------------------
il_assert_grep "$LOG" "WR1-SHELLNS-OK" \
    "the SHELL32 namespace fixture passed in-guest"

# --- and exited 78 ----------------------------------------------------------
il_assert_grep "$LOG" "'/tests/wr1_shellns\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "wr1_shellns.exe exited 78"

# --- no failed check, no fault ----------------------------------------------
il_assert_no_grep "$LOG" "WR1-SHELLNS-FAIL" \
    "no fixture check failed"
il_assert_no_grep "$LOG" "FAIL-" \
    "no individual namespace assertion failed"
il_assert_no_grep "$LOG" "TODO .* needs W32|E_NOTIMPL.*section 7" \
    "no import fell through to a loud TODO stub (the namespace is REAL now)"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic|PANIC" \
    "no fault during the run"

il_summary
