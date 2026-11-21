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
    """Count the STATE refusals in the eligibility screen: 13.

    The screen has 15 `return 0;` statements in total, but two of them are
    preconditions, not refusals — `if (!ctx)` and `if (!b || !b->draw)`
    (software is the fallback).  The plan's 13 is the count of *state*
    conditions, and this helper fails if a precondition is ever folded
    into it by accident.  The texture loop is ONE refusal covering N
    units; counting statements (not units) is what the 13 names.
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


def claims():
    plan = read("docs", "plans", "GPU_ACCEL_PLAN.md")
    perfstat = read("kernel", "lib", "perfstat.h")
    graphics = read("drivers", "framebuffer", "graphics.c")
    virtio_gpu = read("drivers", "gpu", "virtio_gpu.c")
    glbackend_h = read("lib", "libgl", "include", "GL", "glbackend.h")
    glbackend_c = read("lib", "libgl", "src", "glbackend.c")
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

    # --- opener pin 2: gfx_flip still pays the second full copy --------
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
        "Fact 6: docs/opengl.md still refuses the hardware-drawing claim "
        "and both docs name RES-54 (the plan must not have weakened them)",
        "RES-54" in opengl and "RES-54" in gstatus and
        "Hardware-accelerated" in opengl))

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
