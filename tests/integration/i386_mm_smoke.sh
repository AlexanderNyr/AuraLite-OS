#!/usr/bin/env bash
# tests/integration/i386_mm_smoke.sh -- I386_PLAN phase I3 smoke test.
#
# Asserts the i386 memory contract on the real `make iso` image:
#
#   * the kernel runs HIGHER-HALF: the #BP self-test's eip must be a
#     0xC01xxxxx address (the strongest single signal that paging is on
#     and the linker/loader/boot32 chain agreed on the layout);
#   * hhdm_offset arrived as 0xC0000000 and was validated, not assumed;
#   * the boot identity window was dropped (NULL faults from here on);
#   * the honest D3 line is printed: no PAE => no NX on i386;
#   * [pmm] PASS, [vmm] PASS, [heap] PASS -- the same self-test
#     contract the x86_64 boot enforces, one width down;
#   * the I2 gates (#BP named+resumed, PIT ticking) still hold;
#   * nothing triple-faulted: the boot reaches the I3 idle line.
#
# Plus the standing x86_64 no-regression pair.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_mm.log"
LOG64="$BUILD/i386_mm_x64.log"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-mm] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
run_qemu() {
    local bin="$1" log="$2"
    rm -f "$log"
    timeout 30 "$bin" \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial file:"$log" -no-reboot \
        >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-mm] OK   %s\n' "$desc"
    else
        printf '  [i386-mm] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-mm] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-mm] OK   %s\n' "$desc"
    fi
}

# ---- the i386 memory bring-up ----
run_qemu qemu-system-i386 "$LOG32"
assert_grep    "$LOG32" "higher half at 0xC0100000"                    "i386: banner claims higher half"
assert_grep    "$LOG32" "eip=c01"                                      "i386: #BP eip proves execution IS higher-half"
assert_grep    "$LOG32" "HHDM offset: c0000000"                        "i386: hhdm_offset validated (loader-owns-it contract)"
assert_grep    "$LOG32" "identity window \[0, 896 MiB) dropped"        "i386: boot identity map dropped"
assert_grep    "$LOG32" "no PAE => no NX"                              "i386: the D3 consequence stated in the log"
assert_grep    "$LOG32" "\[pmm\] PASS"                                 "i386: PMM self-test"
assert_grep    "$LOG32" "\[vmm\] PASS"                                 "i386: paging self-test (map/alias/unmap)"
assert_grep    "$LOG32" "\[heap\] PASS"                                "i386: heap self-test (10000 cycles)"
assert_grep    "$LOG32" "\[isr\] PASS"                                 "i386: I2 #BP gate still green"
assert_grep    "$LOG32" "\[timer\] PASS"                               "i386: I2 PIT gate still green"
assert_grep    "$LOG32" "I3 memory online"                             "i386: reached idle -- no triple fault"
assert_no_grep "$LOG32" "FAIL"                                         "i386: no self-test failures anywhere"
assert_no_grep "$LOG32" "UNHANDLED EXCEPTION"                          "i386: no unexpected faults"

# ---- the standing x86_64 regression gate ----
run_qemu qemu-system-x86_64 "$LOG64"
assert_grep    "$LOG64" "Hello from AuraLite OS kernel!"               "x86_64: kernel banner unchanged"
assert_grep    "$LOG64" "HHDM offset: 0xffff800000000000"              "x86_64: 64-bit HHDM untouched by the 32-bit hhdm write"
assert_no_grep "$LOG64" "(i386)"                                       "x86_64: no 32-bit artefacts in the log"

if [ "$fail" -ne 0 ]; then
    echo "[i386-mm] FAILED"
    exit 1
fi
echo "[i386-mm] all assertions passed"
