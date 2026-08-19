#!/usr/bin/env bash
# test_selftest_modes.sh — the OPT_PLAN.md O2 self-test knob, all three
# positions.
#
# The contract this asserts:
#   full — byte-compatible with the historical boot: the 1-second PIT
#          verification line, the 1000-frame PMM gauntlet, the
#          10000-cycle heap gauntlet, the 16 KiB RNG analysis, and every
#          PASS line the pre-O2 tree printed (test_boot_to_shell's greps
#          continue to hold because the lib pins CI boots to full).
#   fast — every scaled test still RUNS and still prints PASS, at the
#          reduced sizes (100 ms / 100 frames / 500 cycles / 2 KiB).
#   off  — the scaled tests print SKIPPED, and the boot still reaches
#          the interactive shell (off trades proof for speed, not
#          correctness of the boot itself).
#
# Plus the D1 point of the whole phase: the fast boot must actually be
# faster than the full boot, by at least the 1-second PIT window it no
# longer spends — asserted via the [perf] boot stamp, in ticks, on the
# same machine, in the same run.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "selftest modes (OPT_PLAN O2)"

LOG_FULL="$IL_LOGDIR/selftest_full.log"
LOG_FAST="$IL_LOGDIR/selftest_fast.log"
LOG_OFF="$IL_LOGDIR/selftest_off.log"
trap il_dump_on_error EXIT

# ---- boot 1: full ----
IL_LAST_LOG="$LOG_FULL"
il_send_delay 6
il_send "exit"
IL_SELFTEST=full il_run_qemu "$LOG_FULL" 30

il_assert_grep "$LOG_FULL" "\[selftest\] mode: full \(fw_cfg\)"        "full: mode line names fw_cfg"
il_assert_grep "$LOG_FULL" "measuring 1-second delay"                  "full: the 1 s PIT window"
il_assert_grep "$LOG_FULL" "self-test: allocating 1000 frames"         "full: 1000-frame PMM gauntlet"
il_assert_grep "$LOG_FULL" "self-test: 10000 alloc/free cycles"        "full: 10000-cycle heap gauntlet"
il_assert_grep "$LOG_FULL" "PASS \(16 KiB, byte-frequency"             "full: 16 KiB RNG analysis"
il_assert_grep "$LOG_FULL" "\[pmm\] PASS:"                             "full: PMM PASS"
il_assert_grep "$LOG_FULL" "\[heap\] PASS:"                            "full: heap PASS"
il_assert_grep "$LOG_FULL" "\[timer\] PASS:.*in 1s"                    "full: timer PASS, 1 s form"
il_assert_grep "$LOG_FULL" "auralite#"                                 "full: reaches the shell"

# ---- boot 2: fast ----
IL_LAST_LOG="$LOG_FAST"
il_send_delay 6
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG_FAST" 30

il_assert_grep "$LOG_FAST" "\[selftest\] mode: fast \(fw_cfg\)"        "fast: mode line names fw_cfg"
il_assert_grep "$LOG_FAST" "measuring 100-ms delay"                    "fast: the 100 ms PIT window"
il_assert_grep "$LOG_FAST" "self-test: allocating 100 frames"          "fast: 100-frame PMM gauntlet"
il_assert_grep "$LOG_FAST" "self-test: 500 alloc/free cycles"          "fast: 500-cycle heap gauntlet"
il_assert_grep "$LOG_FAST" "PASS \(2 KiB, byte-frequency"              "fast: 2 KiB RNG analysis"
il_assert_grep "$LOG_FAST" "\[pmm\] PASS:"                             "fast: PMM still PASSes"
il_assert_grep "$LOG_FAST" "\[heap\] PASS:"                            "fast: heap still PASSes"
il_assert_grep "$LOG_FAST" "\[timer\] PASS:.*in 100ms"                 "fast: timer PASS, 100 ms form"
il_assert_grep "$LOG_FAST" "auralite#"                                 "fast: reaches the shell"

# ---- boot 3: off ----
IL_LAST_LOG="$LOG_OFF"
il_send_delay 6
il_send "exit"
IL_SELFTEST=off il_run_qemu "$LOG_OFF" 30

il_assert_grep "$LOG_OFF" "\[selftest\] mode: off \(fw_cfg\)"          "off: mode line names fw_cfg"
il_assert_grep "$LOG_OFF" "\[pmm\] self-test: SKIPPED \(selftest=off\)"   "off: PMM skipped loudly"
il_assert_grep "$LOG_OFF" "\[heap\] self-test: SKIPPED \(selftest=off\)"  "off: heap skipped loudly"
il_assert_grep "$LOG_OFF" "\[timer\] self-test: SKIPPED \(selftest=off\)" "off: timer skipped loudly"
il_assert_grep "$LOG_OFF" "\[rng\] self-test: SKIPPED \(selftest=off\)"   "off: RNG analysis skipped loudly"
il_assert_grep "$LOG_OFF" "auralite#"                                  "off: still reaches the shell"

# ---- the D1 assertion: fast is measurably faster than full ----
ticks_of() { grep -aoE '\[perf\] boot-to-shell: [0-9]+' "$1" | grep -oE '[0-9]+' | head -1; }
T_FULL=$(ticks_of "$LOG_FULL")
T_FAST=$(ticks_of "$LOG_FAST")
if [ -n "${T_FULL:-}" ] && [ -n "${T_FAST:-}" ] && \
   [ $(( T_FULL - T_FAST )) -ge 80 ]; then
    il_pass "fast boot beats full by >= 80 ticks (full=$T_FULL fast=$T_FAST)"
else
    il_fail "fast boot not measurably faster: full=${T_FULL:-none} fast=${T_FAST:-none} (need >= 80 tick gap)"
fi

# ---- never-see markers, all three boots ----
for l in "$LOG_FULL" "$LOG_FAST" "$LOG_OFF"; do
    il_assert_no_grep "$l" "PANIC"               "no panic ($(basename "$l"))"
    il_assert_no_grep "$l" "TRIPLE FAULT"        "no triple-fault ($(basename "$l"))"
done

il_summary
