#!/usr/bin/env bash
# test_selfhost_userland.sh — SELFHOST_PLAN.md SH2: the guest toolchain
# rebuilds AuraLite's own userland from source, inside AuraLite.
#
# Everything is built by /bin/tcc in the guest, no host tool touches the
# pipeline after the ISO boots:
#   - crt0 (tools/selfhost/tcc_crt0.s, assembled by tcc's own assembler)
#   - libc core (src/libc/src/libc.c + malloc/env/string_extra/stdlib_extra)
#   - __builtin_* helpers (tcc_builtins.c)
#   - the apps: sysinfo (real app) and userland_ok.c (the §8 receipt)
#   - linked by tcc's ELF linker with /apps/tcc/libtcc1.a
# Then the rebuilt binaries RUN in Ring 3: sysinfo prints its full banner,
# userland_ok prints "[selfhost] userland rebuild PASS: 2 binaries".
#
# Skips (with a note) when make selfhost-deps was not run.
#
# Runtime budget: ~3 min in QEMU/TCG (the libc compile alone is ~40 s).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host userland (SH2): tcc rebuilds the libc + apps in-guest"

LOG="$IL_LOGDIR/selfhost_userland.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if [ ! -f "$IL_ROOT/build/selfhost/tcc.elf" ]; then
    echo "${C_YELLOW}[selfhost] guest tcc not built -- skipping "
         "(run 'make selfhost-deps selfhost-tcc' and rebuild the iso)${C_RESET}"
    il_skip "guest tcc absent from build/selfhost (selfhost-deps not run)"
    il_summary
    exit 0
fi

# ---- build steps (each is one shell line; long link lines need
# MAX_ARGS 32 / INPUT_MAX 512 in init.c, raised in SH2) ----
il_send_delay 6
il_send "mkdir /tmp/o"
il_send_delay 1
il_send "run tcc -c -o /tmp/o/crt0.o /src/libc/tcc_crt0.s"
il_send_delay 4
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
il_send "run tcc -c -I/src/libc/include -o /tmp/o/sysinfo.o /src/apps/sysinfo.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/editor.o /src/apps/editor.c"
il_send_delay 4
il_send "run tcc -c -I/src/libc/include -o /tmp/o/ok.o /src/apps/userland_ok.c"
il_send_delay 4
il_send "run tcc -nostdlib -static -o /tmp/sysinfo /tmp/o/crt0.o /tmp/o/libc.o /tmp/o/bi.o /tmp/o/malloc.o /tmp/o/env.o /tmp/o/se.o /tmp/o/se2.o /tmp/o/sysinfo.o /apps/tcc/libtcc1.a"
il_send_delay 12
il_send "run tcc -nostdlib -static -o /tmp/editor /tmp/o/crt0.o /tmp/o/libc.o /tmp/o/bi.o /tmp/o/malloc.o /tmp/o/env.o /tmp/o/se.o /tmp/o/se2.o /tmp/o/editor.o /apps/tcc/libtcc1.a"
il_send_delay 10
il_send "run tcc -nostdlib -static -o /tmp/ok /tmp/o/crt0.o /tmp/o/libc.o /tmp/o/bi.o /tmp/o/malloc.o /tmp/o/env.o /tmp/o/se.o /tmp/o/se2.o /tmp/o/ok.o /apps/tcc/libtcc1.a"
il_send_delay 8
il_send "stat /tmp/sysinfo"
il_send_delay 1
il_send "run /tmp/sysinfo"
il_send_delay 6
il_send "run /tmp/ok"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 300

il_assert_grep "$LOG" "System Information"   "guest-built sysinfo runs and prints its banner"
il_assert_grep "$LOG" "Process ID"           "guest-built sysinfo reaches getpid()"
il_assert_grep "$LOG" "userland rebuild PASS" "the §8 receipt was printed by the guest-built program"
il_assert_grep "$LOG" "Size:"                "tcc wrote the rebuilt binaries to /tmp"

il_summary
