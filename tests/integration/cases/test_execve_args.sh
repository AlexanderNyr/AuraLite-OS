#!/usr/bin/env bash
# test_execve_args.sh — execve(path, argv, envp) end-to-end (Block C kernel).
#
# The kernel boot self-test (process_self_test) spawns /execve_child in a fresh
# address space; that program calls execve("/argv_echo", argv, envp). /argv_echo
# prints exactly what it received. We assert the kernel built the System V AMD64
# initial process stack correctly (argc, argv[], NULL, envp[], NULL, auxv) and
# that crt0/__libc_start_main decoded it. This runs at boot, before the shell,
# so there is no race on the per-thread SYSCALL save area.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "execve argv/envp marshalling (/execve_child -> /argv_echo)"

LOG="$IL_LOGDIR/execve_args.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# No input needed: the markers are emitted during kernel boot self-test.
il_send_delay 18

il_run_qemu "$LOG" 28

# The child announces the execve, then argv_echo dumps argc/argv/envp.
il_assert_grep_fixed "$LOG" "EXECVE_CHILD calling execve(/argv_echo, argv, envp)" "execve_child reached execve"
il_assert_no_grep_fixed "$LOG" "EXECVE_CHILD execve FAILED"                        "execve did not fail"

# argc and each argv entry (note the embedded space in argv[2]).
il_assert_grep_fixed "$LOG" "ARGV_ECHO argc=4"             "argc == 4"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[0]=/argv_echo" "argv[0] == /argv_echo"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[1]=alpha"      "argv[1] == alpha"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[2]=beta gamma" "argv[2] keeps embedded space"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[3]=42"         "argv[3] == 42"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv_terminated=1"  "argv[argc] is NULL"

# envp: must equal environ and carry the injected variables.
il_assert_grep_fixed "$LOG" "ARGV_ECHO envp_eq_environ=1"  "main envp == environ"
il_assert_grep_fixed "$LOG" "ARGV_ECHO env[0]=P10=on"      "env[0] == P10=on"
il_assert_grep_fixed "$LOG" "ARGV_ECHO env[1]=SHELL=/init" "env[1] == SHELL=/init"
il_assert_grep_fixed "$LOG" "ARGV_ECHO envc=2"             "envc == 2"
il_assert_grep_fixed "$LOG" "ARGV_ECHO getenv_P10=on"      "getenv(P10) == on"

# kernel-side confirmation + no crash.
il_assert_grep "$LOG" "PASS: execve argv/envp self-test completed" "kernel self-test PASS"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                      "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"                                    "no panic"

il_summary
