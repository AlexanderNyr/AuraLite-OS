#!/usr/bin/env bash
# tests/integration/rv_shell_smoke.sh -- RISCV_PLAN phase V5 smoke test.
#
# The `auralite#` gate at the third arch: i386_shell_smoke.sh's session
# script with the arch swapped -- and that sameness is the point, since
# the shell being driven IS the same source file (smallsh.c, promoted
# from shell32.c in this phase).
#
#   * init runs first from /binrv: its exact output lines including the
#     EFAULT negative control FROM USERSPACE, and `exiting 7`;
#   * the shell prompt appears over the SBI console; each scripted
#     command round-trips: uname, echo (exact string), a NESTED
#     `run binrv/init` (SYS_SPAWN at depth 1, the child's output and
#     `exit code 7` reported by the SHELL), an unknown-command
#     diagnostic, and a clean exit;
#   * the ELF loader logged real p_flags mapping (this loader ENFORCES
#     W^X where i386's could only state it);
#   * the kernel survives the session and reaches the V5 idle line.
#
# Skips cleanly when qemu-system-riscv64 is not installed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ELF="$BUILD/kernelrv.elf"
TAR="$BUILD/initrd.tar"
LOG="$BUILD/rv_shell.log"

if ! command -v qemu-system-riscv64 >/dev/null 2>&1; then
    echo "[rv-shell] SKIP: qemu-system-riscv64 not installed" >&2
    exit 0
fi

[ -s "$ELF" ] || make -C "$ROOT" kernelrv >/dev/null
[ -s "$TAR" ] || make -C "$ROOT" "$TAR" >/dev/null

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [rv-shell] OK   %s\n' "$desc"
    else
        printf '  [rv-shell] FAIL %s\n' "$desc"
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [rv-shell] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [rv-shell] OK   %s\n' "$desc"
    fi
}

# ---- the interactive session ----
# Modest sleeps: this kernel reaches the prompt in about a second (no
# TCG x86 self-test gauntlet before it), but the first read only sees
# input typed after the ELF loads.
rm -f "$LOG"
{
    sleep 4
    printf 'uname\n';                  sleep 1
    printf 'echo rv-shell-gate-echo\n'; sleep 1
    printf 'run binrv/init\n';         sleep 2
    printf 'definitely-not-a-cmd\n';   sleep 1
    printf 'exit\n';                   sleep 2
} | timeout 30 qemu-system-riscv64 \
        -machine virt -m 256M \
        -display none -serial stdio -no-reboot \
        -kernel "$ELF" -initrd "$TAR" \
        > "$LOG" 2>/dev/null || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

# ---- init from /binrv ----
assert_grep    "AuraLite rv64 init: userspace is alive"          "init: alive banner from /binrv"
assert_grep    "kernel-pointer write refused (EFAULT)"           "init: EFAULT negative control FROM USERSPACE"
assert_grep    "initrv: exiting 7"                               "init: exits as built"
assert_grep    "\[init\] PASS"                                   "init: kernel saw exit code 7"

# ---- the loader's real-p_flags line ----
assert_grep    "p_flags honoured: PF_X->RX, PF_W->RW"            "loader: W^X-real p_flags mapping logged"

# ---- the shell session ----
assert_grep    "auralite# "                                      "shell: the auralite# prompt (third arch)"
assert_grep    "AuraLite OS riscv64 (Sv39 higher half"           "shell: uname round-trip"
assert_grep    "^rv-shell-gate-echo"                             "shell: echo round-trip, exact string"
assert_grep    "depth 1"                                         "shell: nested spawn ran at depth 1"
assert_grep    "^exit code 7"                                    "shell: SHELL reported the child's exit code"
assert_grep    "definitely-not-a-cmd: unknown command"           "shell: unknown-command diagnostic"
assert_grep    "^bye"                                            "shell: clean exit"
assert_grep    "\[shell\] exited 0"                              "shell: kernel observed the exit"
assert_grep    "V5 complete; console+shell online"               "kernel survived the whole session"
assert_no_grep "UNHANDLED"                                       "no unhandled traps anywhere"

if [ "$fail" -ne 0 ]; then
    echo "[rv-shell] FAILED"
    exit 1
fi
echo "[rv-shell] all assertions passed"
