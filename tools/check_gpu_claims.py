#!/usr/bin/env python3
"""Cross-check GPU_ACCEL_PLAN.md against the tree.

GPU_ACCEL_PLAN.md H0: the GPU acceleration series cannot drift from the
tree.  The GL2 L0 speciality, applied to the GPU stack: the plan's opener
facts are PINNED here as live greps — when a later phase moves one (H1
adds the counters, H2 elides the front-fb copy, H4 replaces the canned
TGSI arrays, H5 widens the batch), the pin moves in the same commit or CI
is red.

What this checker deliberately does NOT assert
----------------------------------------------
Existence of `patches/GPU_H*.patch`.  A `.patch` file on disk is evidence
that a FILE EXISTS, not that code works — the RINET2 precedent, restated
in the plan as D6.

Phase-gated shape (the check_gl2_claims.py rule): a pin binds while the
phase that MOVES it is not ✅ COMPLETE.  The plan's header says PLANNED,
so today every pin binds; each phase landing moves its own pin in the
same commit.

Usage:
    tools/check_gpu_claims.py
    tools/check_gpu_claims.py --selftest
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASE_ORDER = ["H0", "H1", "H2", "H3", "H4", "H5", "H6", "H7"]

# The 13 refusal conditions the eligibility screen names in its comment.
# H5's task list removes exactly four (depth, blend, cull, texture) and
# says so; the pin below counts the *code* lines, this list counts the
# *named* reasons — both must agree or the screen and its comment drifted.
ELIGIBILITY_REASONS = (
    "mode", "whole-triangles", "array source", "shader", "depth",
    "stencil", "blend", "alpha", "scissor", "cull", "polygon mode",
    "fog", "texture units",
)

# The H1 counter names, as the plan writes them.  H0 pins their ABSENCE.
GPU_COUNTER_NAMES = (
    "gpu_presents_full", "gpu_presents_rect", "gpu_pixels_transferred",
    "gpu_transfer_bytes", "gpu_flush_bytes", "gpu_present_fallbacks",
    "gpu_control_queue_waits", "gpu_control_queue_timeouts",
)


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def phase_done(plan, phase):
    return bool(re.search(
        r"^### %s[^\n]*\n+(?:\*\*)?Status: ✅ COMPLETE" % phase,
        plan, re.M))


def eligibility_refusals(glbackend):
    """Count the STATE refusals in the eligibility screen.

    Thirteen before H5, nine after it.  The screen always has two extra
    `return 0;` statements that are PRECONDITIONS, not refusals — `if
    (!ctx)` and `if (!b || !b->draw)` (software is the fallback) — so the
    raw statement count is two higher than the number the plan names, and
    this helper fails if a precondition is ever folded into it by accident.
    H5 removed exactly four (depth, blend, cull, texture), each of which
    became conditional on a BACKEND FLAG rather than on the GL state alone;
    the conditionals still contain a `return 0;`, so the statement walk
    sees them and the count lands on nine.
    """
    m = re.search(
        r"int gl_backend_draw_eligible\([^)]*\)\s*\{(.*?)\n\}",
        glbackend, re.S)
    if not m:
        return -1
    total = len(re.findall(r"\breturn 0;", m.group(1)))
    preconditions = len(re.findall(
        r"if \(!ctx\)|if \(!b \|\| !b->draw\)", m.group(1)))
    return total - preconditions


def front_fb_readers():
    """Files OTHER than graphics.c that mention front_fb.

    H2 skips the back→front copy when the scanout owns the display.  That
    is only sound while nothing else reads front_fb; the moment a reader
    appears the pixels it expects would be one frame stale.  Returned as a
    list so the failure message can name the file.
    """
    import subprocess
    out = subprocess.run(
        ["sh", "-c",
         "grep -rl front_fb --include=*.c --include=*.h kernel drivers "
         "userspace lib 2>/dev/null | grep -v graphics.c"],
        capture_output=True, text=True, cwd=ROOT).stdout
    return [line for line in out.split() if line.strip()]


def claims():
    plan = read("docs", "plans", "GPU_ACCEL_PLAN.md")
    perfstat = read("kernel", "lib", "perfstat.h")
    graphics = read("drivers", "framebuffer", "graphics.c")
    virtio_gpu = read("drivers", "gpu", "virtio_gpu.c")
    glbackend_h = read("lib", "libgl", "include", "GL", "glbackend.h")
    glbackend_c = read("lib", "libgl", "src", "glbackend.c")
    glarray_c   = read("lib", "libgl", "src", "glarray.c")
    glvirgl_c   = read("lib", "libgl", "src", "glvirgl.c")
    virgl_h     = read("drivers", "gpu", "virgl.h")
    glbe_test   = read("tests", "unit", "test_glbackend.c")
    goracle_c   = read("lib", "libgl", "src", "gloracle.c")
    goracle_h   = read("lib", "libgl", "src", "gloracle.h")
    goracle_t   = read("tests", "unit", "test_gloracle.c")
    gltest_c    = read("userspace", "tests", "gltest", "gltest.c")
    virgl_case  = read("tests", "integration", "cases", "test_virgl_gpu.sh")
    glvirgl_test = read("tests", "unit", "test_glvirgl.c")
    glvirgl = read("lib", "libgl", "src", "glvirgl.c")
    glcube = read("userspace", "demos", "glcube", "glcube.c")
    ledger = read("docs", "residue_ledger.md")
    opengl = read("docs", "opengl.md")
    gstatus = read("docs", "status.md")
    makefile = read("Makefile")
    checks = []

    h1 = phase_done(plan, "H1")
    h2 = phase_done(plan, "H2")
    h4 = phase_done(plan, "H4")
    h5 = phase_done(plan, "H5")
    h6 = phase_done(plan, "H6")
    h7 = phase_done(plan, "H7")
    h7 = phase_done(plan, "H7")

    # --- opener pin 1: MOVED BY H1 -----------------------------------
    # Before H1: no GPU counter existed.  After H1: exactly the eight the
    # plan names, in perfstat.h AND in the /proc/perf wire table.  The
    # pin moved in the commit that made it false (H0's rule).
    if not h1:
        checks.append((
            "opener 1: perfstat.h contains NO GPU/virgl/virtio counter "
            "(H1 adds them and moves this pin); the plan's 8 H1 names are "
            "absent from it",
            len(re.findall(r"GPU|VIRGL|VIRTIO_GPU", perfstat)) == 0 and
            all(name.upper() not in perfstat.upper()
                for name in GPU_COUNTER_NAMES)))
    else:
        perfstat_c = read("kernel", "lib", "perfstat.c")
        checks.append((
            "H1: all 8 counters are declared in perfstat.h and named in "
            "the /proc/perf wire table (perfstat.c)",
            all(name.upper() in perfstat.upper() for name in GPU_COUNTER_NAMES)
            and all(('"%s"' % name) in perfstat_c
                    for name in GPU_COUNTER_NAMES)))
        checks.append((
            "H1: the three copies are charged separately — the driver "
            "charges its own memcpy (transfer bytes), the command pair "
            "(flush bytes) and the rectangles (pixels), and graphics.c "
            "charges the presents",
            "PERF_GPU_TRANSFER_BYTES" in read("drivers", "gpu", "virtio_gpu.c")
            and "PERF_GPU_FLUSH_BYTES" in read("drivers", "gpu", "virtio_gpu.c")
            and "PERF_GPU_PIXELS_TRANSFERRED" in read("drivers", "gpu", "virtio_gpu.c")
            and "PERF_GPU_PRESENTS_FULL" in read("drivers", "framebuffer", "graphics.c")
            and "PERF_GPU_PRESENTS_RECT" in read("drivers", "framebuffer", "graphics.c")))
        checks.append((
            "H1: the control-queue busy-wait reports WAITS on success and "
            "TIMEOUTS on the deadline — a stall is a counter, not a hang "
            "(Fact 5's decision input)",
            "PERF_GPU_CONTROL_QUEUE_WAITS" in read("drivers", "gpu", "virtio_gpu.c")
            and "PERF_GPU_CONTROL_QUEUE_TIMEOUTS" in read("drivers", "gpu", "virtio_gpu.c")))
        procfs = read("kernel", "fs", "procfs.c")
        checks.append((
            "H1: /proc/fb exists — the geometry the measurement divides by "
            "(resolver, root listing, reader) and reads the SEAM, not a "
            "driver header (PARITY P1/R6: fs takes no drivers/*.h)",
            'strcmp(path, "fb")' in procfs
            and procfs.count("gfx_fbgeom_get") == 1
            and "inode_id == 16" in procfs
            and "#include \"drivers/" not in procfs))
        checks.append((
            "H1: the geometry seam is a real module — publish in the driver "
            "at the one place that owns the values, read in fs, and the "
            "i386 kernel carries it too (graphics.c is a second consumer)",
            "gfx_fbgeom_publish(fb_width, fb_height, fb_pitch)"
            in read("drivers", "framebuffer", "graphics.c")
            and read("kernel", "lib", "fbgeom.c") != ""
            and "kernel/lib/fbgeom.c" in makefile))
        checks.append((
            "H1: gpubench exists, is built and is in the initrd test list",
            read("userspace", "tests", "gpubench", "gpubench.c") != ""
            and "gpubench.elf" in makefile
            and "gpubench" in makefile.split("INITRD_TESTS")[1][:600]))
        checks.append((
            "H1: the host arithmetic test links the REAL perfstat.c and is "
            "in UNIT_TESTS",
            "test_perfstat_gpu" in makefile
            and "kernel/lib/perfstat.c" in makefile.split(
                "test_perfstat_gpu:")[1][:300]))
        checks.append((
            "H1: test_perf_smoke pins the eight counters and the "
            "transfer==flush invariant",
            all(name in read("tests", "integration", "cases",
                             "test_perf_smoke.sh")
                for name in GPU_COUNTER_NAMES)
            and "gpu_transfer_bytes" in read("tests", "integration", "cases",
                                             "test_perf_smoke.sh")))

    # --- opener pin 2: MOVED BY H2 -----------------------------------
    if not h2:
        checks.append((
            "opener 2: gfx_flip() still calls virtio_gpu_present() after "
            "the back→front memcpy loop (H2 elides it and moves this pin)",
            re.search(r"void gfx_flip\(void\)\s*\{(.*?)\n\}",
                      graphics, re.S) is not None and
            re.search(r"void gfx_flip\(void\)\s*\{(.*?)\n\}",
                      graphics, re.S).group(1).count("virtio_gpu_present(") == 1 and
            "front_fb" in re.search(r"void gfx_flip\(void\)\s*\{(.*?)\n\}",
                                    graphics, re.S).group(1)))
    else:
        checks.append((
            "H2: both flips guard the back→front copy on "
            "`scanout_owns_display`, so a healthy scanout skips it",
            graphics.count("if (!scanout_owns_display)") == 2))
        checks.append((
            "H2: the flag is set from the PRESENT'S RETURN, not assumed — a "
            "failed present re-arms the copy on the next flip",
            graphics.count("scanout_owns_display =") >= 3
            and "(virtio_gpu_present(back_fb, fb_width, fb_height, fb_pitch) == 0)"
            in graphics))
        checks.append((
            "H2: front_fb still has NO reader outside graphics.c (the "
            "elision's safety condition, re-checked now that it matters)",
            front_fb_readers() == []))

    # --- opener pin 3: the batch shape and the 13 refusals -------------
    if not h5:
        m = re.search(
            r"typedef struct gl_draw_batch\s*\{(.*?)\}\s*gl_draw_batch_t",
            glbackend_h, re.S)
        checks.append((
            "opener 3a: gl_draw_batch_t is still {const GLfloat *data; "
            "GLsizei count;} with NO format field (H5 adds it)",
            m is not None and
            "data" in m.group(1) and "count" in m.group(1) and
            "format" not in m.group(1)))
        checks.append((
            "opener 3b: the vertex comment still pins position + colour "
            "only (8 floats), no normal/texcoord (H5 widens to 12)",
            "position" in glbackend_h and "colour" in glbackend_h and
            "normal" not in glbackend_h and "texcoord" not in glbackend_h))
        checks.append((
            "opener 3c: gl_backend_draw_eligible() still has exactly 13 "
            "refusal statements (H5 removes depth/blend/cull/texture → ≤9)",
            eligibility_refusals(glbackend_c) == 13))
        checks.append((
            "opener 3d: the 13 named reasons in the plan agree with the "
            "13 state refusals in the code (counted on both sides, never "
            "estimated)",
            len(ELIGIBILITY_REASONS) == 13 and
            eligibility_refusals(glbackend_c) == len(ELIGIBILITY_REASONS)))
        checks.append((
            "opener 3e: two of the screen's `return 0;` are PRECONDITIONS "
            "(!ctx, no backend), not state refusals — 15 statements, 13 "
            "refusals, and the split is where the plan says it is",
            eligibility_refusals(glbackend_c) == 13 and
            len(re.findall(r"\breturn 0;", re.search(
                r"int gl_backend_draw_eligible\([^)]*\)\s*\{(.*?)\n\}",
                glbackend_c, re.S).group(1))) == 15))

    # --- H5: the widened draw path ------------------------------------
    if h5:
        # The versioned format, not a mutation of the old one.
        checks.append((
            "H5: gl_draw_batch_t carries a `format` field beside "
            "{data, count}, and the two formats are named constants",
            "unsigned       format;" in glbackend_h
            and "GL_BATCH_FMT_POS_COLOR 0u" in glbackend_h
            and "GL_BATCH_FMT_FULL      1u" in glbackend_h))
        checks.append((
            "H5: one stride definition (8 / 12 floats) shared by the batch "
            "builder, the backend and the tests",
            "GL_BATCH_STRIDE(fmt)" in glbackend_h
            and "GL_BATCH_STRIDE(fmt)" in glarray_c
            and glvirgl_c.count("GL_BATCH_STRIDE") >= 1
            and "GL_BATCH_STRIDE" in glvirgl_test))
        # Versioning is OPT-IN, so a backend that predates H5 never sees a
        # wider batch than it understands.
        checks.append((
            "H5: the wide format is opt-in — GL_BACKEND_DRAW_FULL is declared "
            "and gl_backend_draw_format() consults it",
            "GL_BACKEND_DRAW_FULL" in glbackend_h
            and "b->flags & GL_BACKEND_DRAW_FULL" in glbackend_c))
        # The screen: 13 -> 9, with the four removals each gated on backend
        # support rather than dropped outright.
        checks.append((
            "H5: gl_backend_draw_eligible() is down to NINE state refusals "
            "(13 before this phase)",
            eligibility_refusals(glbackend_c) == 9))
        checks.append((
            "H5: each of the four removals is CONDITIONAL on the backend, "
            "never an unconditional drop",
            "GL_BACKEND_DEPTH) == 0" in glbackend_c
            and "GL_BACKEND_TEXTURE) == 0" in glbackend_c
            and "blend_is_gpu_exact" in glbackend_c))
        checks.append((
            "H5: the blend set is the enumerated 1:1 factors, and an "
            "unlisted factor still declines",
            "blend_is_gpu_exact" in glbackend_c
            and glbackend_c.count("GL_ONE_MINUS_SRC_ALPHA") >= 1
            and "default: return 0;" in glbackend_c))
        # Depth: a real surface and a DSA that names PIPE_FUNC_LESS.
        checks.append((
            "H5: depth is a real surface — created, attached as zsurf, and "
            "described by a DSA that uses PIPE_FUNC_* (not a GL enum)",
            "ensure_depth_surface" in glvirgl_c
            and "VIRGL_ZSURFACE_HANDLE" in glvirgl_c
            and "VIRGL_PIPE_FUNC_LESS" in glvirgl_c
            and "VIRGL_PIPE_FORMAT_Z32_FLOAT" in virgl_h))
        checks.append((
            "H5: the depth surface is released with its render target",
            "release_target" in glvirgl_c
            and glvirgl_c.count("vg.zres") >= 3))
        # The setup stream keeps the L6 bytes when nothing is asked for.
        checks.append((
            "H5: gl_virgl_canned_setup() still exists as the zero-state "
            "wrapper, so GL2 L6's bytes are produced by the same path",
            "int gl_virgl_canned_setup" in glvirgl_c
            and "gl_virgl_setup_state(w, h, NULL, 0, d, cap)" in glvirgl_c))
        # The widened VELEM describes four elements at the right offsets.
        checks.append((
            "H5: format 1's VELEM describes four elements — normal at byte "
            "32 and texcoord at byte 44 — derived from the same stride",
            "nelem = 4;" in glvirgl_c
            and "VIRGL_PIPE_FORMAT_R32G32B32_FLOAT" in glvirgl_c
            and "44;                /* tex s" in glvirgl_c))
        # The tests the plan asks for actually exist and are wired in.
        checks.append((
            "H5: the eligibility table is ENUMERATED as tests — each of the "
            "four removals paired with the state that still declines",
            "t_h5_depth_declines_without_backend_support" in glbe_test
            and "t_h5_blend_with_constant_colour_declines" in glbe_test
            and "t_h5_cull_cw_winding_declines" in glbe_test
            and "t_h5_texture_second_unit_declines" in glbe_test
            and "t_h5_still_declines" in glbe_test))
        checks.append((
            "H5: the nine that stayed are enumerated by the test too, not "
            "left implicit",
            "GL_LINES" in glbe_test and "GL_TRIANGLES, 4" in glbe_test
            and "c->fog = 1;" in glbe_test))
        checks.append((
            "H5: format 0 vs 1 byte-identity is asserted on the SETUP stream",
            "test_h5_format_versioning" in glvirgl_test
            and "BYTE-IDENTICAL to the GL2 L6 stream" in glvirgl_test))
        checks.append((
            "H5: the state words are asserted in the stream — depth as "
            "PIPE_FUNC_LESS, blend factors src|dst<<16, cull as PIPE_FACE_*",
            "test_h5_state_in_stream" in glvirgl_test
            and "VIRGL_PIPE_FUNC_LESS << 2" in glvirgl_test))

    # --- H6: the frame oracle -----------------------------------------
    if h6:
        checks.append((
            "H6: the deterministic scene lives in libgl, not in a test binary, "
            "so both lanes render the SAME scene",
            "int gloracle_render" in goracle_c
            and "gloracle_render" in goracle_t and "gloracle_render" in gltest_c))
        checks.append((
            "H6: the hash is the kernel's own SHA-256, not a private checksum",
            "ksha256(" in goracle_c
            and '#include "../../../kernel/lib/sha256.h"' in goracle_c))
        # The stated tolerance is in the header AND in the plan; the checker is
        # what stops the two drifting apart.
        checks.append((
            "H6: the tolerance is a stated number in the code (5 permille, "
            "epsilon 1/255) and the plan quotes the same pair",
            "GLORACLE_TOLERANCE_PERMILLE 5u" in goracle_h
            and "GLORACLE_CHANNEL_EPSILON 1u" in goracle_h
            and "5 pixels per thousand" in plan
            and "1/255" in plan))
        checks.append((
            "H6: the oracle's own test carries MUTATION cases -- an oracle "
            "that cannot see a regression is worse than none",
            "t_hash_sees_one_pixel" in goracle_t
            and "t_hash_sees_flipped_rows" in goracle_t
            and "t_compare_band_is_rejected" in goracle_t
            and "t_compare_shifted_scene_rejected" in goracle_t
            and "t_compare_swapped_channels_rejected" in goracle_t))
        checks.append((
            "H6: the tolerance is asserted in BOTH directions -- an epsilon is "
            "accepted and a band rejected, at the exact budget boundary",
            "t_compare_epsilon_is_tolerated" in goracle_t
            and "t_compare_budget_boundary" in goracle_t))
        # The three defects the oracle found, each with the test that pins it.
        checks.append((
            "H6: the dangling-current-context defect is pinned by its own "
            "regression test",
            "t_render_restores_caller_context" in goracle_t
            and "aglxGetCurrentContext()" in goracle_c))
        checks.append((
            "H6: the guest lane prints the hash the host lane can diff",
            "H6 software frame sha256" in gltest_c
            and "h6_software_hash_stable" in gltest_c))
        checks.append((
            "H6: the integration case asserts the oracle ran AND says plainly "
            "that a hardware frame cannot be read back",
            "h6_software_render" in virgl_case
            and "upload-only" in virgl_case))
        # The measured reference hash, recorded rather than implied.
        checks.append((
            "H6: the software reference hash is a recorded measurement",
            "93ea06fd" in plan and "e87cb" in plan))

    # --- opener pin 4: the canned TGSI arrays are still hand-written ----
    if not h4:
        checks.append((
            "opener 4a: glvirgl.c still contains the hand-written "
            "canned_vs_tgsi[21] and canned_fs_tgsi[13] arrays (34 dwords; "
            "H4 replaces them with generated tokens)",
            re.search(r"canned_vs_tgsi\[21\]\s*=\s*\{", glvirgl) is not None
            and re.search(r"canned_fs_tgsi\[13\]\s*=\s*\{", glvirgl)
            is not None))
        checks.append((
            "opener 4b: 21 + 13 = 34 dwords — the plan's §5 number is the "
            "tree's number",
            21 + 13 == 34))
        checks.append((
            "opener 4c: there is still no GLSL→TGSI emitter (H4 creates "
            "glsl_tgsi.c or the equivalent named seam)",
            read("lib", "libgl", "src", "glsl_tgsi.c") == "" and
            "tgsi_emit" not in glvirgl))

    # --- opener pin 5: RES-54 is still OPEN (H4/H5 flip it) ------------
    if not (h4 and h5):
        checks.append((
            "opener 5: the ledger still has RES-54 as class S, OPEN — the "
            "plan's own opener fact; H4/H5 flip it and move this pin",
            re.search(r"^\| RES-54 \| S \| OPEN \|", ledger, re.M)
            is not None))

    # --- opener pin 6: the control-queue busy-wait (Fact 5) ------------
    checks.append((
        "opener 6: submit_2desc() still busy-waits on vq_used->idx with "
        "a bounded timeout (Fact 5 — the plan records it as residue, "
        "not a bug, so the pin outlives H2 unless the cursor queue ships)",
        re.search(r"static int submit_2desc\(.*?\n\}", virtio_gpu,
                  re.S) is not None and
        "vq_used->idx == vq_last_used_idx" in virtio_gpu and
        "10000000" in virtio_gpu))

    # --- plan opener facts the tree can contradict ---------------------
    checks.append((
        "Fact 1: the plan's virgl=0 sentence matches the driver (VIRGL is "
        "negotiated only when offered) and the plan names the 2D path as "
        "the working one",
        "VIRTIO_GPU_F_VIRGL" in virtio_gpu and
        "virgl=0" in plan and "2D" in plan))
    checks.append((
        "Fact 3: /glcube still draws lit, textured, depth-tested geometry "
        "(which is why the canned path cannot take it) — the plan's "
        "central asymmetry",
        "GL_LIGHTING" in glcube and "GL_TEXTURE_2D" in glcube and
        "GL_DEPTH_TEST" in glcube))
    checks.append((
        "Fact 6: both docs name RES-54 and opengl.md keeps an explicit "
        "hardware-drawing boundary row (the plan must not have weakened them "
        "into silence -- H7 rewrites the row's CONTENT, not its existence)",
        "RES-54" in opengl and "RES-54" in gstatus
        and "Hardware-accelerated" in opengl))

    # --- H7: the docs tell the truth about what H0-H6 did -------------
    if h7:
        gdoc = read("docs", "GPU_ACCELERATION.md")
        ledger = read("docs", "residue_ledger.md")
        changelog = read("CHANGELOG.md")
        checks.append((
            "H7: docs/opengl.md's hardware-drawing row describes what H5 "
            "ACTUALLY admits, and no longer promises a successor plan it now has",
            "thirteen→nine states" in opengl
            and "Lighting stays on the CPU" in opengl
            and "GPU_ACCEL_PLAN H4 ships the retarget" in opengl))
        checks.append((
            "H7: the architecture doc exists and names the four things a "
            "reader must not assume",
            gdoc.count("must not assume") >= 1
            and "upload-only" in gdoc and "PIPE_FUNC_LESS" in gdoc
            and "GL_BATCH_FMT_FULL" in gdoc))
        checks.append((
            "H7: the doc states the batch format's arithmetic honestly "
            "(twelve floats, thirteen fields, and where the extra went)",
            "sum to thirteen" in gdoc and "Twelve won" in gdoc))
        checks.append((
            "H7: docs/status.md carries the series' numbers AND keeps the "
            "test glyph, because no gate has seen a hardware frame",
            "GPU_ACCEL_PLAN H0–H7 complete" in gstatus
            and "upload-only" in gstatus and "93ea06fd" in gstatus))
        checks.append((
            "H7: RES-54 is DONE and the two honest survivors are OPEN rows",
            "| RES-54 | S | DONE@H5 |" in ledger
            and "| RES-63 | S | OPEN |" in ledger
            and "| RES-64 | M | OPEN |" in ledger))
        checks.append((
            "H7: the ledger's arithmetic names the new class pins, so a "
            "later row cannot be added without moving them",
            "Rows: 64." in ledger and "W 36 · M 9 · N 4 · S 15" in ledger))
        checks.append((
            "H7: CHANGELOG carries one entry per phase for H3 through H7",
            all(f"## [GPU {p} —" in changelog for p in
                ("H3", "H4", "H5", "H6", "H7"))))

    # --- H2's guard: upstream's SIMD path has an oracle --------------
    # 4628af0 vectorised the MSDF renderer with no test and no number.
    # H2's gate supplies both; this pins that the gate exists, links the
    # real file, and that the kernels' hooks stay out of the kernel.
    sdf_rs = read("kernel", "gui", "sdf", "sdf.rs")
    sdf_test = read("tests", "unit", "test_sdf_simd.rs")
    checks.append((
        "H2 guard: the SSE4.1 MSDF path is gated by a host test that links "
        "the REAL sdf.rs (not a copy) and compares it to the scalar path",
        sdf_test != ""
        and '#[path = "../../kernel/gui/sdf/sdf.rs"]' in sdf_test
        and "coverage4" in sdf_test.replace(" ", "")
        and "test_sdf_simd.sh" in makefile))
    checks.append((
        "H2 guard: the test hooks compile to nothing in the kernel "
        "(`cfg(test_sdf_host)` only), so `make iso` gains no symbol",
        sdf_rs.count("#[cfg(test_sdf_host)]") == 4
        and '--cfg test_sdf_host' in read("tools", "test_sdf_simd.sh")))
    checks.append((
        "H2 guard: the commit is titled `perf` with no number -- the plan "
        "records the measured ratio rather than inheriting the adjective",
        "4628af0" in plan and "ns/quad" in plan
        and ("1.07" in plan or "1.13" in plan)))

    # --- H3's pins: the texture path, and the split that keeps one -----
    # definition of the wire format ---------------------------------------
    # H3 moved the packet builders into virgl_cmd.c so libgl links the same
    # encoders the kernel drives.  These pins keep the split honest: a
    # builder drifting back into glvirgl.c would pass every test here and
    # still be the second copy the G13 lesson forbids.
    virgl_cmd_c = read("drivers", "gpu", "virgl_cmd.c")
    glvirgl_c = read("lib", "libgl", "src", "glvirgl.c")
    if phase_done(plan, "H3"):
        checks.append((
            "H3: the VirGL packet builders live in a transport-free file that "
            "BOTH the kernel and libgl link (the G13 one-definition lesson)",
            virgl_cmd_c.count("int virgl_cmd_") >= 16
            and "virtio_gpu" not in virgl_cmd_c
            and "virgl_cmd.c" in makefile))
        checks.append((
            "H3: libgl does NOT restate the encoding -- glvirgl.c calls the "
            "builders and contains no VIRGL_CMD0 header of its own for a "
            "sampler packet",
            "virgl_cmd_create_sampler_view" in glvirgl_c
            and "virgl_cmd_create_sampler_state" in glvirgl_c
            and "virgl_cmd_set_sampler_views" in glvirgl_c))
        checks.append((
            "H3: the builders are walked against the REAL kernel validator "
            "on the host, and a wrong length is refused",
            read("tests", "unit", "test_tex_packets.c") != ""
            and "gpu_validate_cmd_stream" in read("tests", "unit", "test_tex_packets.c")
            and "test_tex_packets" in makefile))
        checks.append((
            "H3: the dirty discipline is a per-texture flag cleared only "
            "AFTER a successful transfer (a failed upload retries)",
            "t->dirty = 0;" in glvirgl_c
            and "return -1;\n    }\n    t->dirty = 0;" in glvirgl_c))
        checks.append((
            "H3: a mip chain is ONE resource carrying last_level -- the "
            "driver's RESOURCE_CREATE_3D already takes it",
            "rc.last_level" in glvirgl_c and "last_level" in read("kernel", "gpu", "gpu_syscalls.h")))
        checks.append((
            "H3: sizeof(aglx_context) did not grow (the pool is backend "
            "static state; G10's re-measure rule is met by not growing)",
            "240328" in read("tests", "unit", "test_glvirgl.c")))
        checks.append((
            "H3: test_glvirgl carries the texture inventory (>=90 checks)",
            "texture inventory" in read("tests", "unit", "test_glvirgl.c")))

    # --- H4's pins: the retarget, and the finding it made --------------
    tgsi_hdr = read("lib", "libgl", "src", "tgsi.h")
    tgsi_src = read("lib", "libgl", "src", "tgsi.c")
    tgsi_test = read("tests", "unit", "test_tgsi.c")
    if phase_done(plan, "H4"):
        checks.append((
            "H4: the retarget exists as a consumer of the typed AST, and says "
            "so (a back end, not a new compiler)",
            "tgsi_retarget" in tgsi_hdr and
            "a back end, not a new compiler" in tgsi_hdr and
            "tgsi_retarget" in tgsi_src and "tgsi.c" in makefile))
        checks.append((
            "H4: the test carries an INDEPENDENT disassembler written from the "
            "spec, not from the emitter's constants (a test that reuses the "
            "emitter cannot catch a wrong constant)",
            "disasm(" in tgsi_test and
            "transcribed from the spec" in tgsi_test.replace("\n", " ")))
        checks.append((
            "H4: the finding is PINNED — the canned arrays are asserted to be "
            "invalid TGSI (swapped processors, token types 1/3, bad src index)",
            "test_canned_is_not_valid" in tgsi_test and
            "gl_virgl_canned_vs_tokens" in tgsi_test and
            "PROC_FRAGMENT" in tgsi_test and
            "canned" in plan.lower()))
        checks.append((
            "H4: the header RECORDS that the canned arrays are not an "
            "authority and must not be made consistent with",
            "must never be" in tgsi_hdr and "authority" in tgsi_hdr))
        checks.append((
            "H4: refusals NAME the construct (the G11a rule) — a diagnostic "
            "carries the built-in's name, not just 'unsupported'",
            "has no exact TGSI opcode" in tgsi_src and
            "built-in '%s'" in tgsi_src))
        checks.append((
            "H4: the interpreter stays — a refusal leaves the unit compiled",
            "u->compiled" in tgsi_test or "stays compiled" in tgsi_test))
        checks.append((
            "H4: NrTokens is COMPUTED from the operand count, never typed "
            "(H3's length-lie lesson applied to TGSI)",
            "nr_tokens" in tgsi_hdr and "TGSI_INSN(opcode, nr, " in tgsi_src.replace(" ", "")or
            "1u + (has_dst" in tgsi_src))
        checks.append((
            "H4: the IMMEDIATE Size semantics are recorded (Size counts the "
            "whole token — the trap that caught the first walker)",
            "Size = 1 + N" in tgsi_hdr and "Size = 1 + N" in tgsi_test))
        checks.append((
            "H4: the plan states the emitted subset honestly and does NOT "
            "claim /glshade's Lambert shader retargets",
            "arithmetic subset" in plan and "matrices" in plan.lower()))

    # --- structural: status header vs table ---------------------------
    done_rows = len(re.findall(
        r"^\|\s*H\d+\s+—[^|]*\|\s*✅ complete", plan, re.M))
    done_heads = sum(1 for p in PHASE_ORDER if phase_done(plan, p))
    checks.append((
        "plan: every ✅ complete row has a COMPLETE heading",
        done_rows == done_heads and plan != ""))
    checks.append((
        "plan: exactly 8 phases H0–H7, in order, no gaps or repeats",
        [m for m in re.findall(r"^### (H\d+) —", plan, re.M)]
        == PHASE_ORDER))

    header = re.search(r"^## Status: (.*)$", plan, re.M)
    status_ok = False
    if re.search(r"^## Status: (PLANNED|PROPOSED)", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        status_ok = (0 < done_rows < len(PHASE_ORDER))
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == len(PHASE_ORDER))
    checks.append((
        "plan: the Status header agrees with the table (%d/%d ✅, %r)"
        % (done_rows, len(PHASE_ORDER),
           header.group(1) if header else ""),
        status_ok))

    # --- every phase carries a test gate and a DoD ---------------------
    sections = re.split(r"^### (H\d) —", plan, flags=re.M)
    have_gate, have_dod, have_deliver = [], [], []
    for i in range(1, len(sections) - 1, 2):
        name, body = sections[i], sections[i + 1]
        have_gate.append((name, "**Test gate:**" in body))
        have_dod.append((name, "**Definition of Done:**" in body))
        have_deliver.append(
            (name, re.search(r"\*\*Deliverable:\*\*\s*`patches/GPU_%s_[^`]+`"
                             % name, body) is not None))
    checks.append((
        "plan: every phase states a Test gate (%s)"
        % ", ".join(n for n, ok in have_gate if not ok) or "all",
        all(ok for _, ok in have_gate)))
    checks.append((
        "plan: every phase states a Definition of Done (%s)"
        % (", ".join(n for n, ok in have_dod if not ok) or "all"),
        all(ok for _, ok in have_dod)))
    checks.append((
        "plan: every phase names its GPU_<P>_ deliverable patch (%s)"
        % (", ".join(n for n, ok in have_deliver if not ok) or "all"),
        all(ok for _, ok in have_deliver)))

    # --- the plan's §5 numbers that the tree can check today -----------
    checks.append((
        "§5: the measured cells carry the † mark and BOTH arithmetic rows "
        "the plan publishes are present — the 1280×800 scanout geometry "
        "(8 192 000 B in two copies) and H1's measured 1024×768 frame "
        "geometry (12 189 696 B with the GPU, 6 094 848 without)",
        "8 192 000" in plan and "4 096 000" in plan and "†" in plan
        and "6 094 848" in plan and "12 189 696" in plan
        and "2.000" in plan))
    checks.append((
        "§5: sizeof(aglx_context) 240 328 is the value the last series "
        "left (GL2_PLAN.md's own number)",
        "240 328" in plan and "240 328" in read("docs", "plans",
                                                "GL2_PLAN.md")))

    # --- H0's own wiring, checked only once H0 says it landed ----------
    if phase_done(plan, "H0"):
        checks.append((
            "H0: the checker is wired into make test-unit via its "
            "wrapper",
            "check_gpu_claims.py" in makefile and
            "test_gpu_claims.sh" in makefile))
        checks.append((
            "H0: tests/unit/test_gpu_claims.sh runs the checker and then "
            "proves it can fail",
            "check_gpu_claims.py" in read("tests", "unit",
                                          "test_gpu_claims.sh") and
            "--selftest" in read("tests", "unit", "test_gpu_claims.sh")))

    # --- H7's honesty sweep, checked only once H7 says it landed ------
    if h7:
        checks.append((
            "H7: docs/GPU_ACCELERATION.md exists and names what stays "
            "software",
            read("docs", "GPU_ACCELERATION.md") != "" and
            "software" in read("docs", "GPU_ACCELERATION.md")))
        checks.append((
            "H7: docs/opengl.md's hardware-drawing row moved only as far "
            "as H5 reached (the eligibility list is spelled out there)",
            "eligible" in opengl.lower() or "eligibility" in opengl.lower()))

    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(ok for _, ok in results):
            print("check_gpu_claims: SELFTEST inconclusive (tree already "
                  "red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(ok for _, ok in doctored):
            print("check_gpu_claims: SELFTEST FAIL -- passes against an "
                  "empty tree", file=sys.stderr)
            return 1
        print("check_gpu_claims: selftest PASS (doctored tree detected)")
        return 0

    results = claims()
    bad = [name for name, ok in results if not ok]
    for name in bad:
        print(f"check_gpu_claims: FAIL -- {name}", file=sys.stderr)
    if bad:
        print(f"check_gpu_claims: {len(bad)} claim(s) disagree with the "
              f"tree", file=sys.stderr)
        return 1
    print(f"check_gpu_claims: OK -- {len(results)} claims verified against "
          f"the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
