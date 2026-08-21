#!/usr/bin/env bash
# tests/integration/a64_shell_smoke.sh -- ARM64_PLAN A5c smoke test.
#
# The auralite# gate, fourth architecture: an Image boot carries the
# four-tenant initrd in, the kernel runs /bina64/init (EL0, ELF64,
# exit 7 asserted), then the SHARED smallsh interactively -- builtins,
# a nested spawn that must ROUND-TRIP its exit code, and the
# cross-tenant refusal (a riscv64 binary named and refused, not
# executed and not crashed on).
#
# Two hard-won regression pins ride along:
#   * the log-size fuse: the SP_EL0 nested-spawn bug made the shell's
#     read() flood -EFAULT prompts at serial speed (20 MB in 40 s).
#     A healthy session is ~5 KB; anything past 200 KB IS that bug
#     class again, so the fuse fails the test before the log eats the
#     workspace.
#   * "exit code 7" from the nested spawn: the exit-code path travels
#     exit_code_box, not the rv64 rc-1 encoding -- the first draft
#     copied the wrong convention and every child "exited 0".
#
# Skips cleanly without qemu-system-aarch64 / llvm-objcopy / the tar.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/kernela64.img"
TAR="$BUILD/initrd.tar"
LOG="$BUILD/a64_shell.log"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[a64-shell] SKIP: qemu-system-aarch64 not installed" >&2
    exit 0
fi
if ! command -v llvm-objcopy >/dev/null 2>&1; then
    echo "[a64-shell] SKIP: llvm-objcopy not installed" >&2
    exit 0
fi
[ -s "$IMG" ] || make -C "$ROOT" kernela64-img >/dev/null
[ -s "$TAR" ] || { echo "[a64-shell] SKIP: build/initrd.tar absent (make iso first)" >&2; exit 0; }

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [a64-shell] OK   %s\n' "$desc"
    else
        printf '  [a64-shell] FAIL %s\n' "$desc"
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [a64-shell] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [a64-shell] OK   %s\n' "$desc"
    fi
}

rm -f "$LOG"
{
    sleep 4
    printf 'uname\n';            sleep 1
    printf 'echo four-tenants\n'; sleep 1
    printf 'run bina64/init\n';  sleep 2
    printf 'run binrv/init\n';   sleep 1
    printf 'exit\n';             sleep 1
} | timeout 45 qemu-system-aarch64 -machine virt -cpu cortex-a72 \
        -m 256M -display none -serial stdio -no-reboot \
        -kernel "$IMG" -initrd "$TAR" > "$LOG" 2>/dev/null || true
tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

# ---- the fuse FIRST: a flooding session must fail fast and small ----
if [ "$(wc -c < "$LOG")" -gt 200000 ]; then
    printf '  [a64-shell] FAIL log-size fuse blown (%s bytes): the prompt-flood bug class is back\n' "$(wc -c < "$LOG")"
    echo "[a64-shell] FAILED (log truncated to 2 KB tail): $LOG"
    tail -c 2048 "$LOG"
    exit 1
fi
printf '  [a64-shell] OK   log-size fuse intact (%s bytes)\n' "$(wc -c < "$LOG")"

# ---- init, the pack-time promise kept at run time ----
assert_grep "\[init\] PASS: inita64 ran and exited 7 as built"  "init ran at EL0 and exited 7"
assert_grep "kernel-pointer write refused (EFAULT) -- good"      "EFAULT negative control from C"

# ---- the shell session ----
assert_grep "AuraLite shell (smallsh): type 'help'"              "the SHARED shell banner (fourth ISA, zero forks)"
assert_grep "AuraLite OS aarch64 (TTBR1 higher half"             "uname identifies the arch"
assert_grep "four-tenants"                                       "echo round-trips"
assert_grep "running /bina64/init (entry 0x0000000008048000"     "nested spawn maps at the treaty address"
assert_grep "depth 1"                                            "spawn nesting depth tracked"
assert_grep "exit code 7"                                        "nested exit code ROUND-TRIPS (the exit_code_box pin)"
assert_grep "refused: machine 243 (riscv64"                      "cross-tenant refusal, NAMED (a64 rejects rv)"
assert_grep "\[shell\] exited 0"                                 "clean shell exit"
assert_grep "A5c complete; powering off"                         "the session ends by PSCI, not timeout"

# ---- never-see ----
assert_no_grep "READ EFAULT"                                     "no EFAULT flood (the SP_EL0 pin)"
# Exactly ONE EL0 fault is legitimate: the A4 self-test's privileged-mrs
# negative control (contained, code 128).  A second one means a real
# userspace crash this session should not have had.
FAULTS=$(grep -ac "EL0 fault" "$LOG" || true)
if [ "$FAULTS" -le 1 ]; then
    printf '  [a64-shell] OK   EL0 faults: %s (the self-test negative control only)\n' "$FAULTS"
else
    printf '  [a64-shell] FAIL %s EL0 faults (only the self-test control is licensed)\n' "$FAULTS"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "[a64-shell] FAILED (log: $LOG)"
    exit 1
fi
echo "[a64-shell] all A5c assertions passed"
