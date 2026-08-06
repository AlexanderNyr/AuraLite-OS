#!/usr/bin/env bash
# test_rng.sh — INTERNET_PLAN.md phase N0 test gate.
#
# The kernel CSPRNG must:
#   1. detect and USE RDRAND/RDSEED when the CPU provides them
#      (IL_CPU="qemu64,+rdrand,+rdseed");
#   2. without hardware RNG, fall back to the interrupt-jitter pool, LOG
#      the estimated entropy, and still reach a seeded, self-tested state;
#   3. two separate boots produce DIFFERENT output (the old TSC/LCG filler
#      did not reliably manage even this).
#
# The statistical gate (1 MiB byte-frequency + bit-runs) runs host-side in
# tests/unit/test_rng.c against the identical rng_core.h; over the serial
# console we assert the kernel's own 16 KiB self-test instead.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

trap il_dump_on_error EXIT

# ---------------------------------------------------------------- section 1
il_section "N0: hardware RNG path (RDRAND + RDSEED)"

LOG1="$IL_LOGDIR/rng_hw.log"
IL_LAST_LOG="$LOG1"
IL_CPU="qemu64,+rdrand,+rdseed" il_run_qemu "$LOG1" 40

il_assert_grep "$LOG1" "\[rng\] CPU features: RDRAND=yes RDSEED=yes" \
               "RDSEED and RDRAND detected via CPUID"
il_assert_grep "$LOG1" "\[rng\] seeded from RDSEED" \
               "DRBG seeded from RDSEED"
il_assert_grep "$LOG1" "\[rng\] self-test: PASS" \
               "boot self-test (byte-frequency + bit-runs) passes"
il_assert_grep "$LOG1" "\[rng\] sample: [0-9a-f]{64}" \
               "32-byte boot sample printed"
il_assert_no_grep "$LOG1" "PANIC" "no kernel panic"

# ---------------------------------------------------------------- section 2
il_section "N0: fallback path (no hardware RNG, interrupt jitter)"

LOG2="$IL_LOGDIR/rng_jitter.log"
IL_LAST_LOG="$LOG2"
il_run_qemu "$LOG2" 45        # default IL_CPU=qemu64: no RDRAND, no RDSEED

il_assert_grep "$LOG2" "\[rng\] CPU features: RDRAND=no RDSEED=no" \
               "hardware RNG correctly reported absent"
il_assert_grep "$LOG2" "no usable hardware RNG; falling back" \
               "fallback to the interrupt-jitter pool announced"
il_assert_grep "$LOG2" "\[rng\] pool: [0-9]+ samples, est\. [0-9]+ bits \(threshold 128\)" \
               "estimated entropy is logged at boot, not silently assumed"
il_assert_grep "$LOG2" "\[rng\] seeded from interrupt-jitter pool" \
               "DRBG seeded once the pool crossed the threshold"
il_assert_grep "$LOG2" "\[rng\] self-test: PASS" \
               "boot self-test passes on the fallback path"
il_assert_grep "$LOG2" "\[rng\] sample: [0-9a-f]{64}" \
               "32-byte boot sample printed"
il_assert_no_grep "$LOG2" "PANIC" "no kernel panic"

# ---------------------------------------------------------------- section 3
il_section "N0: two boots differ"

LOG3="$IL_LOGDIR/rng_jitter2.log"
IL_LAST_LOG="$LOG3"
il_run_qemu "$LOG3" 45        # second plain-qemu64 boot

s1="$(grep -oE '\[rng\] sample: [0-9a-f]{64}' "$LOG2" | head -1 | awk '{print $3}')"
s2="$(grep -oE '\[rng\] sample: [0-9a-f]{64}' "$LOG3" | head -1 | awk '{print $3}')"

if [ -n "$s1" ] && [ -n "$s2" ]; then
    il_pass "both boots produced a sample"
else
    il_fail "a boot produced no sample (s1='$s1' s2='$s2')"
fi
if [ -n "$s1" ] && [ -n "$s2" ] && [ "$s1" != "$s2" ]; then
    il_pass "two boots produce different CSPRNG output"
else
    il_fail "two boots produced IDENTICAL output ($s1)"
fi

il_summary
