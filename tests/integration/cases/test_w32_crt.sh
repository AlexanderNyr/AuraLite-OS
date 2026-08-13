#!/usr/bin/env bash
# test_w32_crt.sh — WIN32_PLAN.md phase W32-6 gate.
#
# The plan asks for four things:
#
#   1. a global constructor runs before main (.CRT$XC*), and TLS callbacks
#      run before that;
#   2. command-line parsing matches documented Win32 quoting -- that lives in
#      the host unit test (tests/unit/test_w32_argv.c, 139 assertions against
#      Microsoft's published examples), because it is pure string handling
#      and belongs where sanitizers can see it;
#   3. a divide by zero inside __try reaches __except, and the same fault
#      OUTSIDE __try terminates the process cleanly with no kernel fault;
#   4. the honest note about not being table-driven unwinding, recorded in
#      docs/win32.md and WIN32_PLAN.md.
#
# This case covers 1 and 3, plus the hostile TLS fixture.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "W32-6 CRT startup, TLS and SEH"

LOG="$IL_LOGDIR/w32_crt.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/w32run /tests/crttest.exe"
il_send_delay 5
il_send "run /apps/w32run /tests/crtbad.exe"
il_send_delay 5
il_send "run /apps/sehtest"
il_send_delay 6
il_send "run /apps/sehtest crash"
il_send_delay 5
il_send "run /apps/sehtest filter"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 120

# --- 1. startup order ------------------------------------------------------
il_assert_grep "$LOG" "CRT-TLS-CALLBACK" \
    "TLS callback ran out of the PE's TLS directory"
il_assert_grep "$LOG" "w32run: ran 1 TLS callback\(s\)" \
    "loader reported the callback it ran"
il_assert_grep "$LOG" "CRT-STATIC-INIT" \
    ".CRT\$XCU static initialiser ran"
il_assert_grep "$LOG" "w32run: ran 1 static initialiser\(s\)" \
    "loader found the initialiser table by section"
# The fixture checks the ordering itself and only prints this if the TLS
# callback ran BEFORE the constructor, and both before the entry point.
il_assert_grep "$LOG" "CRT-ORDER-OK" \
    "TLS callbacks ran before static initialisers, both before entry"
il_assert_grep "$LOG" "W32-CRT-OK" \
    "CRT fixture reported success"
il_assert_grep "$LOG" "'/apps/w32run' \(tid [0-9]+\) exited \(code=77\)" \
    "CRT fixture exited with its success status"

# --- the hostile TLS callback ---------------------------------------------
# crtbad.exe has its first TLS callback pointing outside the image.  Nothing
# should be called: the loader refuses before transferring control, which is
# stronger than catching a fault afterwards.
il_assert_grep "$LOG" "w32run: refusing malformed TLS directory" \
    "TLS callback outside the image is refused, not called"

# --- 3. __try / __except ---------------------------------------------------
il_assert_grep "$LOG" "SEH-DIV0-CAUGHT" \
    "divide by zero inside __try reached __except"
# The mask test: a second fault must also be caught.  With plain longjmp
# instead of siglongjmp, SIGFPE would still be blocked and this would kill
# the process.
il_assert_grep "$LOG" "SEH-SECOND-CAUGHT" \
    "a SECOND fault is caught too (signal mask restored on the way out)"
il_assert_grep "$LOG" "SEH-AV-CAUGHT" \
    "an access violation is distinguished from a divide by zero"
il_assert_grep "$LOG" "SEH-INNER-CAUGHT" \
    "the innermost __try handles the fault, not an outer one"
il_assert_grep "$LOG" "SEH-BALANCED" \
    "non-faulting __try blocks leave the nesting stack balanced"
il_assert_grep "$LOG" "W32-SEH-OK" \
    "SEH fixture reported success"
il_assert_grep "$LOG" "'/apps/sehtest' \(tid [0-9]+\) exited \(code=44\)" \
    "SEH fixture exited with its success status"

# --- the same fault OUTSIDE __try ------------------------------------------
# It must terminate the process, with the right signal, and must NOT be
# swallowed by the shim.
il_assert_grep "$LOG" "SEH-UNGUARDED-FAULT" \
    "unguarded fault path was reached"
il_assert_no_grep "$LOG" "SEH-SURVIVED-UNGUARDED" \
    "an unguarded fault is not swallowed"
il_assert_grep "$LOG" "\[signal\] terminate pid=[0-9]+ by signal 8" \
    "unguarded divide by zero terminates the process with SIGFPE"
il_assert_grep "$LOG" "'/apps/sehtest' \(tid [0-9]+\) exited \(code=136\)" \
    "and with the conventional 128+SIGFPE status"

# --- SetUnhandledExceptionFilter ------------------------------------------
il_assert_grep "$LOG" "SEH-FILTER-CALLED" \
    "the unhandled exception filter got its last chance"
il_assert_no_grep "$LOG" "SEH-FILTER-NOT-CALLED" \
    "execution did not continue past the faulting instruction"

# --- nothing took the kernel with it ---------------------------------------
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic" \
    "no kernel fault from any of the above"
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived all five runs and exited cleanly"

il_summary
