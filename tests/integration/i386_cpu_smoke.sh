#!/usr/bin/env bash
# tests/integration/i386_cpu_smoke.sh -- I386_PLAN phase I2 smoke test.
#
# I1 proved the chain with a stub; I2 replaced the stub with the real
# bring-up kernel.  This test asserts the whole I2 contract on the real
# `make iso` image under qemu-system-i386:
#
#   * the I1 stub is gone (its banner must NOT appear);
#   * GDT + 32-bit TSS, 256-gate IDT, PIC remap all report;
#   * interrupts are enabled and the PIT self-test observes real ticks
#     (this exercises gate wiring, PIC unmask and EOI end to end);
#   * a deliberate int3 produces a NAMED diagnostic with a register
#     dump and a cpu number (the FIX_R0 discipline), and execution
#     RESUMES -- the "[isr] PASS" line prints after the dump;
#   * the boot reaches the I2 idle line -- nothing triple-faulted.
#
# Plus the standing regression gate: the same bytes on qemu-system-x86_64
# still boot the 64-bit kernel with no 32-bit artefacts in the log.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_cpu.log"
LOG64="$BUILD/i386_cpu_x64.log"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-cpu] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
run_qemu() {
    local bin="$1" log="$2"
    rm -f "$log"
    timeout 25 "$bin" \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial file:"$log" -no-reboot \
        >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-cpu] OK   %s\n' "$desc"
    else
        printf '  [i386-cpu] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-cpu] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-cpu] OK   %s\n' "$desc"
    fi
}

# ---- the i386 bring-up ----
run_qemu qemu-system-i386 "$LOG32"
assert_grep    "$LOG32" "Hello from AuraLite OS kernel (i386)!"        "i386: kernel banner"
assert_no_grep "$LOG32" "i386 stub alive"                              "i386: the I1 stub is gone"
assert_grep    "$LOG32" "handoff magic OK"                             "i386: boot_info accepted"
assert_grep    "$LOG32" "GDT loaded (kernel + user segments + 32-bit TSS)" "i386: GDT + TSS"
assert_grep    "$LOG32" "IDT installed: 256 gates"                     "i386: IDT"
assert_grep    "$LOG32" "PIC remapped (IRQs -> vectors 32-47)"         "i386: PIC remap"
assert_grep    "$LOG32" "interrupts enabled, exception handling online" "i386: sti reached"
assert_grep    "$LOG32" "deliberate #BP self-test"                     "i386: int3 fired"
assert_grep    "$LOG32" "vector=3 (Breakpoint)"                        "i386: exception NAMED"
assert_grep    "$LOG32" "cpu=0"                                        "i386: diagnostic names the CPU (R0 discipline)"
assert_grep    "$LOG32" "\[isr\] PASS: deliberate #BP named, dumped, resumed" "i386: execution resumed after the fault"
assert_grep    "$LOG32" "\[timer\] PASS: PIT ticking"                  "i386: PIT ticks observed (gate+PIC+EOI path)"
# I3 note: the final line advances phase by phase ("I2 bring-up
# complete" -> "I3 memory online" -> ...); assert the *contract* (the
# boot reached an idle line) rather than a phase number this test would
# then pin forever.
assert_grep    "$LOG32" "idle (I[0-9] adds\|memory online; idle"       "i386: reached idle -- no triple fault"
assert_no_grep "$LOG32" "UNHANDLED EXCEPTION"                          "i386: no unexpected faults"

# ---- the standing x86_64 regression gate ----
run_qemu qemu-system-x86_64 "$LOG64"
assert_grep    "$LOG64" "Hello from AuraLite OS kernel!"               "x86_64: kernel banner unchanged"
assert_no_grep "$LOG64" "(i386)"                                       "x86_64: no 32-bit artefacts in the log"

if [ "$fail" -ne 0 ]; then
    echo "[i386-cpu] FAILED"
    exit 1
fi
echo "[i386-cpu] all assertions passed"
