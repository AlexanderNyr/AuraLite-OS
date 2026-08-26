#!/usr/bin/env bash
# test_selfhost_tcc.sh — SELFHOST_PLAN.md SH1: the guest TinyCC compiles and
# links a freestanding binary inside AuraLite, and that binary runs.
#
# This is the first link of the self-hosting chain: /bin/tcc is built by
# the HOST (make selfhost-deps selfhost-tcc) against AuraLite's own libc,
# shipped in the initrd, and then does real work in the guest:
#
#   run tcc -nostdlib -o /tmp/h /tests/selfhost_hello.c
#   run /tmp/h            -> prints the §8 receipt "[selfhost] tcc PASS: ..."
#
# The binary it produces is linked by tcc's OWN ELF linker (no host
# linker), so this exercises compiler + assembler + linker end to end.
#
# Skips (with a printed note, not a failure) when the toolchain was not
# built: `make iso` only stages it when build/selfhost/tcc-src exists
# (make selfhost-deps).  Wiring the toolchain into CI lands in SH9.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host toolchain (SH1): guest tcc builds + runs a binary"

LOG="$IL_LOGDIR/selfhost_tcc.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if [ ! -f "$IL_ROOT/build/selfhost/tcc.elf" ]; then
    echo "${C_YELLOW}[selfhost] guest tcc not built -- skipping "
         "(run 'make selfhost-deps selfhost-tcc' and rebuild the iso)${C_RESET}"
    il_skip "guest tcc absent from build/selfhost (selfhost-deps not run)"
    il_summary
    exit 0
fi

il_send_delay 6
il_send "ls /bin/tcc"
il_send_delay 1
il_send "run tcc -v"
il_send_delay 3
il_send "run tcc -nostdlib -o /tmp/h /tests/selfhost_hello.c"
il_send_delay 6
il_send "stat /tmp/h"
il_send_delay 1
il_send "run /tmp/h"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_grep "$LOG" "/bin/tcc"            "guest tcc is staged in the initrd"
il_assert_grep "$LOG" "tcc version"         "guest tcc runs and reports its version"
il_assert_grep "$LOG" "selfhost] tcc PASS"  "the receipt contract line was printed by the guest-built binary"
il_assert_grep "$LOG" "Size:"               "tcc wrote its output to /tmp/h"

il_summary
