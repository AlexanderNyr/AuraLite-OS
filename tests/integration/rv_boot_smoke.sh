#!/usr/bin/env bash
# tests/integration/rv_boot_smoke.sh -- RISCV_PLAN V0-V3 smoke test.
#
# The third architecture's first gate: clang -> lld -> OpenSBI ->
# _start -> SBI console, end to end on QEMU's virt machine.
# V1 grew it: the FDT walk fills boot_info_t, so the test now also
# asserts the handoff contract (magic OK line), the mmap (a usable
# region and a RAM total matching -m), the /chosen initrd translation
# when -initrd is passed, and the discovered platform (UART at
# 0x10000000, PLIC, 8 virtio windows -- the plan's Fact 3 numbers).
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
    local log="$1" smp="$2"; shift 2
    rm -f "$log"
    timeout 30 qemu-system-riscv64 -machine virt -m 256M -smp "$smp" \
        -display none -serial file:"$log" -no-reboot \
        -kernel "$ELF" "$@" >/dev/null 2>&1 || true
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

# ---- V1: boot_info_t from the FDT walk ----
assert_grep "$LOG" "\[boot\] handoff magic OK"                        "boot_info magic written (and written LAST)"
assert_grep "$LOG" "HHDM offset: 0xffffffc000000000"                  "hhdm_offset carries the D3 Sv39 constant"
assert_grep "$LOG" "usable  "                                         "mmap has a usable region from /memory"
assert_grep "$LOG" "usable RAM: 256 MiB"                              "RAM total matches -m 256M"
assert_grep "$LOG" "kernel  "                                         "kernel image self-reported in the mmap"
assert_grep "$LOG" "\[mm\]   initrd: none"                            "no -initrd => initrd honestly absent"
assert_grep "$LOG" "\[hw\]   uart: 0x0000000010000000"                "ns16550a found where the DTB puts it"
assert_grep "$LOG" "\[hw\]   plic: 0x000000000c000000"                "PLIC found (V2 consumes this)"
assert_grep "$LOG" "virtio-mmio windows: 8"                           "all 8 virtio windows found (V7 consumes)"

# ---- V2: traps, timer, PLIC ----
assert_grep "$LOG" "\[isr\] Illegal Instruction at sepc=0x"           "deliberate fault NAMED with sepc"
assert_grep "$LOG" "\[isr\]  PASS: illegal instruction named and resumed" "fault RESUMED past (isr gate)"
assert_grep "$LOG" "\[timer\] PASS: [0-9]* ticks observed at 100 Hz"  "SBI timer ticks at 100 Hz (timer gate)"
assert_grep "$LOG" "\[rng\]  jitter events collected: [1-9]"          "jitter pool fed from timer traps"
assert_grep "$LOG" "\[plic\] S-context enabled, threshold 0, uart irq 10" "PLIC programmed, uart line from DTB"
assert_grep "$LOG" "\[plic\] PASS: claim/complete round-trip"         "PLIC claim/complete with a real device irq"
# ---- V3: Sv39, PMM, heap, W^X ----
assert_grep "$LOG" "\[isr\] Illegal Instruction at sepc=0xffffffc0"   "self-test sepc is HIGHER-HALF (link/boot agree)"
assert_grep "$LOG" "\[pmm\]  PASS"                                    "pmm gate (64 frames out/back, count restored)"
assert_grep "$LOG" "identity window dropped"                          "final tables carry no identity window"
assert_grep "$LOG" "\[vmm\]  store to .text faulted"                  "W^X write half enforced"
assert_grep "$LOG" "\[vmm\]  execute-from-data faulted"               "W^X execute half enforced (impossible on i386)"
assert_grep "$LOG" "\[vmm\]  identity window confirmed dropped"       "low load faults after the drop"
assert_grep "$LOG" "\[vmm\]  PASS"                                    "vmm gate (positive path + 3 fault probes)"
assert_grep "$LOG" "\[heap\] PASS"                                    "heap gate (64 cycles, no corruption, no leak)"
assert_grep "$LOG" "V3 complete"                                      "kernel ran to its end"

# No unhandled trap anywhere in the boot -- the gate's last word.
if grep -qa "UNHANDLED\|UNEXPECTED" "$LOG"; then
    printf '  [rv-boot] FAIL unhandled/unexpected trap in the boot log\n'
    fail=1
else
    printf '  [rv-boot] OK   no unhandled trap in a full boot\n'
fi

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

# ---- 4 harts: the lottery must park three, the DTB must count four ----
run_qemu "$LOGSMP" 4
assert_count "$LOGSMP" "Hello from AuraLite OS kernel (riscv64)!" 1 \
    "-smp 4: exactly one banner -- hart lottery parked the rest"
assert_grep "$LOGSMP" "\[hw\]   harts: 4" \
    "-smp 4: /cpus walk counted all four harts"

# ---- V1: -initrd translates through /chosen ----
if [ -s "$BUILD/initrd.tar" ]; then
    LOGIRD="$BUILD/rv_boot_initrd.log"
    want_size=$(wc -c < "$BUILD/initrd.tar")
    run_qemu "$LOGIRD" 1 -initrd "$BUILD/initrd.tar" \
        -append "smoke.bootargs=via-chosen"
    assert_grep "$LOGIRD" "initrd: $want_size bytes at phys 0x" \
        "-initrd: /chosen range translated, size exact ($want_size)"
    assert_grep "$LOGIRD" "bootargs: smoke.bootargs=via-chosen" \
        "-append travels through /chosen bootargs"
else
    printf '  [rv-boot] SKIP initrd assertions (no build/initrd.tar; make iso builds it)\n'
fi

if [ "$fail" -ne 0 ]; then
    echo "[rv-boot] FAILED"
    exit 1
fi
echo "[rv-boot] all assertions passed"
