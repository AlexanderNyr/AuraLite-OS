#!/usr/bin/env bash
# test_lx_lua.sh -- LX_COMPAT_PLAN.md L5: the flagship rung of the Linux
# application ladder — an UNMODIFIED interpreter runs a real script.
#
# The staged binary is stock lua 5.4 (upstream `make linux`: a PIE linked
# against glibc + libm).  lxrun execs it with the staged script, whose
# every section is an assert that fails by name:
#   arithmetic (operators + math lib), strings (upper/sub/format/length),
#   table (10 000 entries, sorted, reduced to an exact host-computed
#   sum), os.time/os.date (the time path), and io.lines over a REAL
#   /linux file (the staged motd).  The receipt is the script's final
#   print("LX5-LUA-OK").
#
# Assertion mechanics (inherited from test_lx_dynamic.sh):
#   - the receipt anchors at ^ only (the serial console CR-terminates
#     lines and the host grep does not parse \r);
#   - exit-status propagation through the kernel's
#     "[thread] '/bin/lxrun' (tid N) exited (code=0)" receipt;
#   - a lua assert failure would print "lua: ...: assertion failed!" to
#     stderr and exit non-zero, so the no-grep on the failure line and
#     the code=0 receipt together prove every section passed.
#
# The placeholder: a host that could not build lua stages a zero-byte
# file and this case skips exactly like test_lx_busybox.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "LX L5: unmodified lua runs its script"

LOG="$IL_LOGDIR/lx_lua.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "lxrun /linux/tests/lua /linux/tests/lua_script.lua"
il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 120

il_assert_grep    "$LOG" "^LX5-LUA-OK" \
                       "lua printed its receipt (the whole script passed)"
il_assert_grep    "$LOG" "/bin/lxrun.*exited .code=0." \
                       "lua exited 0 (every script assert held)"
il_assert_no_grep "$LOG" "assertion failed" "no lua assert fired"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"

il_summary
