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

# RESIDUE2 T8 (RES-02 FIXED): drive the shell once it is up.  A FIFO
# holds QEMU's stdin open; the script watches the serial log for the
# shell banner and only then types, so TCG-slow boots cannot race the
# input into the pre-prompt void (that race is what the old
# fixed-delay probes measured).
FIFO="$BUILD/cpumax_in.fifo"
rm -f "$FIFO"; mkfifo "$FIFO"
# -serial stdio, teed to the log: the file: chardev is output-only
# and silently DROPS stdin, so driving the shell needs stdio (RESIDUE2
# T8: the first version of this driver typed into the void).
timeout 150 qemu-system-x86_64 -cpu max \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -m 512M -display none -serial stdio -no-reboot \
        < "$FIFO" 2>/dev/null | tee "$LOG" >/dev/null &
QEMU_PID=$!
exec 3>"$FIFO"      # keep the write end open for the whole boot

# Wait (up to 90s) for the shell's first SYS_WRITE -- the line RES-02
# was named after.
shell_up=0
for _ in $(seq 1 90); do
    if grep -qa "Interactive Shell" "$LOG" 2>/dev/null; then shell_up=1; break; fi
    sleep 1
done
if [ "$shell_up" -eq 1 ]; then
    sleep 1
    printf 'uname\n' >&3
    sleep 4
    # The serial path can eat the FIRST character of a line (known
    # guest quirk, documented in the test-lib); type it twice so one
    # clean copy reaches the shell.
    printf 'uname\n' >&3
    sleep 8
fi
exec 3>&-           # close stdin: QEMU keeps running on its own
wait "$QEMU_PID" 2>/dev/null || true
rm -f "$FIFO"

assert_grep "Hello from AuraLite OS kernel!"                  "banner under -cpu max"
assert_grep "features: pat=1 pcid=0 invpcid=0 erms=1"         "receipt: ERMS present (and PCID still absent -- H0's fact holds)"
assert_grep "memcpy small-copy crossover: 0 (ERMS fast-string)" "the backend picked the ERMS threshold off the receipt"
# RESIDUE2 T8 (RES-02 root-caused and FIXED): the "first SYS_WRITE
# never lands under -cpu max" oddity is dead.  Feature-bisecting
# -cpu max against the qemu64 control pinned the culprit: SMAP.
# The boot shell's hand-rolled initial user stack (user.c) wrote its
# argc/argv frame from CPL0 with raw stores; under CR4.SMAP every
# store faulted and the init thread spun in an unkillable #PF loop
# (697k identical faults measured in a 30s boot) -- the shell never
# entered Ring 3, and the missing banner was simply the first
# receipt anyone noticed.  The frame write now runs inside an
# explicit user-access window (user_access_enable/disable); ERMS and
# x2APIC stay exonerated (R1's A/B boots), SMAP was the third
# suspect that stuck.  The receipts below are the FIX's gate: the
# banner (the exact line the ledger row named), the prompt, and a
# full uname round-trip under -cpu max.
assert_grep "Interactive Shell"                               "RES-02 FIXED: the shell banner (first SYS_WRITE) lands under -cpu max"
assert_grep "auralite#"                                       "shell prompt appears under -cpu max"
assert_grep "AuraLite OS 0.0.1 x86_64"                        "a full command round-trip works under -cpu max (uname)"
assert_grep "shell active; kmain idling"                      "boot reached the shell handoff with the 0-byte crossover live"
assert_no_grep "PANIC"                                        "no panic under -cpu max"

if [ "$fail" -eq 0 ]; then
    echo "[cpumax] all H2 assertions passed"
else
    echo "[cpumax] FAILED"
fi
exit "$fail"
