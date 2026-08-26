#!/usr/bin/env bash
# test_selfhost_aulink.sh -- SELFHOST_PLAN.md SH3: aulink links the userland
# IN-GUEST, replacing the tcc-built-in linker for the final link.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host linker (SH3): aulink links the userland in-guest"

LOG="$IL_LOGDIR/selfhost_aulink.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if [ ! -f "$IL_ROOT/build/selfhost/tcc.elf" ]; then
    echo "${C_YELLOW}[selfhost] guest tcc not built -- skipping (run 'make selfhost-deps selfhost-tcc' and rebuild the iso)${C_RESET}"
    il_skip "guest tcc absent from build/selfhost (selfhost-deps not run)"
    il_summary
    exit 0
fi

il_send_delay 6
il_send "mkdir /tmp/o"
il_send_delay 1
il_send "run tcc -c -o /tmp/o/crt0.o /src/libc/tcc_crt0.s"
il_send_delay 5
il_send "run tcc -c -I/src/libc/include -o /tmp/o/libc.o /src/libc/src/libc.c"
il_send_delay 45
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
il_send "run tcc -c -I/src/libc/include -o /tmp/o/sysinfo.o /src/apps/sysinfo.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/aulink.o /src/aulink.c"
il_send_delay 8
il_send "run tcc -nostdlib -static -o /tmp/aulink /tmp/o/crt0.o /tmp/o/libc.o /tmp/o/bi.o /tmp/o/malloc.o /tmp/o/env.o /tmp/o/se.o /tmp/o/se2.o /tmp/o/se3.o /tmp/o/aulink.o /apps/tcc/libtcc1.a"
il_send_delay 10
il_send "run /tmp/aulink -T /src/libc/user.ld -o /tmp/sysinfo-au /tmp/o/crt0.o /tmp/o/libc.o /tmp/o/bi.o /tmp/o/malloc.o /tmp/o/env.o /tmp/o/se.o /tmp/o/se2.o /tmp/o/se3.o /tmp/o/sysinfo.o /apps/tcc/libtcc1.a"
il_send_delay 10
il_send "stat /tmp/sysinfo-au"
il_send_delay 1
il_send "run /tmp/sysinfo-au"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 300

il_assert_grep "$LOG" "System Information"   "aulink-linked sysinfo runs and prints its banner"
il_assert_grep "$LOG" "Process ID"           "aulink-linked sysinfo reaches getpid()"
il_assert_grep "$LOG" "Size:"                "aulink wrote /tmp/sysinfo-au"

il_summary
