#!/usr/bin/env bash
# a64_smp_smoke.sh — PARITY_PLAN.md P6: SMP bring-up on aarch64,
# proven.  (Written by hand, not derived by sed — the P3 lesson.)
#
# One boot with -smp 8: the boot core starts every powered-off DTB
# core through PSCI CPU_ON, each secondary reports in from its own
# stack, brings up its BANKED GICv2 CPU interface, and acks one SGI
# round-trip by polling IAR — off the trap path (D5: receipts, not
# scheduling).
#
# -smp 8 is GICv2's ARCHITECTURAL ceiling (8 CPU interfaces, 8 SGIR
# target bits); QEMU virt refuses more with gic-version=2.  The x16
# code limit is proven at -smp 16 by rv_smp_smoke.sh's second lane;
# GICv3 lifts this port's ceiling and is named residue in the plan.
#
# QEMU output to a file only (the standing rule); 200KB fuse.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ELF="$BUILD/kernela64.elf"
IMG="$BUILD/kernela64.img"
TAR="$BUILD/initrd.tar"
LOG="$BUILD/a64_smp.log"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[a64-smp] SKIP: qemu-system-aarch64 not installed" >&2
    exit 0
fi

[ -s "$ELF" ] || make -C "$ROOT" kernela64 >/dev/null
[ -s "$IMG" ] || make -C "$ROOT" kernela64-img >/dev/null
[ -s "$TAR" ] || make -C "$ROOT" build/initrd.tar >/dev/null

rm -f "$LOG"
timeout 180 qemu-system-aarch64 \
        -machine virt -cpu cortex-a72 -smp 8 -m 256M \
        -display none -serial file:"$LOG" -no-reboot \
        -kernel "$IMG" -initrd "$TAR" \
        < /dev/null > /dev/null 2>&1 || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

if [ "$(wc -c < "$LOG")" -gt 200000 ]; then
    echo "[a64-smp] FAIL: log exceeds the 200KB fuse (runaway output)" >&2
    exit 1
fi

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [a64-smp] OK   $desc"
    else
        echo "  [a64-smp] FAIL $desc (pattern: $pat)" >&2
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [a64-smp] FAIL $desc (found: $pat)" >&2
        fail=1
    else
        echo "  [a64-smp] OK   $desc"
    fi
}

assert_grep "\[smp\] 8 core(s) in the DTB"          "the DTB names eight cores"
COUNT=$(grep -ac "online (stack top" "$LOG" || true)
if [ "$COUNT" -eq 7 ]; then
    echo "  [a64-smp] OK   exactly 7 secondaries reported in (counted, not assumed)"
else
    echo "  [a64-smp] FAIL secondary report-ins: $COUNT (expected 7)" >&2
    fail=1
fi
assert_grep "\[smp\] online: 7/7 started core(s)"   "the boot core counted them too"
assert_grep "\[smp\] IPI round-trip: 7/7 ack(s)"    "one SGI, seven acks via banked IAR"
ACKS=$(grep -ac "IPI received, parking" "$LOG" || true)
if [ "$ACKS" -eq 7 ]; then
    echo "  [a64-smp] OK   every ack came from a NAMED core line"
else
    echo "  [a64-smp] FAIL per-core ack lines: $ACKS (expected 7)" >&2
    fail=1
fi
assert_no_grep "CPU_ON.*FAILED"                     "no PSCI start failure"
assert_no_grep "UNHANDLED\|SYNC EXCEPTION\|panic"   "no trap anywhere in the boot"
# R5 (RES-14): user code OFF the boot core, receipts counted.
assert_grep "init ran at EL0 ON CORE"               "R5: init executed on a secondary core"
assert_grep "inita64: exiting 7"                    "R5: the user code itself spoke from that core"

# ---- R4: the GICv3 x16 lane (the ceiling GICv2 could not lift) ------
LOG16="$BUILD/a64_smp16.log"
rm -f "$LOG16"
timeout 240 qemu-system-aarch64 \
        -machine virt,gic-version=3 -cpu cortex-a72 -smp 16 -m 256M \
        -display none -serial file:"$LOG16" -no-reboot \
        -kernel "$ELF" \
        < /dev/null > /dev/null 2>&1 || true
tr -d '\r' < "$LOG16" > "$LOG16.clean" && mv "$LOG16.clean" "$LOG16"

C16=$(grep -ac "online (stack top" "$LOG16" || true)
if [ "$C16" -eq 15 ]; then
    echo "  [a64-smp] OK   x16/GICv3 lane: exactly 15 secondaries reported in"
else
    echo "  [a64-smp] FAIL x16/GICv3 report-ins: $C16 (expected 15)" >&2
    fail=1
fi
if grep -qa "\[smp\] IPI round-trip: 15/15 ack(s)" "$LOG16"; then
    echo "  [a64-smp] OK   x16/GICv3 lane: affinity SGIs acked 15/15"
else
    echo "  [a64-smp] FAIL x16/GICv3 lane: IPI 15/15 missing" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "[a64-smp] FAILED — log tails:" >&2
    tail -20 "$LOG" >&2
    tail -20 "$LOG16" >&2
    exit 1
fi
echo "[a64-smp] all assertions passed (v2 -smp 8 and GICv3 -smp 16)"
