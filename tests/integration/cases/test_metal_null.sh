#!/usr/bin/env bash
# test_metal_null.sh — the R11 metal package's QEMU NULL test.
#
# RESIDUE_PLAN R11's exit line: "the package exists and runs under
# QEMU as a NULL test; metal numbers arrive when the user runs it".
# This case IS that null run, through the same integration harness as
# every other case: boot the canonical image, then assert that every
# BOOT-TIME receipt line docs/metal_receipts.md asks the user to
# capture still prints, and that the PCID accounting is silent on a
# pcid=0 boot (TCG has no PCID — measured this phase, `+pcid` is
# refused with a warning — so a nonzero counter here would mean the
# accounting fired without the feature).  tools/metal_receipts.sh
# --null-test is the user-facing spelling of the same rehearsal.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "metal-package NULL test (RESIDUE R11)"

LOG="$IL_LOGDIR/metal_null.log"
IL_LAST_LOG="$LOG"
trap 'il_dump_on_error' EXIT

# The package pieces exist before anything boots (cwd is
# tests/integration; the tree root is two levels up).  Presence, not
# the exec bit: the R11 deploy measured that patch(1) drops mode
# headers git-apply honours, so a 644 checkout is a legal state —
# everything in this tree runs the scripts through bash anyway.
if [ -f ../../tools/metal_receipts.sh ]; then
    il_pass "package script exists (tools/metal_receipts.sh; bash-run, exec bit not required)"
else
    il_fail "package script missing (tools/metal_receipts.sh)"
fi
if grep -q "cr3_noflush_switches" ../../docs/metal_receipts.md 2>/dev/null; then
    il_pass "paste-back doc carries the PCID slots (docs/metal_receipts.md)"
else
    il_fail "paste-back doc missing the PCID slots"
fi

il_send_delay 25
il_send 'cat /proc/perf'
il_send_delay 3
il_send 'exit'
il_run_qemu "$LOG" 45

# Slot 1: feature ground truth + PAT readback.
il_assert_grep "$LOG" "\[cpu\]   features: pat=[01] pcid=[01] invpcid=[01] erms=[01]" \
    "slot 1: feature ground-truth line prints"
il_assert_grep "$LOG" "\[vmm\] IA32_PAT: PA4=WC" \
    "slot 1: PAT readback line prints"

# Slot 2: ERMSB crossover.
il_assert_grep "$LOG" "memcpy small-copy crossover" \
    "slot 2: crossover line prints"

# Slot 7 (NEW this phase): MADT discovery — QEMU must AGREE with the
# hardcode; the interesting machines disagree, and get named.
il_assert_grep_fixed "$LOG" "[ioapic] base 0xfec00000 (MADT agree)" \
    "slot 7: MADT discovery line, QEMU agrees"

# Slots 5/6 in their pcid=0 fallback: counters exist through
# /proc/perf, read zero, and the enable line is absent.
il_assert_grep "$LOG" "^cr3_noflush_switches 0\$" \
    "slot 6 fallback: cr3_noflush_switches exists, 0 on pcid=0"
il_assert_grep "$LOG" "^pcid_generation_wraps 0\$" \
    "slot 6 fallback: pcid_generation_wraps exists, 0 on pcid=0"
il_assert_no_grep "$LOG" "\[vmm\] PCID enabled" \
    "slot 5 fallback: no PCIDE enable line on a pcid=0 boot"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"

il_summary
