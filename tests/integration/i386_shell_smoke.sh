#!/usr/bin/env bash
# tests/integration/i386_shell_smoke.sh -- I386_PLAN phase I7 smoke test.
#
# The `auralite#` gate, delivered where I5 deferred it.  Drives the
# interactive shell over serial on the real `make iso` image:
#
#   * the VGA text console and the PS/2 keyboard initialise (the input
#     RING is shared: serial input exercises the same cons32 path the
#     keyboard feeds);
#   * the shell prompt appears, and each scripted command round-trips:
#     uname, echo (exact string), a NESTED `run bin32/init32` -- a user
#     image spawning a second user image through SYS_SPAWN, with the
#     child's full output and `exit code 7` reported by the SHELL, not
#     the kernel -- an unknown-command diagnostic, and a clean exit;
#   * the kernel survives the whole session and reaches the I7 idle
#     line; no unhandled exceptions anywhere (this catches both
#     regressions of the two REAL bugs this phase found: the cleared-IF
#     hlt deadlock and the esp0-bulldozes-live-frames corruption).
#
# Plus the standing x86_64 no-regression pair.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_shell.log"
LOG64="$BUILD/i386_shell_x64.log"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-shell] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-shell] OK   %s\n' "$desc"
    else
        printf '  [i386-shell] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-shell] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-shell] OK   %s\n' "$desc"
    fi
}

# ---- the interactive session ----
# Generous sleeps: TCG boots through every earlier phase's self-tests
# before the prompt, and serial input typed before the first read is
# only buffered as deep as the UART FIFO.
rm -f "$LOG32"
{
    sleep 25
    printf 'uname\n';                 sleep 2
    printf 'echo shell-gate-echo\n';  sleep 2
    printf 'run bin32/init32\n';      sleep 3
    printf 'ls /\n';                  sleep 2
    printf 'stat etc/motd\n';         sleep 2
    printf 'cat etc/motd\n';          sleep 2
    printf 'definitely-not-a-cmd\n';  sleep 2
    printf 'exit\n';                  sleep 3
} | timeout 50 qemu-system-i386 \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial stdio -no-reboot \
        > "$LOG32" 2>/dev/null || true

assert_grep    "$LOG32" "VGA text console online"                      "i386: VGA console up (kprintf second sink)"
assert_grep    "$LOG32" "PS/2 keyboard on IRQ 1"                       "i386: keyboard driver up"
assert_grep    "$LOG32" "auralite# "                                   "i386: the auralite# prompt (the I5-deferred gate)"
assert_grep    "$LOG32" "AuraLite OS i386 (protected mode"             "i386: uname round-trip"
assert_grep    "$LOG32" "^shell-gate-echo"                             "i386: echo round-trip, exact string"
assert_grep    "$LOG32" "depth 1"                                      "i386: nested spawn ran at depth 1"
assert_grep    "$LOG32" "init32: exiting 7"                            "i386: spawned child's own output seen"
assert_grep    "$LOG32" "^exit code 7"                                 "i386: SHELL reported the child's exit code"
# ---- PARITY P4: the file five over the initrd ----
assert_grep    "$LOG32" "  etc/motd"                                   "i386 P4 ls: readdir lists the initrd"
assert_grep    "$LOG32" "file, "                                       "i386 P4 stat: size through the trap"
assert_grep    "$LOG32" "filesystem layout: see docs/filesystem.md"    "i386 P4 cat: motd content read through fd"
assert_no_grep "$LOG32" "ls: cannot open"                              "i386 P4: open('/') succeeded"
assert_no_grep "$LOG32" "cat: cannot open"                             "i386 P4: open(etc/motd) succeeded"
assert_grep    "$LOG32" "definitely-not-a-cmd: unknown command"        "i386: unknown-command diagnostic"
assert_grep    "$LOG32" "^bye"                                         "i386: clean shell exit"
assert_grep    "$LOG32" "\[shell\] exited 0"                           "i386: kernel observed the shell's exit"
assert_grep    "$LOG32" "console+shell online; idle"                   "i386: kernel survived the whole session"
assert_no_grep "$LOG32" "UNHANDLED EXCEPTION"                          "i386: no faults (esp0 + sti;hlt regressions covered)"
assert_no_grep "$LOG32" "FAIL"                                         "i386: no self-test failures anywhere in the boot"

# ---- the standing x86_64 regression gate ----
rm -f "$LOG64"
timeout 35 qemu-system-x86_64 \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial file:"$LOG64" -no-reboot \
        >/dev/null 2>&1 || true
assert_grep    "$LOG64" "Hello from AuraLite OS kernel!"               "x86_64: kernel banner unchanged"
assert_no_grep "$LOG64" "(i386)"                                       "x86_64: no 32-bit artefacts in the log"

if [ "$fail" -ne 0 ]; then
    echo "[i386-shell] FAILED"
    exit 1
fi
echo "[i386-shell] all assertions passed"
