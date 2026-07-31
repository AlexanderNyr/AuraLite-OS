#!/usr/bin/env bash
# test_opengl.sh — OpenGL stack regression (GL_PLAN.md).
#
# Runs /gltest, which exercises the bulk-pixel presentation path used by the
# whole GL stack plus the negative cases that the kernel must reject without
# faulting.  Phase G0 covers the ag_blit path; later phases extend /gltest with
# context, matrix and rasterizer checks, and this case picks them up for free.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "OpenGL stack (/gltest)"

LOG="$IL_LOGDIR/opengl.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run /gltest"
il_send_delay 12
il_send "exit"

il_run_qemu "$LOG" 60

# The test program prints its own verdict.
il_assert_grep    "$LOG" "\\[gl\\] ALL TESTS PASSED"      "gltest reports all checks passed"
il_assert_no_grep "$LOG" "\\[gl\\] FAIL"                  "no individual gl check failed"

# Presentation path must work.
il_assert_grep    "$LOG" "\\[gl\\] PASS blit_basic"       "bulk pixel blit works"
il_assert_grep    "$LOG" "\\[gl\\] PASS blit_alpha"       "alpha blit works"
il_assert_grep    "$LOG" "\\[gl\\] PASS blit_clip_negative" "off-window blit is clipped"

# Hostile pointers must be refused rather than crashing the kernel.  These are
# the assertions that justify the kernel change in phase G0.
il_assert_grep    "$LOG" "\\[gl\\] PASS blit_reject_kernel_ptr" "kernel-space src pointer rejected"
il_assert_grep    "$LOG" "\\[gl\\] PASS blit_reject_unmapped"   "unmapped src pointer rejected"
il_assert_grep    "$LOG" "\\[gl\\] PASS blit_reject_huge"       "oversized blit rejected"
il_assert_grep    "$LOG" "\\[gl\\] PASS blit_after_rejects"     "window still usable after rejects"

# Phase G1: context lifecycle, glClear and presentation.
il_assert_grep    "$LOG" "\\[gl\\] PASS ctx_create"            "GL context created"
il_assert_grep    "$LOG" "\\[gl\\] PASS gl_clear_blue"         "glClear writes the requested colour"
il_assert_grep    "$LOG" "\\[gl\\] PASS gl_clear_depth_far"    "depth buffer cleared to the far plane"
il_assert_grep    "$LOG" "\\[gl\\] PASS gl_invalid_clear_mask" "invalid clear mask raises GL_INVALID_VALUE"
il_assert_grep    "$LOG" "\\[gl\\] PASS gl_invalid_clear_no_effect" "invalid clear mask clears nothing"
il_assert_grep    "$LOG" "\\[gl\\] PASS gl_swap_buffers"       "frame presented to the window"
il_assert_grep    "$LOG" "\\[gl\\] PASS gl_swap_after_resize"  "context still usable after resize"
il_assert_grep    "$LOG" "\\[gl\\] PASS gl_no_context_is_error" "GL without a context is diagnosable"

# Crash safety.
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"            "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                          "no kernel panic"
il_assert_no_grep "$LOG" "triple fault"                   "no triple fault"

il_summary
