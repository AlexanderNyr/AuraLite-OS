#!/usr/bin/env bash
# test_boot_to_shell.sh — boot the kernel and reach the Ring 3 init shell.
#
# Verifies the entire phase-0..phase-11 chain reaches the interactive prompt.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "boot-to-shell"

LOG="$IL_LOGDIR/boot_to_shell.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# No input — we just want to observe the boot banner reaching the shell.
il_send_delay 6
il_send "exit"
il_run_qemu "$LOG" 12

# Core boot milestones.
il_assert_grep "$LOG" "Hello from AuraLite OS kernel!"                    "Limine → kmain banner"
il_assert_grep "$LOG" "IDT installed: 256 gates"                          "IDT initialised"
il_assert_grep "$LOG" "PIC remapped"                                      "PIC remap"
il_assert_grep "$LOG" "TSS loaded"                                        "TSS"
il_assert_grep "$LOG" "SYSCALL/SYSRET configured"                         "MSR setup"
il_assert_grep "$LOG" "HHDM offset: 0xffff800000000000"                   "HHDM set up"
il_assert_grep "$LOG" "\[pmm\] PASS:"                                     "PMM self-test"
il_assert_grep "$LOG" "\[vmm\] PASS:"                                     "Paging self-test"
il_assert_grep "$LOG" "\[heap\] PASS:"                                    "Heap self-test"
il_assert_grep "$LOG" "\[timer\] PASS:"                                   "PIT timer accuracy"
il_assert_grep "$LOG" "\[sched\] PASS:"                                   "Scheduler interleave"
il_assert_grep "$LOG" "\[vfs\] PASS:"                                     "VFS layer functional"
il_assert_grep "$LOG" "\[boot\] starting init shell \(Ring 3\)"        "Ring 3 init reached"
il_assert_grep "$LOG" "auralite#"                                        "Interactive shell prompt"

# Triple-fault / panic markers we never want to see.
il_assert_no_grep "$LOG" "PANIC"                                          "no kernel panic"
il_assert_no_grep "$LOG" "TRIPLE FAULT"                                   "no triple-fault"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                            "no unhandled exception"

il_summary
