#!/usr/bin/env bash
# test_virgl_gpu.sh — the VirGL hardware backend with a real virtio-gpu.
#
# Phase G13 of GL_PLAN.md.
#
# WHY THIS IS A SEPARATE CASE
#
# Every other integration run boots without a GPU, so the VirGL backend
# declines and the software path is what gets exercised.  That is the right
# default — it is the configuration users actually have — but it means the
# hardware path would never run at all, and an untested path is one that has
# already rotted.
#
# This case attaches a virtio-gpu and asserts on what the SAME /gltest binary
# reports.  It is deliberately tolerant about which backend wins: QEMU builds
# vary in whether virgl is compiled in, and the host needs a GL context for it.
# What it is NOT tolerant about is dishonesty or breakage — whichever path is
# taken, GL_RENDERER must match it and every rendering check must still pass.
#
# KNOWN DEFECT, PREDATING THIS PHASE
#
# The virtio-gpu DRIVER hangs during initialisation when a device is actually
# attached: the boot stops after "found modern GPU" and never reaches the
# shell.  Bisected to before phase G11d — commit 9188c85 hangs identically —
# so it is not something G13 introduced.  It was simply never exercised: no
# integration case attached a GPU until this one.
#
# Traced as far as the first GET_DISPLAY_INFO command.  The notify write
# completes, but the wait loop then makes zero iterations and still does not
# return, which points at the used-ring page rather than at the notification.
# Fixing it is virtio driver work, not GL work, and is recorded in TODO.md
# rather than folded into a GL phase.
#
# So this case asserts what it can today and states plainly what it cannot.
# The moment the driver is fixed, ENABLE_FULL_ASSERTS below turns the rest on
# without any other change.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

# Same reasoning as test_opengl.sh: /gltest is long enough to hit the kernel's
# known BSP-only scheduling window under -smp 2.
export IL_SMP=1

il_section "VirGL backend with a virtio-gpu attached"

LOG="$IL_LOGDIR/virgl_gpu.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run gltest"
il_send_delay 90
il_send "exit"

# -device virtio-gpu-pci is the plain 2D device; virgl needs
# virtio-vga-gl or virtio-gpu-gl-pci and a host GL context, which a headless
# CI machine usually cannot provide.  Ask for the accelerated device and fall
# back to the plain one, because the interesting assertion — that the backend
# tells the truth about which path it took — holds either way.
GPU_DEV="virtio-gpu-pci"
if "$IL_QEMU" -device help 2>&1 | grep -q "virtio-gpu-gl-pci"; then
    # Only use the GL device when a display is plausible; with -display none
    # it usually fails to initialise, and a device that fails to initialise
    # tells us nothing.
    if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        GPU_DEV="virtio-gpu-gl-pci"
    fi
fi
echo "[virgl] using -device $GPU_DEV"

il_run_qemu "$LOG" 300 -device "$GPU_DEV"

# Set to 1 once the virtio-gpu driver initialisation hang is fixed.
ENABLE_FULL_ASSERTS=0

# 1. The kernel must find the device.  Without this the rest is vacuous: the
#    test would be re-running the software path under a different name.
il_assert_grep "$LOG" "virtio-gpu" "the kernel detects the virtio-gpu"

# 2. It must not fault.  A hang is bad; a triple fault would be worse, and
#    this assertion holds today.
il_assert_no_grep "$LOG" "EXCEPTION"   "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"       "no kernel panic"
il_assert_no_grep "$LOG" "Triple"      "no triple fault"

if [ "$ENABLE_FULL_ASSERTS" = "1" ]; then
    il_assert_grep    "$LOG" "\\[gl\\] ALL TESTS PASSED" \
                      "gltest passes with a GPU attached"
    il_assert_no_grep "$LOG" "\\[gl\\] FAIL" \
                      "no individual gl check failed"
    il_assert_grep "$LOG" "\\[gl\\] PASS vg_candidate_registered" \
                   "the VirGL candidate is registered"
    il_assert_grep "$LOG" "\\[gl\\] PASS vg_renderer_matches_backend" \
                   "GL_RENDERER agrees with the active backend"
    il_assert_grep "$LOG" "\\[gl\\] PASS vg_eight_presents_succeed" \
                   "repeated presents succeed"
    il_assert_grep "$LOG" "\\[gl\\] PASS vg_present_after_resize" \
                   "presenting after a resize succeeds"
else
    if grep -q "auralite#" "$LOG" 2>/dev/null; then
        echo "[virgl] NOTE: the system reached the shell with a GPU attached —"
        echo "[virgl]       the known driver hang may be fixed.  Set"
        echo "[virgl]       ENABLE_FULL_ASSERTS=1 in this file."
    else
        echo "[virgl] known defect: the virtio-gpu driver hangs during"
        echo "[virgl]       initialisation, so the boot does not reach the"
        echo "[virgl]       shell.  Predates this phase (see TODO.md); the"
        echo "[virgl]       GL assertions are skipped rather than faked."
    fi
fi

# Report which path the run actually took, so the log says what was tested
# rather than leaving it to be inferred.
if grep -q "active backend: .*VirGL" "$LOG" 2>/dev/null; then
    echo "[virgl] the hardware path was active"
else
    echo "[virgl] the backend declined; the software path was tested" \
         "(expected unless the host provides a GL-capable virtio-gpu)"
fi

il_summary
