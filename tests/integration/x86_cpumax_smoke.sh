#!/usr/bin/env bash
# tests/integration/x86_cpumax_smoke.sh -- HW_PLAN H2.
#
# The `-cpu max` lane: the one TCG configuration that exposes ERMS
# (measured in H0; qemu64 has none).  Asserts the feature receipt
# says erms=1, that the rep-string backend picked the 0-byte
# crossover off it (the ERMSB fast-string contract), and that the
# boot still reaches the shell -- a feature-gated threshold that
# breaks the boot would otherwise hide behind the qemu64-only lanes.
#
# The PERFORMANCE half of ERMSB stays a metal receipt (plan §6): TCG
# emulates rep-string one iteration at a time regardless, so the
# threshold's wall-clock effect is not measurable here -- this lane
# proves the DETECTION and the WIRING, which is what TCG can prove.
#
# Skips cleanly without qemu-system-x86_64 or the ISO.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG="$BUILD/x86_cpumax.log"

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "[cpumax] SKIP: qemu-system-x86_64 not installed" >&2
    exit 0
fi
[ -s "$ISO" ] || { echo "[cpumax] SKIP: build/auralite.iso absent (make iso)" >&2; exit 0; }

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [cpumax] OK   %s\n' "$desc"
    else
        printf '  [cpumax] FAIL %s\n' "$desc"
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [cpumax] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [cpumax] OK   %s\n' "$desc"
    fi
}

rm -f "$LOG"
timeout 150 qemu-system-x86_64 -cpu max \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M -display none -serial file:"$LOG" -no-reboot \
        >/dev/null 2>&1 || true

assert_grep "Hello from AuraLite OS kernel!"                  "banner under -cpu max"
assert_grep "features: pat=1 pcid=0 invpcid=0 erms=1"         "receipt: ERMS present (and PCID still absent -- H0's fact holds)"
assert_grep "memcpy small-copy crossover: 0 (ERMS fast-string)" "the backend picked the ERMS threshold off the receipt"
# The kernel-side healthy end, NOT the userspace prompt: measured
# during this phase (control on the pre-H2 kernel, identical), the
# interactive shell's banner does not appear on the serial log under
# -cpu max even though the shell process starts -- a pre-existing
# -cpu max oddity recorded in HW_PLAN H2's result, not this phase's
# regression (the control run proves it).  RES-02 (residue ledger)
# narrowed it further: the shell PROCESS starts (kernel receipts
# print, boot-to-shell tick prints) but its first SYS_WRITE never
# reaches serial; two suspects EXONERATED by A/B boots --
# qemu64,+erms shows the banner (ERMS/string_fast innocent) and
# max,-x2apic still hides it (x2APIC innocent).  Still open.  The kernel reaching
# "shell active" with the 0-byte crossover live is this lane's
# actual claim.
assert_grep "shell active; kmain idling"                      "boot reached the shell handoff with the 0-byte crossover live"
assert_no_grep "PANIC"                                        "no panic under -cpu max"

if [ "$fail" -eq 0 ]; then
    echo "[cpumax] all H2 assertions passed"
else
    echo "[cpumax] FAILED"
fi
exit "$fail"
