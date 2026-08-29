#!/usr/bin/env bash
# test_selfhost_kernel_tcc.sh -- SELFHOST_PLAN.md SH5c: the tcc-built kernel
# BOOTS.
#
# The link-level half of SH5c is the host gate
# (tests/unit/test_sh5c_kernel_tcc.sh: 126 tcc objects, aulink link, flag
# audits, packed-layout parity); this case proves the self-host-built kernel
# actually RUNS: tools/selfhost/build_kernel_tcc.sh builds kernel-tcc.elf
# (host tcc for C, mini-asm for the .asm files, aulink with the real
# kernel.ld) and packs a dual-boot ISO through the tree's own
# mkisoimage_dual.sh, then boots it in QEMU and greps the standard
# boot-to-shell receipts -- the same core milestones test_boot_to_shell
# asserts, applied to the self-built kernel.
#
# The kernel is still BUILT ON THE HOST here (SH5c's lane); moving the build
# itself in-guest is SH5d's gate, whose receipt
# ([selfhost] kernel PASS: tcc-built kernel booted to shell) this case
# deliberately does NOT print.
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$(dirname "$0")/.."
. lib/lib.sh

HOST_TCC="$ROOT/build/selfhost/host-tcc-src/tcc"
LIBTCC1="$ROOT/build/selfhost/libtcc1.a"
if [ ! -x "$HOST_TCC" ] || [ ! -f "$LIBTCC1" ]; then
    echo "${C_YELLOW}[selfhost] host tcc / libtcc1 not built -- skipping (run 'make selfhost-deps selfhost-tcc selfhost-host-tcc')${C_RESET}"
    il_skip "host tcc / libtcc1 absent (selfhost deps not run)"
    exit 0
fi

# ---- build the tcc kernel + ISO BEFORE il_init (which would otherwise
# ---- insist on the canonical build/auralite.iso) ------------------------
# build_kernel_tcc.sh creates this directory itself, but the shell has to open
# the redirect target BEFORE that script starts, so the parent must already
# exist.  Without this mkdir the case died at line 1 of the build with
# "No such file or directory" and reported a missing kernel-tcc.iso -- a
# failure that named the wrong thing entirely.  It went unnoticed because the
# case skips unless host tcc was built, and no CI job built it.
mkdir -p "$ROOT/build/selfhost/kernel-tcc"
if ! ( cd "$ROOT" && bash tools/selfhost/build_kernel_tcc.sh --iso ) >"$ROOT/build/selfhost/kernel-tcc/iso-build.log" 2>&1; then
    echo "${C_RED}[lib] tcc kernel build failed${C_RESET}"
    tail -15 "$ROOT/build/selfhost/kernel-tcc/iso-build.log" | sed 's/^/    /'
    exit 2
fi
[ -f "$ROOT/build/selfhost/kernel-tcc.iso" ] || { echo "${C_RED}[lib] kernel-tcc.iso missing${C_RESET}"; exit 2; }

export IL_ISO="$ROOT/build/selfhost/kernel-tcc.iso"
il_init
il_have qemu-system-x86_64

il_section "self-host kernel (SH5c): tcc+mini-asm+aulink kernel boots to the shell"

LOG="$IL_LOGDIR/selfhost_kernel_tcc.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 30
il_send "uname -a"
il_send_delay 2
il_send "sysinfo"
il_send_delay 4
il_send "exit"
il_run_qemu "$LOG" 100

# Core boot milestones (mirrors test_boot_to_shell on the self-built kernel).
il_assert_grep "$LOG" "Hello from AuraLite OS kernel!"              "kmain banner from tcc-compiled C"
il_assert_grep "$LOG" "IDT installed: 256 gates"                    "IDT initialised"
il_assert_grep "$LOG" "TSS loaded"                                  "TSS"
il_assert_grep "$LOG" "SYSCALL/SYSRET configured"                   "MSR setup"
il_assert_grep "$LOG" "HHDM offset: 0xffff800000000000"             "HHDM set up"
il_assert_grep "$LOG" "\[pmm\] PASS:"                               "PMM self-test"
il_assert_grep "$LOG" "\[vmm\] PASS:"                               "Paging self-test"
il_assert_grep "$LOG" "\[sched\] PASS:"                             "Scheduler interleave"
il_assert_grep "$LOG" "\[vfs\] PASS:"                               "VFS layer functional"
il_assert_grep "$LOG" "\[smp\] PASS:"                               "SMP bring-up"
il_assert_grep "$LOG" "\[boot\] starting init shell \(Ring 3\)"     "Ring 3 init reached"
il_assert_grep "$LOG" "auralite#"                                   "Interactive shell prompt"
il_assert_grep "$LOG" "AuraLite OS 0.0.1 x86_64"                    "uname answers from the tcc kernel"
il_assert_grep "$LOG" "\[perf\] boot-to-shell:"                     "boot-to-shell perf receipt"
# The tcc kernel runs the userland binary through the SYSCALL path the
# tcc-compiled syscall_dispatch implements.
il_assert_grep "$LOG" "'/bin/sysinfo'.*exited \(code=0\)" \
    "a Ring 3 child ran and exited cleanly"

il_assert_no_grep "$LOG" "PANIC"                                    "no kernel panic"
il_assert_no_grep "$LOG" "TRIPLE FAULT"                             "no triple-fault"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                      "no unhandled exception"
il_assert_no_grep "$LOG" "STOP="                                    "no BSOD stop"

il_summary
