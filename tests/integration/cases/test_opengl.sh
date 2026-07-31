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

# One QEMU boot drives both programs.
#
# The glcube frame count goes through a file because the shell's "run" command
# uses spawn(), which does not forward argv -- the same convention /apm uses.
il_send_delay 8
il_send "run /gltest"
il_send_delay 14
il_send "write /tmp/glcube.frames 12"
il_send_delay 2
il_send "run /glcube"
il_send_delay 45
il_send "exit"

il_run_qemu "$LOG" 140

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

# Phase G2: matrix stacks, immediate mode and the transform pipeline.
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_point_pixel"        "vertex lands on the expected pixel"
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_push_pop"           "glPushMatrix/glPopMatrix restore the matrix"
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_stack_underflow"    "matrix stack underflow is reported"
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_line_run"           "line rasterises a contiguous run"
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_triangle_edge"      "triangle edges are drawn"
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_smooth_red_falls"   "smooth shading interpolates colour"
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_perspective_foreshortens" "perspective foreshortens distant geometry"
il_assert_grep    "$LOG" "\\[gl\\] PASS geo_behind_eye_dropped" "geometry behind the eye is dropped"

# The demo must have started and reached the GL stack.  Note the deliberately
# loose pattern: /glcube and the shell write to the same serial console from
# different threads, so their lines interleave mid-string and an exact-match
# grep on a full line is unreliable here.
il_assert_grep    "$LOG" "glcube"                         "glcube demo ran"

# Crash safety.
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"            "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                          "no kernel panic"
il_assert_no_grep "$LOG" "triple fault"                   "no triple fault"

il_summary
