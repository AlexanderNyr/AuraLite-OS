#!/usr/bin/env python3
"""Cross-check GL2_PLAN.md against the tree.

GL2_PLAN.md L0: the OpenGL continuation series cannot drift from the
tree.  The Y0 speciality, applied to libgl: the plan's opener facts are
PINNED here as live greps — when a later phase moves one (L1 lands
stencil, L2 lands copies, L3 raises the unit count, L6 adds a draw
hook), the pin moves
in the same commit or CI is red.

What this checker deliberately does NOT assert
----------------------------------------------
Existence of `patches/GL2_L*.patch`.  A `.patch` file on disk is
evidence that a FILE EXISTS, not that code works — the RINET2
precedent.  Every phase is judged by the code and the gates it landed.

Usage:
    tools/check_gl2_claims.py
    tools/check_gl2_claims.py --selftest
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASE_ORDER = ["L0", "L1", "L2", "L3", "L4", "L5", "L6", "L7"]

# Host libgl binaries that UNIT_TESTS must keep.  Super-set is fine
# (L1 adds test_glstencil); dropping one of these is a regression.
LIBGL_HOST = (
    "test_glmath", "test_glstate", "test_glimm", "test_glraster",
    "test_glclip", "test_gllight", "test_gltex", "test_gltex2",
    "test_glarray", "test_glu", "test_glbackend", "test_glfbo",
    "test_glstencil",
    "test_glsl", "test_glslexec", "test_glprog", "test_glcoexist",
    "test_glvirgl",
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


def backend_has_draw(glbackend):
    """True iff gl_backend_t actually has a draw function pointer.

    The header *comment* already talks about drawing a batch of
    triangles — that must not count.  Only the struct body does.
    """
    m = re.search(
        r"typedef struct gl_backend\s*\{(.*?)\}\s*gl_backend_t",
        glbackend, re.S)
    if not m:
        return False
    return "(*draw)" in m.group(1)


def claims():
    plan = read("docs", "plans", "GL2_PLAN.md")
    makefile = read("Makefile")
    glh = read("lib", "libgl", "include", "GL", "gl.h")
    glctx = read("lib", "libgl", "src", "glcontext.h")
    glmatrix = read("lib", "libgl", "src", "glmatrix.c")
    glfbo = read("lib", "libgl", "src", "glfbo.c")
    glstate = read("lib", "libgl", "src", "glstate.c")
    glvirgl = read("lib", "libgl", "src", "glvirgl.c")
    glbackend = read("lib", "libgl", "include", "GL", "glbackend.h")
    glcube = read("userspace", "demos", "glcube", "glcube.c")
    glgears = read("userspace", "demos", "glgears", "glgears.c")
    opengl = read("docs", "opengl.md")
    checks = []

    l0 = phase_done(plan, "L0")
    l1 = phase_done(plan, "L1")
    l2 = phase_done(plan, "L2")
    l3 = phase_done(plan, "L3")
    l5 = phase_done(plan, "L5")
    l6 = phase_done(plan, "L6")

    # --- opener pins (moved by later phases, in the same commit) ------
    if not l3:
        checks.append((
            "opener: GL_MAX_TEXTURE_UNITS_IMPL is 2 — L3 moves this pin",
            re.search(r"#define\s+GL_MAX_TEXTURE_UNITS_IMPL\s+2\b",
                      glctx) is not None))
        checks.append((
            "opener: glMatrixMode(GL_TEXTURE) is GL_INVALID_OPERATION "
            "— L3 moves this pin",
            "mode == GL_TEXTURE" in glmatrix and
            "GL_INVALID_OPERATION" in glmatrix and
            "no texture matrix yet" in glmatrix))
        checks.append((
            "opener: gl.h has no GL_COMBINE token — L3 moves this pin",
            "#define GL_COMBINE" not in glh))
    if not l1:
        checks.append((
            "opener: glfbo.c still refuses a stencil buffer — L1 moves "
            "this pin",
            "there is no stencil buffer" in glfbo))
        checks.append((
            "opener: glClear accepts GL_STENCIL_BUFFER_BIT and ignores "
            "it — L1 moves this pin",
            "GL_STENCIL_BUFFER_BIT is accepted but has no effect"
            in glstate))
    if not l2:
        checks.append((
            "opener: gl.h has no glCopyTex / glBlitFramebuffer — L2 "
            "moves this pin",
            "glCopyTex" not in glh and "glBlitFramebuffer" not in glh))
    if not l6:
        checks.append((
            "opener: glvirgl.c does NOT implement DRAW_VBO — L6 moves "
            "this pin",
            "It does NOT implement DRAW_VBO" in glvirgl))
        checks.append((
            "opener: gl_backend_t has no draw member — L6 moves this pin",
            not backend_has_draw(glbackend)))
    if not l5:
        checks.append((
            "opener: /glcube and /glgears do not call glCreateShader / "
            "glUseProgram — L5 moves this pin",
            "glCreateShader" not in glcube and
            "glUseProgram" not in glcube and
            "glCreateShader" not in glgears and
            "glUseProgram" not in glgears))

    # --- L0: the rig (when the plan says it landed) -------------------
    if l0:
        checks.append((
            "L0: the checker is wired into make test-unit",
            "check_gl2_claims.py" in makefile and
            "test_gl2_claims.sh" in makefile))
        checks.append((
            "L0: tests/unit/test_gl2_claims.sh exists",
            "check_gl2_claims.py" in read("tests", "unit",
                                         "test_gl2_claims.sh")))
        checks.append((
            "L0: the virtio-gpu hang sentence is gone from "
            "docs/opengl.md (RES-40 closed at residue R1)",
            "The virtio-gpu driver hangs during initialisation"
            not in opengl))
        checks.append((
            "L0: docs/opengl.md records the RES-40 correction",
            "RES-40" in opengl and
            ("no longer reproduces" in opengl or
             "closed" in opengl.lower())))
        checks.append((
            "L0: UNIT_TESTS still names every libgl host binary",
            all(name in makefile for name in LIBGL_HOST)))
        checks.append((
            "L0: the plan correction is recorded (hang was stale "
            "against TODO.md / the residue ledger)",
            "RES-40" in plan and "stale" in plan.lower()))

    # --- L1: stencil (when the plan says it landed) -------------------
    if l1:
        checks.append((
            "L1: GL_STENCIL_TEST, GL_KEEP and glStencilFunc are in gl.h",
            "#define GL_STENCIL_TEST" in glh and
            "#define GL_KEEP" in glh and
            "glStencilFunc" in glh))
        checks.append((
            "L1: AGLX_STENCIL is a context flag",
            "AGLX_STENCIL" in read("lib", "libgl", "include", "GL",
                                   "auraglx.h")))
        checks.append((
            "L1: test_glstencil is in UNIT_TESTS",
            "test_glstencil" in makefile))
        checks.append((
            "L1: glClear writes the stencil plane",
            "mask & GL_STENCIL_BUFFER_BIT" in glstate and
            "ctx->stencil" in glstate and
            "GL_STENCIL_BUFFER_BIT is accepted but has no effect"
            not in glstate))
        checks.append((
            "L1: docs/opengl.md no longer lists stencil as unimplemented",
            "GL_STENCIL_BUFFER_BIT is accepted by `glClear` and ignored"
            not in opengl))
        checks.append((
            "L1: GL_STENCIL_ATTACHMENT is a real FBO slot",
            "there is no stencil buffer" not in glfbo))

    # --- L2: copies (when the plan says it landed) --------------------
    if l2:
        checks.append((
            "L2: glCopyTexImage2D and glBlitFramebuffer are in gl.h",
            "glCopyTexImage2D" in glh and "glBlitFramebuffer" in glh))
        checks.append((
            "L2: GL_READ_FRAMEBUFFER is a bind target",
            "#define GL_READ_FRAMEBUFFER" in glh))
        checks.append((
            "L2: glfbo.c implements glBlitFramebuffer",
            "void glBlitFramebuffer" in glfbo))
        checks.append((
            "L2: test_glfbo.c covers blit",
            "glBlitFramebuffer" in read("tests", "unit", "test_glfbo.c")))
        checks.append((
            "L2: docs/opengl.md no longer lists copy/blit as unimplemented",
            "Render into the texture directly with an FBO instead"
            not in opengl))

    # --- L3: COMBINE + texture matrix + 4 units -----------------------
    if l3:
        checks.append((
            "L3: GL_MAX_TEXTURE_UNITS_IMPL is 4",
            re.search(r"#define\s+GL_MAX_TEXTURE_UNITS_IMPL\s+4\b",
                      glctx) is not None))
        checks.append((
            "L3: gl.h defines GL_COMBINE",
            "#define GL_COMBINE" in glh))
        checks.append((
            "L3: glMatrixMode(GL_TEXTURE) is no longer GL_INVALID_OPERATION",
            "no texture matrix yet" not in glmatrix))
        checks.append((
            "L3: test_gltex2.c covers COMBINE",
            "GL_COMBINE" in read("tests", "unit", "test_gltex2.c")))
        checks.append((
            "L3: docs/opengl.md no longer lists COMBINE / units / "
            "texture matrix as unimplemented",
            "The GL 1.3 programmable combiner is absent" not in opengl and
            "More than 2 texture units" not in opengl and
            "glMatrixMode(GL_TEXTURE)` reports `GL_INVALID_OPERATION`"
            not in opengl))

    # --- structural: status header vs table ---------------------------
    done_rows = len(re.findall(
        r"^\|\s*L\d+\s+—[^|]*\|\s*✅ complete", plan, re.M))
    done_heads = sum(1 for p in PHASE_ORDER if phase_done(plan, p))
    checks.append((
        "plan: every complete row has a COMPLETE heading",
        done_rows == done_heads and plan != ""))

    header_line = re.search(r"^## Status:.*$", plan, re.M)
    header = header_line.group(0) if header_line else ""
    status_ok = False
    if re.search(r"^## Status: PLANNED", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        status_ok = (0 < done_rows < len(PHASE_ORDER))
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == len(PHASE_ORDER))
    checks.append((
        "plan: the Status header agrees with the table (%d/%d ✅, %r)"
        % (done_rows, len(PHASE_ORDER), header),
        status_ok))
    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(ok for _, ok in results):
            print("check_gl2_claims: SELFTEST inconclusive (tree "
                  "already red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(ok for _, ok in doctored):
            print("check_gl2_claims: SELFTEST FAIL -- passes against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_gl2_claims: selftest PASS (doctored tree "
              "detected)")
        return 0

    results = claims()
    bad = [name for name, ok in results if not ok]
    for name in bad:
        print(f"check_gl2_claims: FAIL -- {name}", file=sys.stderr)
    if bad:
        print(f"check_gl2_claims: {len(bad)} claim(s) disagree "
              f"with the tree", file=sys.stderr)
        return 1
    print(f"check_gl2_claims: OK -- {len(results)} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
