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
il_send_delay 30
il_send "write /tmp/glcube.frames 12"
il_send_delay 2
il_send "run /glcube"
il_send_delay 45
il_send "exit"

il_run_qemu "$LOG" 170

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

# Phase G3: filled rasterizer, depth buffer, culling.
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_triangle_filled"     "triangles are filled"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_gouraud_red"         "Gouraud interpolation across the face"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_depth_nearer_wins"   "depth test keeps the nearer surface"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_depth_farther_rejected" "depth test rejects the farther surface"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_cull_back_face"      "back faces are culled"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_no_diagonal_seam"    "shared edge tiles with no seam"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_scissor_inside"      "scissor test clips"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_polymode_line_hollow" "glPolygonMode(GL_LINE) restores wireframe"
il_assert_grep    "$LOG" "\\[gl\\] PASS ras_huge_triangle_bounded" "huge triangles are bounded by the buffer"

# Phase G4: frustum clipping and the attribute stack.
il_assert_grep    "$LOG" "\\[gl\\] PASS clip_near_plane_split"   "near-plane geometry is split, not dropped"
il_assert_grep    "$LOG" "\\[gl\\] PASS clip_fully_behind_dropped" "geometry behind the eye is dropped"
il_assert_grep    "$LOG" "\\[gl\\] PASS clip_two_behind"         "two-vertices-behind case is fanned correctly"
il_assert_grep    "$LOG" "\\[gl\\] PASS clip_line_crossing_near" "lines crossing the near plane are shortened"
il_assert_grep    "$LOG" "\\[gl\\] PASS clip_far_plane"          "far-plane clipping works"
il_assert_grep    "$LOG" "\\[gl\\] PASS clip_preserves_winding"  "clipping preserves winding for culling"
il_assert_grep    "$LOG" "\\[gl\\] PASS clip_camera_flythrough"  "camera can fly through geometry"
il_assert_grep    "$LOG" "\\[gl\\] PASS attrib_restores_enable"  "glPushAttrib/glPopAttrib restore state"

# Phase G5: lighting and materials.
il_assert_grep    "$LOG" "\\[gl\\] PASS lit_diffuse_head_on"     "diffuse term saturates head-on"
il_assert_grep    "$LOG" "\\[gl\\] PASS lit_diffuse_perpendicular" "diffuse falls off at 90 degrees"
il_assert_grep    "$LOG" "\\[gl\\] PASS lit_specular_highlight"  "Blinn-Phong specular highlight"
il_assert_grep    "$LOG" "\\[gl\\] PASS lit_emission"            "emission applies without lights"
il_assert_grep    "$LOG" "\\[gl\\] PASS lit_distance_attenuation" "positional lights attenuate with distance"
il_assert_grep    "$LOG" "\\[gl\\] PASS lit_color_material"      "GL_COLOR_MATERIAL drives the material"
il_assert_grep    "$LOG" "\\[gl\\] PASS lit_gouraud_gradient"    "lighting is Gouraud-interpolated"

# Phase G6: textures, blending, alpha test, fog.
il_assert_grep    "$LOG" "\\[gl\\] PASS tex_quadrant_red"        "nearest sampling hits the right texel"
il_assert_grep    "$LOG" "\\[gl\\] PASS tex_quadrant_blue"       "texture v axis is not flipped"
il_assert_grep    "$LOG" "\\[gl\\] PASS tex_wrap_repeat"         "GL_REPEAT tiles the texture"
il_assert_grep    "$LOG" "\\[gl\\] PASS tex_bilinear_average"    "bilinear filtering averages texels"
il_assert_grep    "$LOG" "\\[gl\\] PASS tex_env_modulate"        "GL_MODULATE combines texel and fragment"
il_assert_grep    "$LOG" "\\[gl\\] PASS tex_env_replace"         "GL_REPLACE ignores the fragment colour"
il_assert_grep    "$LOG" "\\[gl\\] PASS blend_src_alpha"         "alpha blending composites correctly"
il_assert_grep    "$LOG" "\\[gl\\] PASS alpha_test_discards"     "alpha test discards below the reference"
il_assert_grep    "$LOG" "\\[gl\\] PASS fog_far"                 "distant geometry is fully fogged"
il_assert_grep    "$LOG" "\\[gl\\] PASS fog_near"                "near geometry is unfogged"

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
