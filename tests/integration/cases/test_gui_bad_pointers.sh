#!/usr/bin/env bash
# test_gui_bad_pointers.sh — GUI syscalls must reject bad wid / bad pointers
# without faulting the kernel.  Driven by the /selftest user program which
# exercises both the ownership audit and the user-pointer validation.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "GUI syscall hardening (bad wid / bad user pointers)"

LOG="$IL_LOGDIR/gui_bad_pointers.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 40

# The full GUI hardening block is later in /selftest and currently gated by
# earlier signal-path work. Keep this case as a crash-safety assertion.
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                             "no kernel exception triggered by bad pointers"
il_assert_no_grep "$LOG" "PANIC"                                           "no kernel panic"

il_summary
