#!/usr/bin/env bash
# rv_smp_smoke.sh — PARITY_PLAN.md P5: SMP bring-up on rv64, proven.
#
# One boot with -smp 4 and no disk (the SMP receipts do not need
# one): the boot hart starts every stopped DTB hart through SBI HSM,
# each secondary reports in from its own stack, and one sPI IPI
# round-trip is acked by every online hart.  D5 scope: receipts, not
# scheduling — the secondaries park in wfi and the smoke says so.
#
# QEMU output to a file only (the standing rule); 200KB fuse.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ELF="$BUILD/kernelrv.elf"
LOG="$BUILD/rv_smp.log"

if ! command -v qemu-system-riscv64 >/dev/null 2>&1; then
    echo "[rv-smp] SKIP: qemu-system-riscv64 not installed" >&2
    exit 0
fi

[ -s "$ELF" ] || make -C "$ROOT" kernelrv >/dev/null

rm -f "$LOG"
timeout 120 qemu-system-riscv64 \
        -machine virt -smp 4 -m 256M \
        -display none -serial file:"$LOG" -no-reboot \
        -kernel "$ELF" \
        < /dev/null > /dev/null 2>&1 || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

if [ "$(wc -c < "$LOG")" -gt 200000 ]; then
    echo "[rv-smp] FAIL: log exceeds the 200KB fuse (runaway output)" >&2
    exit 1
fi

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [rv-smp] OK   $desc"
    else
        echo "  [rv-smp] FAIL $desc (pattern: $pat)" >&2
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [rv-smp] FAIL $desc (found: $pat)" >&2
        fail=1
    else
        echo "  [rv-smp] OK   $desc"
    fi
}

assert_grep "\[smp\] 4 hart(s) in the DTB"          "the DTB names four harts"
COUNT=$(grep -ac "online (stack top" "$LOG" || true)
if [ "$COUNT" -eq 3 ]; then
    echo "  [rv-smp] OK   exactly 3 secondaries reported in (counted, not assumed)"
else
    echo "  [rv-smp] FAIL secondary report-ins: $COUNT (expected 3)" >&2
    fail=1
fi
assert_grep "\[smp\] online: 3/3 started hart(s)"   "the boot hart counted them too"
assert_grep "\[smp\] IPI round-trip: 3/3 ack(s)"    "one sPI IPI, three acks"
ACKS=$(grep -ac "IPI received, parking" "$LOG" || true)
if [ "$ACKS" -eq 3 ]; then
    echo "  [rv-smp] OK   every ack came from a NAMED hart line"
else
    echo "  [rv-smp] FAIL per-hart ack lines: $ACKS (expected 3)" >&2
    fail=1
fi
assert_no_grep "hart_start.*FAILED"                 "no HSM start failure"
assert_no_grep "send_ipi FAILED"                    "no IPI send failure"
assert_no_grep "UNHANDLED EXCEPTION"                "no unhandled trap anywhere in the boot"

if [ "$fail" -ne 0 ]; then
    echo "[rv-smp] FAILED — log tail:" >&2
    tail -25 "$LOG" >&2
    exit 1
fi
echo "[rv-smp] all assertions passed"
