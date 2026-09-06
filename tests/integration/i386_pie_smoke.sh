#!/usr/bin/env bash
# tests/integration/i386_pie_smoke.sh -- RESIDUE2 T8 (RES-18) gate.
#
# "A PIE binary runs on one tenant; receipt" -- the ledger row's named
# deliverable.  /bin32/pie32 is a static PIE (-fPIE + ld.lld -pie:
# ET_DYN, .rel.dyn R_386_RELATIVE).  The i386 tenant's elf32load seats
# it at 0x10000000 and applies the relocations as its own dynamic
# linker.  This smoke drives the i386 shell to `run bin32/pie32` and
# pins BOTH halves of the receipt:
#
#   * the LOADER line: base 0x10000000, 3 R_386_RELATIVE applied;
#   * the PROGRAM line: its own runtime address, which can only be
#     correct if relocation actually happened (an unrelocated pointer
#     aims at the link-time image base 0), and the program's own
#     pointer-table self-check (a bad table mutates the receipt
#     prefix from "PIE32:" to "PIE!:").
#
# Standing no-regression: the ET_EXEC path (bin32/init32) still loads.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG="$BUILD/i386_pie.log"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-pie] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-pie] OK   %s\n' "$desc"
    else
        printf '  [i386-pie] FAIL %s\n' "$desc"
        fail=1
    fi
}
assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-pie] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-pie] OK   %s\n' "$desc"
    fi
}

rm -f "$LOG"
{
    sleep 25                       # TCG boots through every phase first
    printf 'run bin32/pie32\n';    sleep 5
    printf 'run bin32/init32\n';   sleep 4
    printf 'exit\n';               sleep 2
} | timeout 60 qemu-system-i386 \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M \
        -display none -serial stdio -no-reboot \
        > "$LOG" 2>/dev/null || true

assert_grep    "$LOG" "\[elf32\] PIE: base 0x10000000, 3 R_386_RELATIVE applied" \
    "loader accepted the ET_DYN image and applied all 3 relocations"
assert_grep    "$LOG" "PIE32: running from a RELOCATED base @ 0x1000" \
    "the program runs from its RELOCATED base (address proves it)"
assert_grep    "$LOG" "PIE32: running" \
    "pointer-table self-check passed (a broken table says PIE!:)"
assert_grep    "$LOG" "init32: exiting 7" \
    "no regression: the ET_EXEC path still loads (init32)"
assert_no_grep "$LOG" "UNHANDLED EXCEPTION" \
    "no faults: bad relocation would page-fault immediately"
assert_no_grep "$LOG" "\[elf32\] refused" \
    "loader refused nothing in the session"

if [ "$fail" -eq 0 ]; then
    echo "[i386-pie] all RES-18 assertions passed"
else
    echo "[i386-pie] FAILED"
fi
exit "$fail"
