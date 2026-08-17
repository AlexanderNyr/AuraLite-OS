#!/usr/bin/env bash
# tests/integration/a64_boot_smoke.sh -- ARM64_PLAN A0-A1 smoke test.
#
# The fourth architecture's first gate: clang -> lld -> QEMU ELF load
# -> EL1 _start -> PL011 -> PSCI power-off, end to end on QEMU's virt
# machine.
# A1 grew it: the SHARED DTB walker (kernel/dt/fdt.c -- the same
# object the riscv64 kernel links) fills boot_info_t, so the test now
# also asserts the handoff contract (magic OK line), the mmap (usable
# RAM matching -m), the discovered platform (PL011 at 0x9000000 with
# its NORMALISED INTID 33 = SPI 1 + 32, GICD/GICC, all 32 virtio
# windows at INTIDs 48..79, the PSCI conduit assert), the /chosen
# initrd translation when -initrd is passed, and -smp 4 discovering
# 4 CPUs from /cpus.
#
# Every assertion regression-covers a fact MEASURED during the plan's
# fact-finding (ARM64_PLAN.md section 1), so a QEMU behaviour change
# announces itself as a named red line, not a mystery:
#   * the stub banner appears (the whole toolchain path holds);
#   * CurrentEL is EL1 (Fact 2.1: ELF -kernel enters at EL1);
#   * x0 at entry is 0 -- NOT the DTB pointer (Fact 2.2's first half:
#     the Linux Image protocol's x0 promise does not apply to ELF
#     payloads; the day it starts applying, THIS line names it);
#   * the DTB magic is found at the RAM base 0x40000000 (Fact 2.2's
#     second half -- the address every later phase stands on);
#   * CNTFRQ_EL0 reads 62500000 (Fact 2.3: frequency is a register);
#   * the run ends by PSCI power-off (exit inside the timeout), not
#     by hanging to the clock (Fact 4);
#   * with -smp 4, exactly ONE banner appears (D5: assert, don't
#     assume, even when the assumption is documented QEMU behaviour);
#   * under -machine virt,virtualization=on the kernel REFUSES with
#     the D1 banner instead of limping along at EL2.
#
# Skips cleanly when qemu-system-aarch64 is not installed (the same
# convention as every optional-tool gate in this suite).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ELF="$BUILD/kernela64.elf"
LOG="$BUILD/a64_boot.log"
LOGSMP="$BUILD/a64_boot_smp.log"
LOGEL2="$BUILD/a64_boot_el2.log"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[a64-boot] SKIP: qemu-system-aarch64 not installed" >&2
    exit 0
fi

[ -s "$ELF" ] || make -C "$ROOT" kernela64 >/dev/null

fail=0
run_qemu() {
    local log="$1" smp="$2" machine="$3"; shift 3
    rm -f "$log"
    timeout 30 qemu-system-aarch64 -machine "$machine" -cpu cortex-a72 \
        -m 256M -smp "$smp" \
        -display none -serial file:"$log" -no-reboot \
        -kernel "$ELF" "$@" >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -qa "$pat" "$log"; then
        printf '  [a64-boot] OK   %s\n' "$desc"
    else
        printf '  [a64-boot] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_count() {
    local log="$1" pat="$2" want="$3" desc="$4"
    local got
    got=$(grep -ac "$pat" "$log" || true)
    if [ "$got" -eq "$want" ]; then
        printf '  [a64-boot] OK   %s\n' "$desc"
    else
        printf '  [a64-boot] FAIL %s (want %s, got %s)\n' "$desc" "$want" "$got"
        fail=1
    fi
}

# ---- single CPU: the banner and every measured-fact echo ----
start=$(date +%s)
run_qemu "$LOG" 1 virt
elapsed=$(( $(date +%s) - start ))

assert_grep "$LOG" "Hello from AuraLite OS kernel (aarch64)!"      "stub banner (clang -> lld -> QEMU -> EL1 path holds)"
assert_grep "$LOG" "\[boot\] CurrentEL: EL1"                       "entered at EL1 (Fact 2.1)"
assert_grep "$LOG" "x0 at entry: 0x0000000000000000"               "x0 is 0, not the DTB pointer (Fact 2.2)"
assert_grep "$LOG" "DTB probe at RAM base 0x0000000040000000: magic 0x00000000D00DFEED OK" \
                                                                   "DTB magic found at the RAM base (Fact 2.2)"
assert_grep "$LOG" "CNTFRQ_EL0: 62500000 Hz"                       "timer frequency read from the register (Fact 2.3)"
assert_grep "$LOG" "A1 complete; powering off via PSCI"            "reached the healthy end of the phase"

# PSCI power-off, not the timeout: the run must end well inside the
# 30 s budget (generous bound; a hang eats all 30).
if [ "$elapsed" -lt 15 ]; then
    printf '  [a64-boot] OK   run ended by PSCI power-off (%ss), not the timeout (Fact 4)\n' "$elapsed"
else
    printf '  [a64-boot] FAIL run took %ss -- PSCI SYSTEM_OFF did not end it\n' "$elapsed"
    fail=1
fi

# ---- A1: the shared walker fills boot_info_t ----
assert_grep "$LOG" "handoff magic OK, path=PSCI, boot_info filled from DTB" \
                                                                   "boot_info handoff contract (magic written LAST)"
assert_grep "$LOG" "usable RAM: 256 MiB"                           "mmap usable RAM matches -m 256M"
assert_grep "$LOG" "uart: 0x0000000009000000 irq 33"               "PL011 found with NORMALISED INTID 33 (SPI 1 + 32 -- the A1 off-by-32 rule)"
assert_grep "$LOG" "gicd: 0x0000000008000000 gicc: 0x0000000008010000" \
                                                                   "GICv2 pair discovered (per-depth device state: v2m child did not wipe it)"
assert_grep "$LOG" "virtio-mmio windows: 32 (irq 48..79)"          "all 32 virtio windows, INTIDs normalised 48..79 (A7 consumes)"
assert_grep "$LOG" "psci: method hvc (matches psci.c's conduit)"   "PSCI conduit asserted against the tree, not assumed (D2)"
assert_grep "$LOG" "initrd: none"                                  "no -initrd => honestly reported absent, not faked"

# ---- A1 measured deviation: QEMU does NOT load -initrd for ELF
# payloads on this machine (measured in A1: no /chosen properties
# written AND the payload bytes absent from RAM -- the Linux-Image
# boot path is what activates initrd loading, same family as the
# x0-is-not-the-DTB fact).  The walker's /chosen code is real and the
# riscv64 suite exercises it every push; THIS assert pins the QEMU
# behaviour so the day it changes, the gate names it.  A5 owns the
# exit ramp (a raw-Image boot path for the initrd-carrying boots). ----
IRD="$BUILD/a64_smoke_initrd.bin"
LOGIRD="$BUILD/a64_boot_initrd.log"
printf 'AuraLite a64 smoke initrd payload' > "$IRD"
run_qemu "$LOGIRD" 1 virt -initrd "$IRD"
assert_grep "$LOGIRD" "initrd: none"                               "-initrd with an ELF payload is IGNORED by QEMU (measured A1 fact, pinned)"

# ---- -smp 4: exactly one banner (D5), and /cpus counted ----
run_qemu "$LOGSMP" 4 virt
assert_count "$LOGSMP" "Hello from AuraLite OS kernel (aarch64)!" 1 \
    "-smp 4 prints the banner exactly once (D5: assert, don't assume)"
assert_grep "$LOGSMP" "cpus: 4 (boot cpu 0)"                       "-smp 4 => 4 CPUs discovered from /cpus"

# ---- virtualization=on: the D1 refusal, not an EL2 limp-along ----
run_qemu "$LOGEL2" 1 virt,virtualization=on
assert_grep "$LOGEL2" "CurrentEL != EL1; refusing to boot (ARM64_PLAN D1)" \
    "EL2 entry gets the honest refusal banner (D1)"
assert_count "$LOGEL2" "Hello from AuraLite OS kernel (aarch64)!" 0 \
    "no kernel banner on the refusal path"

if [ "$fail" -ne 0 ]; then
    echo "[a64-boot] FAILURES above; log: $LOG" >&2
    exit 1
fi
echo "[a64-boot] all assertions passed"
