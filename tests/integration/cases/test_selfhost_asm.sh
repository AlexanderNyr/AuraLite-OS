#!/usr/bin/env bash
# test_selfhost_asm.sh -- SELFHOST_PLAN.md SH4e: mini-asm runs IN-GUEST.
#
# The assembler's self-hosting proof: mini-asm.c is compiled by the guest tcc
# (SH2), linked against the guest libc (SH3's aulink recipe), and then run
# inside AuraLite to assemble the three boot-critical `-f elf64` sources
# (isr_stubs.asm, syscall_entry.asm, boot.asm) with --check-dir, byte-
# comparing each output against the host-built reference objects staged in
# the initrd.  The receipt is the plan's contract line.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host assembler (SH4e): mini-asm built by tcc and run in-guest"

LOG="$IL_LOGDIR/selfhost_asm.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if [ ! -f "$IL_ROOT/build/selfhost/tcc.elf" ]; then
    echo "${C_YELLOW}[selfhost] guest tcc not built -- skipping (run 'make selfhost-deps selfhost-tcc' and rebuild the iso)${C_RESET}"
    il_skip "guest tcc absent from build/selfhost (selfhost-deps not run)"
    il_summary
    exit 0
fi

# --- compile the guest libc + mini-asm with tcc, link the assembler --------
il_send_delay 6
il_send "mkdir /tmp/o"
il_send_delay 1
il_send "mkdir /tmp/out"
il_send_delay 1
il_send "run tcc -c -o /tmp/o/crt0.o /src/libc/tcc_crt0.s"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/libc.o /src/libc/src/libc.c"
il_send_delay 40
il_send "run tcc -c -I/src/libc/include -o /tmp/o/bi.o /src/libc/tcc_builtins.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/malloc.o /src/libc/src/malloc.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/env.o /src/libc/src/env.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/se.o /src/libc/src/string_extra.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/se2.o /src/libc/src/stdlib_extra.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/se3.o /src/libc/src/stdio_extra.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/mini.o /src/mini-asm.c"
il_send_delay 8
il_send "run tcc -nostdlib -static -o /tmp/mini-asm /tmp/o/crt0.o /tmp/o/libc.o /tmp/o/bi.o /tmp/o/malloc.o /tmp/o/env.o /tmp/o/se.o /tmp/o/se2.o /tmp/o/se3.o /tmp/o/mini.o /apps/tcc/libtcc1.a"
il_send_delay 10

# --- assemble the boot-critical sources in-guest, check against refs --------
il_send "stat /tmp/mini-asm"
il_send_delay 1
il_send "run /tmp/mini-asm -f elf64 -I/src/selfhost --check-dir /src/selfhost/ref -o /tmp/out /src/selfhost/isr_stubs.asm /src/selfhost/syscall_entry.asm /src/selfhost/boot.asm"
il_send_delay 10
il_send "exit"

il_run_qemu "$LOG" 300

il_assert_grep "$LOG" "byte-identical"          "in-guest mini-asm reports byte-identical objects"
il_assert_grep "$LOG" "asm PASS: 3/3 objects"   "SH4e receipt: [selfhost] asm PASS: 3/3 objects byte-identical"

il_summary
