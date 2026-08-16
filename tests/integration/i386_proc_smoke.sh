#!/usr/bin/env bash
# tests/integration/i386_proc_smoke.sh -- I386_PLAN phase I4 smoke test.
#
# Asserts the process contract on the real `make iso` image:
#
#   * [sched] PASS -- two kernel threads made progress under PIT
#     preemption with the boot thread hlt-waiting (neither worker ever
#     yields: progress IS the proof of preemption);
#   * the int 0x80 gate is armed DPL=3 with AuraLite numbers (D4);
#   * a Ring 3 program actually ran: the EXACT string "RING3-OK" was
#     written through SYS_WRITE from user space (exact because the
#     bytes are hand-assembled -- an off-by-one in the padding once
#     printed "ING3-OK", and this assert is what caught it);
#   * exit(42) round-tripped through int 0x80;
#   * the negative control: a privileged `hlt` from Ring 3 was
#     terminated via #GP containment (code 141 = 128+13), and the
#     kernel SURVIVED -- the [user] PASS line and the I4 idle line
#     both print after it;
#   * all earlier phase gates (pmm/vmm/heap/isr/timer) still hold.
#
# Plus the standing x86_64 no-regression pair.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_proc.log"
LOG64="$BUILD/i386_proc_x64.log"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-proc] SKIP: qemu-system-i386 not installed" >&2
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
        printf '  [i386-proc] OK   %s\n' "$desc"
    else
        printf '  [i386-proc] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-proc] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-proc] OK   %s\n' "$desc"
    fi
}

# ---- the i386 process bring-up ----
run_qemu qemu-system-i386 "$LOG32"
assert_grep    "$LOG32" "\[sched\] round-robin online (BSP-only"       "i386: scheduler online, D5 stated"
assert_grep    "$LOG32" "\[sched\] PASS: 2 workers preempted"          "i386: preemptive interleave proven"
assert_grep    "$LOG32" "int 0x80 gate armed (DPL=3)"                  "i386: syscall gate DPL=3 (D4)"
# ^RING3-OK\r?$ -- kputc32 emits CRLF on the serial line, so the raw
# log carries a trailing \r before the newline.
assert_grep    "$LOG32" $'^RING3-OK\r\{0,1\}$'                          "i386: Ring 3 SYS_WRITE output, exact string"
assert_grep    "$LOG32" "\[user\] exit(42) via int 0x80"               "i386: exit code round-trip"
assert_grep    "$LOG32" "vector=13.*terminating image (code 141)"      "i386: negative control -- Ring 3 hlt contained via #GP"
assert_grep    "$LOG32" "\[user\] PASS: Ring 3 write/getpid/exit"      "i386: user self-test verdict"
# Phase-advancing idle line (the same generalisation the I2/I3 smokes
# carry): the negative control runs BEFORE this line prints, so any
# "online; idle" reached after it proves the kernel survived the fault.
assert_grep    "$LOG32" "online; idle (I[0-9]"                         "i386: kernel SURVIVED the Ring 3 fault -- reached idle"
assert_grep    "$LOG32" "\[pmm\] PASS"                                 "i386: I3 pmm gate still green"
assert_grep    "$LOG32" "\[vmm\] PASS"                                 "i386: I3 vmm gate still green"
assert_grep    "$LOG32" "\[heap\] PASS"                                "i386: I3 heap gate still green"
assert_grep    "$LOG32" "\[isr\] PASS"                                 "i386: I2 #BP gate still green"
assert_grep    "$LOG32" "\[timer\] PASS"                               "i386: I2 PIT gate still green"
assert_no_grep "$LOG32" "UNHANDLED EXCEPTION"                          "i386: no unexpected kernel faults"
assert_no_grep "$LOG32" "\[sched\] FAIL\|\[user\] FAIL"                "i386: no self-test failures"

# ---- the standing x86_64 regression gate ----
run_qemu qemu-system-x86_64 "$LOG64"
assert_grep    "$LOG64" "Hello from AuraLite OS kernel!"               "x86_64: kernel banner unchanged"
assert_no_grep "$LOG64" "(i386)"                                       "x86_64: no 32-bit artefacts in the log"

if [ "$fail" -ne 0 ]; then
    echo "[i386-proc] FAILED"
    exit 1
fi
echo "[i386-proc] all assertions passed"
