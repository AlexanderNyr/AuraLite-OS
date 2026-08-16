#!/usr/bin/env bash
# tests/integration/i386_user_smoke.sh -- I386_PLAN phase I5 smoke test.
#
# I4 proved Ring 3 with hand-assembled bytes; I5 replaces them with the
# real thing.  Asserts on the `make iso` image under qemu-system-i386:
#
#   * the SHARED initrd.tar mounts on the i386 kernel (USTAR reader);
#   * /bin32/init32 -- an ELF32 binary compiled from C with the 32-bit
#     crt0 + int 0x80 libc -- is found, class-validated, mapped and run
#     in Ring 3;
#   * its output round-trips: banner, pid, sched_yield return;
#   * the negative control runs IN USERSPACE this time: init32 calls
#     write() with a kernel pointer and asserts the kernel refused it
#     (-EFAULT), printing "-- good" only on refusal;
#   * exit(7) round-trips and the kernel logs the [init] PASS verdict;
#   * every earlier gate (sched/user/pmm/vmm/heap/isr/timer) holds.
#
# Plus the standing x86_64 no-regression pair -- doubly load-bearing
# in this phase because initrd.tar itself changed (a /bin32 entry was
# added to the shared archive).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_user.log"
LOG64="$BUILD/i386_user_x64.log"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-user] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
run_qemu() {
    local bin="$1" log="$2"
    rm -f "$log"
    timeout 35 "$bin" \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial file:"$log" -no-reboot \
        >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-user] OK   %s\n' "$desc"
    else
        printf '  [i386-user] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-user] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-user] OK   %s\n' "$desc"
    fi
}

# ---- the i386 userspace bring-up ----
run_qemu qemu-system-i386 "$LOG32"
assert_grep    "$LOG32" "\[initrd\] USTAR at phys.*files"              "i386: shared initrd.tar mounted"
assert_grep    "$LOG32" "\[elf32\] mapped .* user page(s), entry 08048000" "i386: ELF32 parsed and mapped in the user window"
assert_grep    "$LOG32" "running /bin32/init32"                        "i386: init32 launched from /bin32"
assert_grep    "$LOG32" "AuraLite i386 init: userspace is alive"       "i386: compiled-C Ring 3 output"
assert_grep    "$LOG32" "init32: sched_yield returned"                 "i386: second syscall shape round-trips"
assert_grep    "$LOG32" "kernel-pointer write refused (EFAULT) -- good" "i386: userspace-driven EFAULT negative control"
assert_no_grep "$LOG32" "BUG: kernel-pointer write was serviced"       "i386: the kernel never serviced a kernel pointer"
assert_grep    "$LOG32" "\[user\] exit(7) via int 0x80"                "i386: exit(7) trapped"
assert_grep    "$LOG32" "\[init\] PASS: init32 ran and exited 7"       "i386: kernel-side verdict"
assert_grep    "$LOG32" "\[sched\] PASS"                               "i386: I4 sched gate still green"
assert_grep    "$LOG32" "\[user\] PASS"                                "i386: I4 user gate still green"
assert_grep    "$LOG32" "\[pmm\] PASS"                                 "i386: I3 pmm gate still green"
assert_grep    "$LOG32" "\[vmm\] PASS"                                 "i386: I3 vmm gate still green"
assert_grep    "$LOG32" "\[heap\] PASS"                                "i386: I3 heap gate still green"
assert_no_grep "$LOG32" "\[init\] FAIL\|\[elf32\] refused"             "i386: no loader refusals, no init failure"
assert_no_grep "$LOG32" "UNHANDLED EXCEPTION"                          "i386: no unexpected kernel faults"

# ---- the standing x86_64 regression gate (initrd.tar changed!) ----
run_qemu qemu-system-x86_64 "$LOG64"
assert_grep    "$LOG64" "Hello from AuraLite OS kernel!"               "x86_64: kernel banner unchanged"
assert_grep    "$LOG64" "starting init shell (Ring 3)"                 "x86_64: 64-bit init still reached with the fatter initrd"
assert_no_grep "$LOG64" "(i386)"                                       "x86_64: no 32-bit artefacts in the log"

if [ "$fail" -ne 0 ]; then
    echo "[i386-user] FAILED"
    exit 1
fi
echo "[i386-user] all assertions passed"
