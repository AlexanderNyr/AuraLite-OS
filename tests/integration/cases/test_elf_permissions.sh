#!/usr/bin/env bash
# test_elf_permissions.sh — N4 strict user ELF permissions and NX gate.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N4 Strict ELF Permissions & NX"

LOG="$IL_LOGDIR/elf_permissions.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "write /tmp/elfperm.mode write-text"
il_send "run /elfperm"
il_send_delay 3
il_send "write /tmp/elfperm.mode exec-data"
il_send "run /elfperm"
il_send_delay 3
il_send "echo ELFPERM: shell alive after faults"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_grep "$LOG" "ELFPERM: write-text begin" "write-to-text probe started"
il_assert_grep "$LOG" "ELFPERM: exec-data begin"   "execute-from-data probe started"
il_assert_count "$LOG" "\[EXCEPTION\] Page Fault .* from USER mode" 2 "two user-mode permission faults"
il_assert_grep "$LOG" "ELFPERM:? *shell *alive *after *faults" "shell survived both user faults"
il_assert_no_grep "$LOG" "ELFPERM FAIL" "no permission bypass marker"
il_assert_no_grep "$LOG" "from KERNEL mode" "no kernel-mode exception"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"

il_summary
