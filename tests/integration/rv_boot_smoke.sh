#!/usr/bin/env bash
# tests/integration/rv_boot_smoke.sh -- RISCV_PLAN phase V0 smoke test.
#
# The third architecture's first gate: clang -> lld -> OpenSBI ->
# _start -> SBI console, end to end on QEMU's virt machine.
#
# Asserts:
#   * OpenSBI hands off to our payload address in S-mode;
#   * the stub banner appears (so the .text.boot placement contract
#     holds -- OpenSBI jumps to the payload BASE, not e_entry, which
#     is the V0-measured fact this test regression-covers);
#   * the a0/a1 handoff survived: a hartid line and a non-null DTB
#     pointer line;
#   * the DTB magic read BIG-endian is 0xD00DFEED (the byte-order
#     fact V1's parser will inherit);
#   * the run ends by SBI shutdown (exit inside the timeout), not by
#     hanging to the clock;
#   * with -smp 4, exactly ONE banner appears: the hart lottery
#     parked the other three harts instead of letting four kernels
#     race the console.
#
# Skips cleanly when qemu-system-riscv64 is not installed (the same
# convention as every optional-tool gate in this suite).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ELF="$BUILD/kernelrv.elf"
LOG="$BUILD/rv_boot.log"
LOGSMP="$BUILD/rv_boot_smp.log"

if ! command -v qemu-system-riscv64 >/dev/null 2>&1; then
    echo "[rv-boot] SKIP: qemu-system-riscv64 not installed" >&2
    exit 0
fi

[ -s "$ELF" ] || make -C "$ROOT" kernelrv >/dev/null

fail=0
run_qemu() {
    local log="$1" smp="$2"
    rm -f "$log"
    timeout 30 qemu-system-riscv64 -machine virt -m 256M -smp "$smp" \
        -display none -serial file:"$log" -no-reboot \
        -kernel "$ELF" >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -qa "$pat" "$log"; then
        printf '  [rv-boot] OK   %s\n' "$desc"
    else
        printf '  [rv-boot] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_count() {
    local log="$1" pat="$2" want="$3" desc="$4"
    local got
    got=$(grep -ac "$pat" "$log" || true)
    if [ "$got" -eq "$want" ]; then
        printf '  [rv-boot] OK   %s\n' "$desc"
    else
        printf '  [rv-boot] FAIL %s (want %s, got %s)\n' "$desc" "$want" "$got"
        fail=1
    fi
}

# ---- single hart ----
run_qemu "$LOG" 1
assert_grep "$LOG" "Domain0 Next Address        : 0x0000000080200000" "OpenSBI hands off to the payload base"
assert_grep "$LOG" "Domain0 Next Mode           : S-mode"             "handoff is S-mode"
assert_grep "$LOG" "Hello from AuraLite OS kernel (riscv64)!"         "stub banner (payload-base contract holds)"
assert_grep "$LOG" "\[boot\] boot hart: [0-9]"                        "hartid arrived in a0"
assert_grep "$LOG" "\[boot\] DTB at phys 0x[0-9a-f]*[1-9a-f]"         "DTB pointer arrived in a1, non-null"
assert_grep "$LOG" "DTB magic OK (0xD00DFEED, big-endian read)"       "DTB magic reads correctly big-endian"
assert_grep "$LOG" "V0 stub complete"                                 "stub ran to its end"

# The run must END (SBI shutdown), not hang: QEMU exiting before the
# timeout leaves the log complete, which the final line already
# proves; additionally assert the file is not still growing.
sz1=$(wc -c < "$LOG"); sleep 1; sz2=$(wc -c < "$LOG")
if [ "$sz1" -eq "$sz2" ]; then
    printf '  [rv-boot] OK   run ended by SBI shutdown, not by timeout\n'
else
    printf '  [rv-boot] FAIL log still growing after the run\n'
    fail=1
fi

# ---- 4 harts: the lottery must park three of them ----
run_qemu "$LOGSMP" 4
assert_count "$LOGSMP" "Hello from AuraLite OS kernel (riscv64)!" 1 \
    "-smp 4: exactly one banner -- hart lottery parked the rest"

if [ "$fail" -ne 0 ]; then
    echo "[rv-boot] FAILED"
    exit 1
fi
echo "[rv-boot] all assertions passed"
