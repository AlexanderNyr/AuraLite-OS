#!/usr/bin/env bash
# test_perf_smoke.sh — the OPT_PLAN.md O0 measuring rig, exercised end to end.
#
# What this asserts (and, per OPT_PLAN D2, what it deliberately does not):
#   - the [perf] boot stamp appears and is under the absurdity ratchet
#     (60 s of PIT ticks) — a regression that slow is a hang, not noise;
#   - /proc/perf exists, is parseable, and every O0 counter is present;
#   - the membench table is produced to completion;
#   - the compositor takes no FULL redraws while the machine sits idle at
#     the shell (full_dirty is an init/theme/resize event, not a steady
#     state) — the dirty-rect path is allowed, the taskbar clock uses it;
#   - the numbers themselves are archived to the log for the plan's §6
#     table, NOT gated on: TCG throughput is weather, not a contract.
#
# Measured fact recorded during O0 bring-up: the default (BIOS/SeaBIOS)
# test boot has NO 32-bpp framebuffer — the BIOS Stage 2 sets no VBE mode,
# so gfx_init() bails on bpp!=32 and every compositor pixel counter reads
# 0 while the GUI runs dimensionless.  The same image under OVMF gets a
# GOP framebuffer and the pixel counters light up (composited ≈ 14× flipped
# — O4's headroom, measured).  This case therefore asserts counter
# PRESENCE and the frames_full rate bound, both of which hold on either
# firmware; pixel-delta assertions belong to O4's UEFI-booted gate.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "perf-smoke (OPT_PLAN O0)"

LOG="$IL_LOGDIR/perf_smoke.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# First /proc/perf read, 3 idle seconds, second read: the delta between the
# two "compositor_frames_full" values is the idle-full-redraw assertion.
il_send_delay 7
il_send "cat /proc/perf"
il_send_delay 3
il_send "cat /proc/perf"
il_send "cat /proc/loadavg"
il_send "run membench"
il_send_delay 30
il_send "echo perf-gate-end"
il_send_delay 2
il_send "exit"
il_run_qemu "$LOG" 75

# --- the boot stamp ---
il_assert_grep "$LOG" "\[perf\] boot-to-shell: [0-9]+ ticks"    "boot stamp printed"

BOOT_TICKS=$(grep -aoE '\[perf\] boot-to-shell: [0-9]+' "$LOG" | grep -oE '[0-9]+' | head -1)
if [ -n "${BOOT_TICKS:-}" ] && [ "$BOOT_TICKS" -lt 6000 ]; then
    il_pass "boot-to-shell under the 60 s absurdity ratchet ($BOOT_TICKS ticks)"
else
    il_fail "boot-to-shell ratchet: got '${BOOT_TICKS:-none}' ticks (limit 6000)"
fi

# --- /proc/perf: every O0 counter present ---
for c in boot_ticks_to_shell compositor_frames_full compositor_frames_partial \
         compositor_pixels_composited compositor_pixels_flipped \
         tlb_shootdowns_full tlb_shootdowns_ranged tlb_ipis_skipped \
         kmalloc_walk_steps kmalloc_class_hits uart_tx_sync_bytes \
         uart_tx_ring_bytes; do
    il_assert_grep "$LOG" "^$c [0-9]+" "/proc/perf lists $c"
done

# --- O7: the machine at an idle shell is IDLE (busy% first field of
# loadavg).  Pre-O7 the kmain yield-loop alone held this at ~36; the
# 15.0 ratchet is 40x above the post-O7 measurement (0.3) and half the
# regression signature — D2's "ratchet against absurdity" shape. ---
BUSY=$(grep -aE '^[0-9]+\.[0-9]+ [0-9]+\.[0-9]+' "$LOG" | head -1 | awk '{print $1}')
BUSY_INT=${BUSY%%.*}
if [ -n "${BUSY:-}" ] && [ "${BUSY_INT:-100}" -lt 15 ]; then
    il_pass "idle shell is idle (busy ${BUSY}%)"
else
    il_fail "idle shell is busy: ${BUSY:-unreadable}% (ratchet 15%)"
fi

# --- O3: the TX ring carries the log; the sync path is the exception ---
SYNC_B=$(grep -aE '^uart_tx_sync_bytes [0-9]+' "$LOG" | grep -oE '[0-9]+' | head -1)
RING_B=$(grep -aE '^uart_tx_ring_bytes [0-9]+' "$LOG" | grep -oE '[0-9]+' | head -1)
if [ -n "${SYNC_B:-}" ] && [ -n "${RING_B:-}" ] && [ "$RING_B" -gt "$SYNC_B" ]; then
    il_pass "TX ring carries the majority (ring=$RING_B sync=$SYNC_B bytes)"
else
    il_fail "TX ring not carrying: ring=${RING_B:-none} sync=${SYNC_B:-none}"
fi

# --- idle steady state: full recomposites bounded between the two reads ---
# One-shot full redraws are legitimate — the boot notification ("GUI
# subsystem initialized.", gui.c, 4000 ms) expires a few seconds into the
# run and its teardown sets full_dirty once.  The regression this guards
# against is a RATE: a compositor that re-renders fully per tick would add
# ~100 full frames per idle second (~300 in this window), not one.
FULL_VALS=$(grep -aE '^compositor_frames_full [0-9]+' "$LOG" | grep -oE '[0-9]+')
FULL_FIRST=$(echo "$FULL_VALS" | head -1)
FULL_LAST=$(echo "$FULL_VALS" | tail -1)
if [ -n "${FULL_FIRST:-}" ] && [ -n "${FULL_LAST:-}" ] && \
   [ $(( FULL_LAST - FULL_FIRST )) -le 1 ]; then
    il_pass "idle full recomposites bounded ($FULL_FIRST -> $FULL_LAST, one-shot events only)"
else
    il_fail "full recomposites while idle: ${FULL_FIRST:-none} -> ${FULL_LAST:-none} (limit: +1)"
fi

# --- HW_PLAN H0: the CPU feature receipts (every lane prints them) ---
il_assert_grep  "$LOG" "\[cpu\]   features: pat=[01] pcid=[01] invpcid=[01] erms=[01]" \
                                                "CPU feature receipt printed (H2/H3/H4 ground truth)"
il_assert_grep  "$LOG" "\[cpu\]   IA32_PAT = 0x" "IA32_PAT readback printed (H3's starting point)"

# --- membench ran to completion and produced the table ---
il_assert_grep  "$LOG" "MEMBENCH begin"        "membench started"
il_assert_count "$LOG" "MEMBENCH memcpy-a"  4  "aligned memcpy rows (4 sizes)"
il_assert_count "$LOG" "MEMBENCH memcpy-u"  4  "misaligned memcpy rows (4 sizes)"
il_assert_grep  "$LOG" "MEMBENCH memset-a"     "memset row"
il_assert_grep  "$LOG" "MEMBENCH-DONE"         "membench completed"
il_assert_grep  "$LOG" "perf-gate-end"         "shell survives the gate"

# --- O2: the fast-mode boot, recorded alongside (D2: recorded, not hard-gated
# beyond "not slower") ---
LOG_FAST="$IL_LOGDIR/perf_smoke_fast.log"
il_send_delay 6
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG_FAST" 30

BOOT_FULL="$BOOT_TICKS"
BOOT_FAST=$(grep -aoE '\[perf\] boot-to-shell: [0-9]+' "$LOG_FAST" | grep -oE '[0-9]+' | head -1)
if [ -n "${BOOT_FAST:-}" ] && [ -n "${BOOT_FULL:-}" ] && [ "$BOOT_FAST" -le "$BOOT_FULL" ]; then
    il_pass "fast boot not slower than full (full=$BOOT_FULL fast=$BOOT_FAST ticks)"
else
    il_fail "fast boot regressed past full: full=${BOOT_FULL:-none} fast=${BOOT_FAST:-none}"
fi

# --- the usual never-see markers ---
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "TRIPLE FAULT"        "no triple-fault"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no unhandled exception"

il_summary
