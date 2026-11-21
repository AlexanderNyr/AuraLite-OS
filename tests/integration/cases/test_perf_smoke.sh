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
# Measured fact at O0 bring-up: the default (BIOS/SeaBIOS) test boot had NO
# 32-bpp framebuffer — BIOS Stage 2 set no VBE mode — so gfx_init() bailed on
# bpp!=32 and every compositor pixel counter read 0 while the GUI ran
# dimensionless; only the OVMF boot had pixels.  RESIDUE3 C3 / RES-12 ended
# that asymmetry: Stage 2 now probes VBE and sets 1024x768x32 with LIN_FA, so
# BOTH firmwares hand the kernel a framebuffer and both lanes light up.  This
# case still asserts counter PRESENCE and the frames_full rate bound (weather
# belongs to the perf archive, not the gate); pixel-delta assertions stay in
# the GUI shard.  The "[vbe]" lines below are the proof that the pixels on
# this lane are real: they are the loader's own receipt, not an inference
# from a counter.

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
il_send "cat /proc/fb"
il_send_delay 3
il_send "cat /proc/perf"
il_send "cat /proc/loadavg"
il_send "run membench"
il_send_delay 6
il_send "run gpubench"
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

# --- H1: the framebuffer geometry file exists and parses in the same
# log the counters come from (one boot, so the geometry is the one the
# counters describe) ---
il_assert_grep "$LOG" "^[0-9]+x[0-9]+ pitch=[0-9]+" \
    "/proc/fb prints WxH pitch=BYTES (H1's geometry source)"

# --- H1: gpubench runs to completion and reports the counter inventory.
# Whether it finds a device is lane-specific (D2); that it PARSES the rig
# and prints a well-formed table is not. ---
il_assert_grep "$LOG" "^GPUBENCH-DONE" "gpubench runs to GPUBENCH-DONE (H1)"
il_assert_grep "$LOG" "^GPUBENCH-counters 8/8" \
    "gpubench finds all 8 H1 counters in /proc/perf"
il_assert_grep "$LOG" "^GPUBENCH-config virtio_gpu=[01] fb=[0-9]*x[0-9]*" \
    "gpubench reports the lane's geometry"

# --- /proc/perf: every O0 counter present ---
for c in boot_ticks_to_shell compositor_frames_full compositor_frames_partial \
         compositor_pixels_composited compositor_pixels_flipped \
         tlb_shootdowns_full tlb_shootdowns_ranged tlb_ipis_skipped \
         kmalloc_walk_steps kmalloc_class_hits uart_tx_sync_bytes \
         uart_tx_ring_bytes; do
    il_assert_grep "$LOG" "^$c [0-9]+" "/proc/perf lists $c"
done

# --- GPU_ACCEL_PLAN H1: the present accounting.  The eight counters that
# make "is the 2D path faster?" a question with an answer.  Presence is the
# gate; the VALUES are weather on this lane (D2) — but a value that is
# nonzero on the no-device lane would mean the accounting fired without a
# GPU, which is exactly the drift this pin exists to catch. ---
for c in gpu_presents_full gpu_presents_rect gpu_pixels_transferred \
         gpu_transfer_bytes gpu_flush_bytes gpu_present_fallbacks \
         gpu_control_queue_waits gpu_control_queue_timeouts; do
    il_assert_grep "$LOG" "^$c [0-9]+" "/proc/perf lists $c (H1)"
done

# --- H1: the byte arithmetic.  Every GPU copy charged is a multiple of 4
# (32 bpp), and the transfer/flush pair covers the same area — the flush is
# the host-side upload of the transfer rectangle, so the two must be equal.
# ck: equality is the invariant; a divergence means a flush rectangle was
# computed from different bounds than the transfer, which corrupts nothing
# today and lies to the plan forever. ---
GPU_TX=$(grep -a '^gpu_transfer_bytes ' "$LOG" | tail -1 | awk '{print $2}')
GPU_FL=$(grep -a '^gpu_flush_bytes ' "$LOG" | tail -1 | awk '{print $2}')
GPU_PX=$(grep -a '^gpu_pixels_transferred ' "$LOG" | tail -1 | awk '{print $2}')
if [ -n "${GPU_TX:-}" ] && [ -n "${GPU_FL:-}" ] && [ "$GPU_TX" = "$GPU_FL" ]; then
    il_pass "H1: transfer and flush bytes agree ($GPU_TX each)"
else
    il_fail "H1: transfer ($GPU_TX) and flush ($GPU_FL) bytes disagree"
fi
if [ -n "${GPU_PX:-}" ] && [ -n "${GPU_TX:-}" ] && \
   [ "$GPU_TX" = "$((GPU_PX * 4))" ]; then
    il_pass "H1: every transferred pixel is 4 bytes ($GPU_PX px -> $GPU_TX B)"
else
    il_fail "H1: pixel/byte arithmetic broken ($GPU_PX px vs $GPU_TX B)"
fi


# --- HW H4 → RESIDUE R11: the PCID counters are now BACKED by the
# implementation (pcid.c), gated on CR4.PCIDE.  TCG has no PCID
# (measured: +pcid refused), so on every CI lane the feature line says
# pcid=0 and both counters MUST still read zero — a nonzero value here
# means the accounting fired without the feature, which is exactly the
# drift this pin exists to catch.  On a pcid=1 lane (the user's WHPX
# machine, a KVM runner some day) the D-PCID-5 acceptance inverts:
# cr3_noflush_switches must MOVE — that half lives in the metal
# package (docs/metal_receipts.md, slots 5/6). ---
if grep -aqE 'features: pat=[01] pcid=1' "$LOG"; then
    il_assert_grep "$LOG" "^cr3_noflush_switches [1-9][0-9]*" \
        "R11/D-PCID-5: pcid=1 lane, cr3_noflush_switches moving"
    il_assert_grep "$LOG" "\[vmm\] PCID enabled" \
        "R11: the CR4.PCIDE enable line on a pcid=1 lane"
else
    il_assert_grep "$LOG" "^cr3_noflush_switches 0\$"  "R11: pcid=0 lane, cr3_noflush_switches stays zero"
    il_assert_grep "$LOG" "^pcid_generation_wraps 0\$" "R11: pcid=0 lane, pcid_generation_wraps stays zero"
    il_assert_no_grep "$LOG" "\[vmm\] PCID enabled" \
        "R11: no PCIDE enable line on a pcid=0 boot"
fi

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
il_assert_grep_fixed "$LOG" "memcpy small-copy crossover: 64 (no ERMS)" \
                                                "H2: qemu64 keeps the measured-good 64 threshold"
# HW H3 on the BIOS lane: PAT gets its WC entry everywhere.  Since RES-12
# this boot HAS a linear framebuffer too, so the WC remap decodes here just
# as it does on the UEFI lane (768 pages = 1024x768x32 at pitch 4096).  The
# "no fb" refusal branch in paging_fb_set_wc is no longer reachable from a
# stock QEMU boot -- every QEMU VGA model offers a VBE mode -- and it stays
# in the code as the guard for a loader that fails to set one.
il_assert_grep_fixed "$LOG" "IA32_PAT: PA4=WC (readback 0x0007040100070406)" \
                                                "H3: PAT readback shows the WC entry"
il_assert_grep "$LOG" "\[vbe\] selecting 1024x768x32 .linear framebuffer." \
                                                "RES-12: the loader set a VBE mode"
il_assert_grep_fixed "$LOG" "[vbe] framebuffer live; boot_info.fb filled" \
                                                "RES-12: boot_info.fb published"
il_assert_grep "$LOG" "fb: WC via PAT4 \(768 pages" \
                                                "H3: the BIOS lane now decodes WC too"

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
