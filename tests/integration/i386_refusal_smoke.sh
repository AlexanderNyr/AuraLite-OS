#!/usr/bin/env bash
# tests/integration/i386_refusal_smoke.sh -- I386_PLAN phase I0 smoke test.
#
# Before I0, booting build/auralite.iso on a CPU without long mode hung
# silently right after "[BL4] entering long mode" -- the log claimed the
# hand-off happened and nothing further ever appeared.  I0 adds a CPUID
# check (boot/bios/stage2/lmcheck.inc) that runs BEFORE any long-mode
# commitment and prints an honest refusal on COM1 and the VGA console.
#
# This test asserts three things:
#   1. qemu-system-i386 (qemu32 vCPU: CPUID present, no LM) -> refusal,
#      and the "entering long mode" line does NOT appear.
#   2. qemu-system-i386 -cpu 486 (no CPUID at all: the EFLAGS.ID toggle
#      path) -> the same refusal.
#   3. qemu-system-x86_64 with the SAME image bytes -> the check passes
#      ("[BL10] CPU supports long mode") and the 64-bit kernel boots to
#      its banner exactly as before.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_refusal_qemu32.log"
LOG486="$BUILD/i386_refusal_486.log"
LOG64="$BUILD/i386_refusal_x64.log"

REFUSAL="\[BL10\] this CPU has no long mode"
COMMIT="entering long mode"

# 1. Build the image if it is not there (CI runs `make iso` first; local
#    runs should not need to remember).
[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-refusal] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
run_qemu() {
    local bin="$1" cpu="$2" log="$3"
    rm -f "$log"
    local cpuargs=()
    [ -n "$cpu" ] && cpuargs=(-cpu "$cpu")
    timeout 20 "$bin" "${cpuargs[@]}" \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial file:"$log" -no-reboot \
        >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-refusal] OK   %s\n' "$desc"
    else
        printf '  [i386-refusal] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-refusal] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-refusal] OK   %s\n' "$desc"
    fi
}

# ---- Case 1: qemu32 (CPUID yes, extended leaf reports no LM). ----
run_qemu qemu-system-i386 "" "$LOG32"
assert_grep    "$LOG32" "$REFUSAL" "qemu32: refusal printed"
assert_no_grep "$LOG32" "$COMMIT"  "qemu32: long-mode entry never attempted"

# ---- Case 2: 486 (no CPUID; the EFLAGS.ID toggle path). ----
run_qemu qemu-system-i386 486 "$LOG486"
assert_grep    "$LOG486" "$REFUSAL" "486: refusal printed (no-CPUID path)"
assert_no_grep "$LOG486" "$COMMIT"  "486: long-mode entry never attempted"

# ---- Case 3: the same bytes on a 64-bit CPU boot exactly as before. ----
run_qemu qemu-system-x86_64 "" "$LOG64"
assert_grep    "$LOG64" "\[BL10\] CPU supports long mode"    "x86_64: check passes"
assert_grep    "$LOG64" "$COMMIT"                            "x86_64: long-mode entry proceeds"
assert_grep    "$LOG64" "Hello from AuraLite OS kernel!"     "x86_64: kernel banner unchanged"
assert_no_grep "$LOG64" "$REFUSAL"                           "x86_64: no refusal text"

if [ "$fail" -ne 0 ]; then
    echo "[i386-refusal] FAILED"
    exit 1
fi
echo "[i386-refusal] all assertions passed"
