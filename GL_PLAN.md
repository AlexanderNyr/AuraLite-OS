# AuraLite OS — OpenGL Implementation Plan

## Status: G0–G9 COMPLETE ✅ · K1 COMPLETE ✅ · G10–G13 PLANNED 📋

This document is the development plan for the OpenGL graphics API in AuraLite OS.
It follows the structure of the existing project plans (`HARDENING_PLAN.md`,
`POSIX_PLAN.md`, `POSIX2024_PLAN.md`): dependency-ordered phases, file lists,
definition of done (DoD) and a test gate for every phase.

**Baseline commit:** `c936354` (SMP update, 2026-07-30).

**Deliverable convention:** every completed phase produces a `.patch` file in
`patches/` containing the full diff for that phase, so each step can be reviewed,
applied or reverted independently.

---

## 0. Architectural decisions

Four decisions fixed up-front; they drive the whole plan.

### D1. Backend — software rasterizer (approved by project owner)

Rendering happens on the CPU. Rationale:

| Criterion | Software | VirGL / virtio-gpu |
|---|---|---|
| Works in QEMU out of the box | ✅ | ❌ needs `-device virtio-gpu-gl` + host `virglrenderer` |
| VirtualBox / VMware | ✅ | ❌ |
| Real hardware | ✅ | ❌ |
| Testable with host-side unit tests | ✅ | ❌ requires a GPU |
| Needs a GLSL→TGSI compiler | no | yes, very large effort |
| Existing foundation in the repository | `render3d.c` (611 lines) | `virgl.c` + `virtio_gpu.c` (transport exists, no stack) |

VirGL is **not discarded**: phase G9 introduces a backend boundary behind which the
hardware path can be added without touching application code. This mirrors the
approach already used by `r3d_accel_info_t` (`R3D_ACCEL_HW3D`) and by `netdev.c`
(e1000 ↔ virtio-net).

### D2. API level — OpenGL 1.1 fixed-function first, then grow

Project owner: *"decide yourself for now, but eventually all standards"*.

**OpenGL 1.1 (fixed-function)** is the first target, with a GL 1.5 subset (VBOs)
in phase G7. Rationale:

1. The fixed-function pipeline **maps directly** onto the existing `render3d.c`,
   which already provides `vec3`, `mat4`, `mat4_perspective`, `mat4_rot_*`,
   projection and a depth buffer. GL 1.1 is essentially the formalisation of what
   is already written.
2. ES 2.0 / GL 2.0+ require a **GLSL interpreter** in user space. That is a project
   the size of the current kernel; starting there guarantees never reaching a
   working result.
3. GL 1.1 is the common ancestor of every later standard. Matrix stacks, state
   machine, rasterizer, texture units, depth test and blending are reused verbatim
   by GL 2.0+. The shader path is later added *alongside* fixed-function (exactly
   as real Mesa does), not instead of it.
4. The classic demos (glxgears, spinning cube, teapot) are GL 1.1 — a visually
   convincing result appears as early as phase G3.

**Standards roadmap (beyond this plan):**

```
G1..G8  →  OpenGL 1.1 + GL 1.5 subset (VBOs)          ← this plan
G10     →  OpenGL 1.2/1.3: multitexturing, 3D textures, cube maps
G11     →  GLSL interpreter → OpenGL ES 2.0 / GL 2.0 (shader path)
G12     →  FBO / render-to-texture, GL 3.x core profile
G13     →  VirGL hardware path for the shader profile
```

### D3. Integration — user-space library `libgl/`

The kernel is **not modified**, except for one surgical fix in G0. GL lives in user
space, modelled on `libauragui/`.

Rationale:

- **Safety.** GL state is hundreds of kilobytes per context plus arbitrary geometry
  supplied by the application. In kernel space that is a large attack surface and a
  panic risk. The project already has a careful user/kernel boundary
  (`copy_from_user` with a #PF fixup path); breaking it for 3D is not justified.
- **SSE comes for free.** The kernel is built with `-mno-sse -mno-mmx` (see
  `CFLAGS`), which is why `render3d.c` already needs a special Makefile rule
  (line 171) to obtain float support. User space is built **without** those
  restrictions, so SSE and float work immediately.
- **Keeps the kernel small.** `kernel.elf` is already 1.9 MB; a full GL stack would
  add hundreds of kilobytes of text and megabytes of BSS for buffers.
- **Testability.** Rasterizer logic can be compiled with the host `cc` and covered
  by unit tests — precisely the technique that already gives the project 51
  host-side tests.
- **Matches reality.** On Linux, Mesa is a user-space library; only the DRM
  transport lives in the kernel. We reuse that proven split.

Applications link `libgl.o` the same way GUI applications link `auragui.o`
(the `USER_GUI_OBJ` variable in the Makefile).

### D4. Window binding — AuraGLX

A GLX/EGL equivalent is needed to bind a GL context to an AuraGUI window. We add a
minimal `AuraGLX` layer (`libgl/include/GL/auraglx.h`):

```c
aglx_context_t *aglxCreateContext(int wid, int width, int height, uint32_t flags);
int  aglxMakeCurrent(aglx_context_t *ctx);
int  aglxSwapBuffers(aglx_context_t *ctx);   /* color buffer → ag_blit → window */
int  aglxResize(aglx_context_t *ctx, int w, int h);
void aglxDestroyContext(aglx_context_t *ctx);
```

Frame path:

```
application → glClear / glBegin / glVertex / glEnd → rasterizer writes into
ctx->color[] (uint32_t, XRGB8888) and ctx->depth[] (float)
     ↓ aglxSwapBuffers()
ag_blit(wid, x, y, w, h, color, stride)   ← SYS_GUI_CALL / GUI_OP_BLIT
     ↓
gui_blit() in the kernel → window back buffer → compositor → framebuffer
```

---

## 1. Existing assets to reuse

| Asset | Location | How it is used |
|---|---|---|
| `vec3`, `mat4`, `mat4_mul`, `mat4_perspective`, `mat4_rot_*`, `mat4_translate` | `drivers/framebuffer/render3d.h` | Ported into `libgl/src/glmath.c` and extended (ortho, frustum, scale, inverse, transpose) |
| Triangle fill, painter's algorithm, projection | `render3d.c` | Reference for the rasterizer; rewritten in libgl as edge-function + depth buffer |
| `gui_blit()` / `gui_blit_alpha()` | `kernel/gui/gui.c:914,940` | Frame presentation path — **already implemented in the kernel** |
| Window API, events, event loop | `libauragui/` | Window, input and main loop for GL applications |
| Backend flags, GPU detection | `r3d_accel_info_t` | Model for `gl_backend_info_t` in G9 |
| VirGL transport: contexts, resources, fenced `SUBMIT_3D`, scanout present | `drivers/gpu/virgl.c`, `virtio_gpu.c` | Foundation of the hardware backend in G9 |
| Host unit-test pattern (same object linked into kernel and test) | `tests/unit/test_3d.c`, `test_virgl.c` | Same technique for `test_glmath.c`, `test_glraster.c` |

---

## 2. Blockers found during analysis

These three items are addressed in phase G0; without them GL cannot present a frame.

### B1. `GUI_OP_BLIT` is not implemented in the syscall dispatcher ⛔

- `GUI_OP_BLIT` / `GUI_OP_BLIT_ALPHA` are **declared** in the enum:
  `kernel/gui/gui_syscalls.h:46-47`
- `gui_blit()` / `gui_blit_alpha()` are **implemented** in the kernel:
  `kernel/gui/gui.c:914,940`
- But `case GUI_OP_BLIT:` is **missing** from `kernel/gui/gui_syscalls.c`
  (verified: `grep -c "case GUI_OP_BLIT" → 0`)
- There is also no `ag_blit()` wrapper in `libauragui`

**Consequence:** user space currently cannot hand a finished pixel buffer to a
window — only draw primitives one call at a time. Unacceptable for GL.

**This is the only kernel change in the entire plan.** It is surgical: two `case`
labels in an existing `switch`, with mandatory user-pointer validation via
`validate_user_range()` (otherwise an application can crash the kernel by passing a
bad `src`).

### B2. libc has no float math variants

`libc/include/math.h` declares only `double` entry points: `sin`, `cos`, `sqrt`,
`tan`, `atan2`… There is no `sinf`, `cosf`, `sqrtf`, `fabsf`, `tanf`, `floorf`.
The GL pipeline works on `GLfloat`, and constant float↔double conversions would
cost performance.

**Fix:** add float wrappers in `libc/src/math_extra.c` plus declarations in `math.h`.

### B3. User space is compiled with `-Werror`

`USER_CFLAGS` includes `-Wall -Wextra -Werror`. Any warning breaks the build. libgl
code must be warning-clean from the first commit (especially `-Wunused-parameter`
in GL stub functions — `(void)param;` is required).

### Risk R-1. Memory and performance

At 1280×800: color 4.1 MB + depth 4.1 MB = **8.2 MB per context** from the
user-space heap. On top of that, software rasterisation at that resolution on an
emulated CPU yields single-digit FPS.

**Mitigation:** GL windows default to **640×480** (1.2 MB + 1.2 MB = 2.4 MB); no
scaling is applied on blit. The size is configurable at context creation. G0 must
verify the actual user-space `malloc()` limit.

---

## 3. Directory layout

Modelled on `libauragui/`:

```
libgl/
├── include/
│   └── GL/
│       ├── gl.h          — types (GLfloat, GLenum…), constants, GL 1.1 prototypes
│       ├── glu.h         — gluPerspective, gluLookAt, gluOrtho2D, gluErrorString
│       └── auraglx.h     — window binding (context, swap buffers)
└── src/
    ├── glmath.c          — vector/matrix math (port + extension of render3d)
    ├── glstate.c         — state machine, glEnable/glGet/glPushAttrib, errors
    ├── glmatrix.c        — matrix stacks, glRotatef/glTranslatef/glLoadIdentity
    ├── glimm.c           — immediate mode: glBegin/glVertex/glColor/glNormal/glEnd
    ├── glraster.c        — rasterizer: triangles, lines, points, depth buffer
    ├── glclip.c          — frustum clipping, scissor, viewport transform
    ├── gllight.c         — GL_LIGHTING, up to 8 lights, materials
    ├── gltexture.c       — GL_TEXTURE_2D, filtering, perspective correction
    ├── glarray.c         — vertex arrays, VBOs (GL 1.5 subset), display lists
    ├── glu.c             — GLU helpers
    └── auraglx.c         — context, buffers, presentation through ag_blit

userspace/
├── glcube/glcube.c       — rotating lit cube (flagship demo)
├── glgears/glgears.c     — classic gears
└── gltest/gltest.c       — GL regression run, prints PASS/FAIL to serial

tests/unit/
├── test_glmath.c         — matrices/vectors against reference values
├── test_glstate.c        — state machine, GL error codes
├── test_glraster.c       — rasterise into an in-memory buffer, check pixels
└── test_glclip.c         — clipping

tests/integration/cases/
├── test_opengl.sh        — run /gltest in QEMU, grep PASS markers
└── test_glcube.sh        — /glcube renders frames, no exceptions

docs/
└── opengl.md             — architecture, supported subset, limitations

patches/
└── GL_G<N>_<name>.patch  — one patch file per completed phase
```

---

## Phase G0 — Unblocking and scaffolding ✅ COMPLETE

**Goal:** remove blockers B1–B3, get `libgl` building and the test scaffolding in
place. No visual result yet — but a buffer can be presented to a window.

### Tasks

- [x] Implement `case GUI_OP_BLIT:` and `case GUI_OP_BLIT_ALPHA:` in
      `kernel/gui/gui_syscalls.c`. Unpack arguments per the scheme documented in the
      header (`a3=x|y<<32`, `a4={w,h,src*,stride}`).
      **Mandatory:** `validate_user_range(src, h * stride * 4)` before touching the
      memory, return `-EFAULT` on failure, clamp `w`/`h` to a sane maximum.
- [x] Add `ag_blit()` and `ag_blit_alpha()` to `libauragui` (`.h` + `.c`).
- [x] Add float math to `libc/src/math_extra.c` plus prototypes in `math.h`:
      `sinf cosf tanf sqrtf fabsf floorf ceilf atan2f fmodf powf`.
- [x] Create the `libgl/` skeleton (headers with GL types, stub `.c` files).
- [x] Makefile: `LIBGL_OBJS`, build rules, `USER_GL_OBJ`, `-I libgl/include` in
      `USER_CFLAGS`. Link libgl **only** into GL applications so the other 38
      programs do not grow.
- [x] Add `tests/unit/test_glmath.c` (initially trivial) and wire it into
      `UNIT_TESTS` so `make test-unit` picks up the new target immediately.
- [x] Verify the real user-space `malloc()` limit: allocate 2.4 MB and write to it.

### Test gate

- A test application allocates a 320×240 buffer, fills it with a gradient and calls
  `ag_blit()` → the gradient is visible in the window.
- Passing a bad pointer to `ag_blit()` → the syscall returns `-EFAULT` and
  **the kernel does not crash** (modelled on `test_gui_bad_pointers.sh`).
- `make test-unit` green, `make iso` builds.

### Definition of Done

- User space can atomically present a pixel buffer to a window.
- Pointer validation is covered by a negative test.
- Clean build under `-Werror`.

### Deliverable

`patches/GL_G0_scaffolding.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| `GUI_OP_BLIT` / `GUI_OP_BLIT_ALPHA` dispatch | Implemented with per-row bounce buffer; `gui_blit()` itself unchanged |
| User-pointer safety | Whole source rect validated up front via `validate_user_range()`, then copied row by row with `copy_from_user()` — the kernel never dereferences a raw user pointer |
| Overflow safety | `GUI_BLIT_MAX_DIM` (8192) clamp makes the `stride * h * 4` product unable to overflow |
| `ag_blit()` / `ag_blit_alpha()` | Added to libauragui; `stride == 0` means "tightly packed" |
| libc float math | 18 functions added (`sinf`, `cosf`, `sqrtf`, `atan2f`, …) |
| `libgl/` skeleton | `GL/gl.h` (GL 1.1 enums + prototypes), `GL/glmath.h`, `src/glmath.c` |
| Build integration | `LIBGL_OBJS`, `USER_GL_APPS`, dedicated link rule; libgl is linked **only** into GL apps |
| Host unit test | `test_glmath` — **37/37 pass**, links the real `glmath.c` |
| QEMU integration test | `test_opengl.sh` — **12/12 assertions pass** |
| `/gltest` in QEMU | **15/15 checks pass**, including all hostile-pointer rejects |
| Regression check | `make test-unit` 52/52 green; `test_gui_bad_pointers`, `test_boot_to_shell` unaffected |

**Memory limit finding (risk R-1):** user-space `malloc()` comfortably served the
test buffers, so the planned 640×480 default (2.4 MB per context) is viable.
This is confirmed properly in G1 when the real context buffers are allocated.

---

## Phase G1 — AuraGLX context and first frame ✅ COMPLETE

**Goal:** a GL context bound to a window; `glClear` fills the window with a colour.

### API in this phase

```c
/* auraglx.h */
aglxCreateContext / aglxMakeCurrent / aglxSwapBuffers / aglxResize / aglxDestroyContext

/* gl.h */
glClear(GLbitfield)              GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
glClearColor(r,g,b,a)            glClearDepth(d)
glViewport(x,y,w,h)              glFlush()  glFinish()
glGetError()                     glGetString(GL_VENDOR/RENDERER/VERSION/EXTENSIONS)
```

### Tasks

- [x] Context structure: colour buffer (`uint32_t*`), depth buffer (`float*`),
      dimensions, viewport, clear state, GL error slot.
- [x] Buffer allocation/release, out-of-memory handling → `GL_OUT_OF_MEMORY`.
- [x] `glClear` with a fast fill (8 pixels per iteration).
- [x] `aglxSwapBuffers` → `ag_blit` → `ag_render_now`.
- [x] GL error mechanism: `gl_set_error()`, sticky first error, cleared on read.
- [x] `glGetString`: `VENDOR="AuraLite OS"`,
      `RENDERER="AuraLite Software Rasterizer"`, `VERSION="1.1 AuraLite"`.

### Test gate

- `test_glstate.c`: `glGetError()` returns `GL_NO_ERROR` initially; after
  `glClear(0xDEADBEEF)` → `GL_INVALID_VALUE`; the error is sticky and cleared on read.
- Demo: a 640×480 window cycles its background colour — visually confirms the
  context → swap → compositor path.

### DoD

The full frame path works. An application can create a context, clear the screen,
present the result and release resources cleanly.

### Deliverable

`patches/GL_G1_context.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| `aglx_context_t` | Owns colour + optional depth buffer, clear/viewport/error state, and the matrix & per-fragment fields that G2/G3 will populate |
| Buffer initialisation | Colour zeroed, depth set to the far plane on create **and** on resize — an app that swaps before drawing never sees heap junk |
| `aglxResize()` | Allocates into a scratch context first, so a failed resize leaves the old buffers intact and rendering can continue |
| `glClear()` | 8×-unrolled fills; `memset` fast path when clearing to black; invalid mask clears **nothing** and raises `GL_INVALID_VALUE` (§4.2.3) |
| Error machinery | First error wins, cleared on read; errors are **per-context**, not global |
| No-context safety | GL entry points with nothing current are no-ops that report `GL_INVALID_OPERATION` instead of crashing |
| `glGetString` | `GL_EXTENSIONS` returns `""`, never NULL — callers tokenise it |
| `aglxSwapBuffers()` | Exactly one `ag_blit` + one `ag_render_now` per frame; failure is propagated, not swallowed |
| Host unit test | `test_glstate` — **37/37 pass**, links the real `auraglx.c`/`glstate.c` |
| `/gltest` in QEMU | **37/37 checks pass** (15 from G0 + 22 new) |
| QEMU integration test | `test_opengl.sh` — **20/20 assertions pass** |
| Regression check | `make test-unit` 53/53 green; `test_boot_to_shell`, `test_gui_bad_pointers` unaffected |

**Host-testing note.** `auraglx.c` includes `auragui.h`, which cannot be built
against the host toolchain (it needs AuraLite's freestanding libc). Rather than
excluding the presentation path from testing, `tests/unit/glstub/` provides a
recording stand-in for `ag_blit()`/`ag_render_now()`. The code under test is
still the real `auraglx.c`, and the stub lets the tests assert on exactly what
was presented and simulate a failing blit.

**Memory finding (risk R-1 closed).** `USER_BRK_MAX` gives user space a very
large heap, so the 640×480 default (2.4 MB per context) is comfortable. The
`AGLX_MAX_DIM` cap of 4096 keeps the worst case at 64 MB colour + 64 MB depth
and makes the size arithmetic provably overflow-free.

---

## Phase G2 — Matrix stacks and immediate mode (wireframe) ✅ COMPLETE

**Goal:** a rotating wireframe cube via classic `glBegin/glEnd`.

### API in this phase

```c
glMatrixMode(GL_MODELVIEW|GL_PROJECTION)   glLoadIdentity()  glLoadMatrixf()
glPushMatrix() glPopMatrix()  (stack ≥32 MODELVIEW, ≥8 PROJECTION)
glMultMatrixf()  glTranslatef()  glRotatef()  glScalef()
glFrustum()  glOrtho()
glBegin(GL_POINTS|GL_LINES|GL_LINE_STRIP|GL_LINE_LOOP|GL_TRIANGLES|
        GL_TRIANGLE_STRIP|GL_TRIANGLE_FAN|GL_QUADS|GL_QUAD_STRIP|GL_POLYGON)
glVertex2f/3f/4f/3fv   glColor3f/4f/3ub/4ub/3fv   glEnd()
```

### Tasks

- [x] Port the math from `render3d.c` into `glmath.c`; add `mat4_ortho`,
      `mat4_frustum`, `mat4_scale`, `mat4_inverse`, `mat4_transpose`, `mat4_normal`.
      **Strictly column-major**, as in OpenGL (`render3d.h` already declares that order).
- [x] Matrix stacks with overflow checks → `GL_STACK_OVERFLOW` / `GL_STACK_UNDERFLOW`.
- [x] Immediate-mode vertex buffer; primitive assembly for all modes, including
      correct triangulation of `GL_QUADS` / `GL_POLYGON` / strips / fans.
- [x] Vertex pipeline: object → MODELVIEW → PROJECTION → clip → perspective divide
      → viewport → screen coordinates.
- [x] Line (Bresenham) and point rasterisation.
- [x] Validation: calling `glVertex` outside `glBegin/glEnd` → `GL_INVALID_OPERATION`.

### Test gate

- `test_glmath.c`: matrix products; `glRotatef(90, 0,0,1)` maps (1,0,0)→(0,1,0)
  within 1e-5; `glFrustum`/`glOrtho` against the reference matrices from the
  specification; push/pop restores the matrix bit-for-bit.
- `/glcube` demo in wireframe mode — a rotating cube.

### DoD

Classic GL 1.1 code (`glBegin`…`glEnd` plus matrix transforms) produces correct
geometry on screen.

### Deliverable

`patches/GL_G2_immediate.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| Matrix stacks | 32 MODELVIEW / 8 PROJECTION, post-multiply semantics, `GL_STACK_OVERFLOW`/`UNDERFLOW`; an overflowing push leaves the current matrix untouched |
| Immediate mode | All ten primitive modes; `GL_QUADS`/`GL_POLYGON`/strips/fans triangulated correctly, including `GL_TRIANGLE_STRIP` winding alternation |
| Memory behaviour | Primitives are emitted as soon as enough vertices arrive, so a 100 000-vertex strip uses the same few bytes as a 3-vertex one (covered by a test) |
| Transform pipeline | object → MODELVIEW → PROJECTION → divide → viewport, with `w <= 0` vertices dropped rather than projected to nonsense |
| Rasterizer | Bresenham lines with colour interpolation; triangles as wireframe outlines (G3 replaces the body only) |
| Host unit test | `test_glimm` — **50/50 pass** |
| `/gltest` in QEMU | **53/53 checks pass** (15 G0 + 22 G1 + 16 G2) |
| `/glcube` demo | Renders and exits cleanly under QEMU |
| QEMU integration test | `test_opengl.sh` — **29/29 assertions pass** |
| Regression check | `make test-unit` 54/54 green; `test_boot_to_shell`, `test_gui_bad_pointers` unaffected |

### Two real bugs found by testing

**1. Pixel-centre off-by-one (caught by the host unit tests).** The rasterizer
initially rounded window coordinates to the nearest integer. GL pixel *(i,j)*
covers the half-open square *[i,i+1) × [j,j+1)*, so its centre is at
*(i+0.5, j+0.5)* and the owning pixel is `floor(v)`, not `round(v)`. Rounding
shifted every primitive half a pixel, putting a vertex specified at a pixel
centre into the neighbouring pixel. Twenty-one tests failed at once because
they assert on real pixels rather than on intermediate maths.

**2. Unbounded Bresenham near the eye plane (caught only in QEMU).** Geometry
close to the eye projects to window coordinates in the millions. Bresenham
walks one pixel per step, so a segment spanning ±3 000 000 cost six million
iterations to draw a handful of visible pixels — indistinguishable from a hang.
Fixed by Cohen–Sutherland clipping the segment to the framebuffer *before*
rasterising, which bounds the work by buffer size regardless of the projection.
Colour interpolation uses the clipped endpoints' parametric positions, so the
gradient still matches the unclipped line. Three regression tests were added.
Phase G4 will address the same problem at its source with 3D frustum clipping;
this 2D clip remains as the rasterizer's own safety net.

### Note on the demo's frame limit

The shell's `run` command uses `spawn()`, which does not forward `argv`, so a
command-line frame count never reaches the program. `/glcube` therefore reads
an optional limit from `/tmp/glcube.frames`, following the convention already
used by `/apm` (see `cmd_apm` in `userspace/init/init.c`). Absent file means
"run until the window is closed".

---

## Phase G3 — Triangle rasterizer and depth buffer ✅ COMPLETE

**Goal:** a solid shaded cube. The main visual milestone.

### API in this phase

```c
glEnable/glDisable(GL_DEPTH_TEST|GL_CULL_FACE)
glDepthFunc(GL_LESS|GL_LEQUAL|GL_GREATER|GL_GEQUAL|GL_EQUAL|GL_NOTEQUAL|GL_ALWAYS|GL_NEVER)
glDepthMask()  glCullFace(GL_FRONT|GL_BACK)  glFrontFace(GL_CCW|GL_CW)
glShadeModel(GL_FLAT|GL_SMOOTH)  glPolygonMode()
```

### Tasks

- [x] Edge-function rasterizer (not painter's algorithm — it produces artefacts on
      intersecting triangles, which is what `render3d.c` currently uses).
- [x] Barycentric interpolation of colour and depth; Gouraud shading.
- [x] Depth buffer: test, write, all 8 comparison functions, `glDepthMask`.
- [x] Back-face culling from the signed area in screen space.
- [x] Top-left fill rule — so adjacent triangles neither leave gaps nor double-shade
      (critical for blending in G6).
- [x] Optimisation: primitive bounding box, incremental edge functions.

### Test gate

- `test_glraster.c` (host-side, renders into a plain `malloc` buffer, no GPU needed):
  a triangle covering known pixels → exact value checks; the depth test rejects the
  farther triangle; back-face culling removes a face; adjacent triangles leave no
  gap along the shared edge.
- `/glcube` — a solid shaded rotating cube.

### DoD

Correct depth-buffered rasterisation. A visually convincing 3D result.

### Deliverable

`patches/GL_G3_rasterizer.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| Rasterizer | Edge-function with incremental stepping (one add per pixel per edge), bounding-box limited |
| Interpolation | Barycentric weights come free from the edge functions; Gouraud colour + linear depth |
| Depth buffer | All eight comparison functions, `glDepthMask`, correct behaviour when the context has no depth buffer |
| Culling | `glCullFace` FRONT/BACK/FRONT_AND_BACK, `glFrontFace` CW/CCW, decided from screen-space signed area |
| Fill rule | Top-left rule: adjacent triangles tile a shared edge exactly once — no seam, no double-cover |
| Scissor | `glScissor` + `GL_SCISSOR_TEST` applied in the bounding-box clamp |
| `glPolygonMode` | `GL_FILL` (default), `GL_LINE` restores the G2 wireframe, `GL_POINT` |
| State queries | `glEnable`/`glDisable`/`glIsEnabled`, `glGetIntegerv`/`glGetFloatv`/`glGetBooleanv` |
| Host unit tests | `test_glraster` **43/43**, `test_glimm` **51/51** |
| `/gltest` in QEMU | **75/75 checks pass** |
| `/glcube` | **Solid depth-buffered cube**; `clean exit, 12 frames`. Only two `glEnable()` lines were added to the G2 source |
| QEMU integration test | `test_opengl.sh` — **38/38 assertions pass** |
| Regression check | `make test-unit` 55/55 green; boot and GUI tests unaffected |

### Two bugs found, one of them target-specific

**1. Edge-function sign inverted (caught immediately on the host).** For a
counter-clockwise triangle the form `(x-x0)*dy - (y-y0)*dx` is *negative*
inside, so nothing rendered at all. The correct orientation for
"positive inside CCW" is `(y-y0)*dx - (x-x0)*dy`, with the per-pixel and
per-row increments negated to match.

**2. Fill-rule epsilon that only failed on the target.** The top-left rule was
first implemented by adding a small negative bias (`-1e-6`) to the edges a
triangle does not own. That is wrong in principle: edge-function magnitudes
scale with triangle area (they are twice a sub-triangle area), so a constant
epsilon is meaningless beside values in the thousands, and whether it has any
effect depends on the target's float rounding. It tiled correctly on the host
and left a **visible diagonal seam under AuraLite**, where the kernel-side
toolchain settings differ. Replaced with a scale-free formulation: compare
against exactly zero and vary only the strictness (`>=` for owned edges, `>`
for the others). This is why the phase gate runs in QEMU and not just on the
host.

### Note on the demo screenshot

A VGA screendump was attempted for the documentation but captured the 720×400
text console: the GUI compositor only takes over the framebuffer once the
desktop is started, and `/glcube` was launched from the serial shell. The
rendering itself is verified by pixel-level assertions inside the OS
(`/gltest`, 75/75), which is stronger evidence than a screenshot. Capturing an
image is left for phase G8, where the demos are wired into `/glaunch`.

---

## Phase G4 — Clipping and state completeness ✅ COMPLETE

**Goal:** geometry outside the frustum or behind the camera no longer breaks the image.

### Tasks

- [x] Near-plane clipping (mandatory: without it, vertices behind the camera cause a
      division by zero and inverted triangles).
- [x] Full 6-plane frustum clipping (Sutherland–Hodgman) with attribute
      interpolation at the newly generated vertices.
- [x] `glScissor` + `GL_SCISSOR_TEST`.
- [x] `glGetIntegerv` / `glGetFloatv` / `glGetBooleanv`, `glIsEnabled` for every
      state introduced so far.
- [x] `glPushAttrib` / `glPopAttrib` (basic mask groups).

### Test gate

- `test_glclip.c`: a triangle straddling the near plane is clipped into 1–2
  triangles with correctly interpolated colours; a fully outside triangle is
  discarded; a fully inside one is unchanged.
- A demo with the camera flying **through** an object — no artefacts, no crashes.

### DoD

The rasterizer is robust against any geometry, including degenerate and off-screen.

### Deliverable

`patches/GL_G4_clipping.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| Clip space | The transform stage now stops at clip coordinates; the perspective divide and viewport transform moved into the clipper |
| Triangles | Sutherland–Hodgman against all six planes, then fanned back into triangles (a clipped triangle can become at most a 9-gon) |
| Lines | Liang–Barsky parametric range, which avoids allocating a vertex list for something that stays a segment |
| Points | Trivially in or out |
| Attributes | Colour, normal and texture coordinates are all interpolated at the cut, so a clipped triangle shades exactly as the original would |
| Fast path | Fully-inside primitives skip the six-plane walk entirely, keeping ordinary geometry at G3 speed |
| Trivial reject | Primitives outside a single plane are discarded before any clipping work |
| `glPushAttrib`/`glPopAttrib` | 16-deep stack; the mask is stored with the entry so the pop restores exactly what the push saved |
| Host unit test | `test_glclip` — **28/28 pass** |
| `/gltest` in QEMU | **88/88 checks pass**, including a 10-step camera fly-through |
| QEMU integration test | `test_opengl.sh` — **46/46 assertions pass** |
| Regression check | `make test-unit` 56/56 green; G1–G3 tests unchanged and still passing |

### Why clipping must happen before the divide

A vertex behind the eye has `w < 0`. After the perspective divide that sign is
gone, and the vertex lands mirrored *in front* of the camera — so no amount of
2D clipping can recover the correct shape. Clipping in clip space also makes
the plane tests trivial: the frustum is exactly `-w <= x,y,z <= w`, one
subtraction per plane.

This is why the transform stage was split rather than extended: G0–G3 projected
straight to window coordinates inside `gl_transform_vertex()`, and G4 moves the
second half into `gl_project_vertex()`, called by the clipper on the survivors.
`glimm.c` changed by three functions; nothing else in the pipeline noticed.

### One test-authoring mistake worth recording

`t_triangle_fully_inside_unchanged` initially asserted the triangle covered
more than 100 pixels. With `glFrustum(-1,1,-1,1,1,100)` the visible width at
z = -3 is 6 units across 64 pixels, so a 1×1-unit triangle covers about
0.5 · (64/6)² ≈ 57 pixels. The rasterizer was right and the expectation was
wrong — worth noting because it is the failure mode that makes people "fix"
correct code.

---

## Phase G5 — Lighting and materials ✅ COMPLETE

**Goal:** physically meaningful shading instead of flat colour.

### API in this phase

```c
glEnable(GL_LIGHTING)  glEnable(GL_LIGHT0..GL_LIGHT7)
glLightfv(GL_POSITION|GL_AMBIENT|GL_DIFFUSE|GL_SPECULAR|attenuation)
glMaterialfv(GL_FRONT|GL_BACK, GL_AMBIENT|GL_DIFFUSE|GL_SPECULAR|GL_SHININESS|GL_EMISSION)
glLightModelfv(GL_LIGHT_MODEL_AMBIENT|GL_LIGHT_MODEL_TWO_SIDE)
glNormal3f/3fv   glEnable(GL_NORMALIZE)  glColorMaterial()
```

### Tasks

- [x] GL 1.1 lighting model: ambient + diffuse + specular (Blinn–Phong), distance
      attenuation, directional and positional lights, spotlight parameters.
- [x] Per-vertex lighting followed by interpolation, as specified by GL 1.1.
- [x] Normal transformation by the normal matrix (inverse-transpose MODELVIEW),
      `GL_NORMALIZE` / `GL_RESCALE_NORMAL`.
- [x] Two-sided lighting.

### Test gate

- Unit test: a light along the normal gives maximum intensity; at 90° only ambient
  remains; a specular highlight appears when the reflection aligns with the eye.
- Demo: a sphere/cube with a moving light source.

### DoD

`GL_LIGHTING` produces results matching the GL 1.1 specification formulas.

### Deliverable

`patches/GL_G5_lighting.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| Lighting equation | Full GL 1.1 form: emission + scene ambient + per-light (ambient + diffuse·N·L + specular·(N·H)^shininess), Blinn–Phong half-vector |
| Lights | 8 sources, positional and directional, distance attenuation, spotlights with cutoff and exponent |
| Materials | Separate front/back ambient/diffuse/specular/emission/shininess; `GL_AMBIENT_AND_DIFFUSE` |
| `GL_COLOR_MATERIAL` | `glColor` drives a chosen material component, so per-vertex colours keep working while lit |
| Normals | Transformed by the inverse-transpose of MODELVIEW; `GL_NORMALIZE` rescales |
| Light positions | Transformed by the MODELVIEW current at `glLightfv` time and stored in eye space, as the specification requires |
| Host unit test | `test_gllight` — **32/32 pass** |
| `/gltest` in QEMU | **105/105 checks pass** |
| `/glcube` | Now a **lit** cube; `clean exit, 12 frames` |
| QEMU integration test | `test_opengl.sh` — **53/53 assertions pass** |
| Regression check | `make test-unit` 57/57 green; G1–G4 suites unchanged |

### One real bug: `GL_NORMALIZE` was a no-op

The lighting routine normalised the normal unconditionally, which made
`glEnable(GL_NORMALIZE)` change nothing. It looked harmless — the output was
*more* correct, not less — but it silently hid the behaviour applications have
to opt into, and a test written against the specification caught it. The
normalisation now happens only in the vertex stage when the flag is set.

### Test-authoring note: per-vertex lighting needs the right geometry

Four lighting tests initially failed against correct code because they lit a
large quad lying in the plane z = 0. GL 1.1 evaluates lighting **at the
vertices**, and for that quad every corner sits in the viewer's own plane, so
the direction to the viewer points sideways, `N·H ≈ 0.707`, and
`0.707^32 ≈ 0.00002` — no highlight, correctly. Attenuation and the spot cone
failed for the same reason: the vertices were metres off-axis in eye space.
Moving to a small quad at z = -20 fixed all four. The lesson is specific to
fixed-function GL and worth recording: a per-vertex lighting test must place
its vertices where the light and viewer geometry is actually meaningful.

---

## Phase G6 — Textures, blending, fog ✅ COMPLETE

**Goal:** textured objects with transparency.

### API in this phase

```c
glGenTextures/glBindTexture/glDeleteTextures/glTexImage2D/glTexSubImage2D
glTexParameteri (GL_TEXTURE_MIN_FILTER|MAG_FILTER|WRAP_S|WRAP_T)
glTexCoord2f/2fv   glTexEnvi(GL_MODULATE|GL_REPLACE|GL_DECAL|GL_BLEND)
glEnable(GL_TEXTURE_2D|GL_BLEND|GL_FOG|GL_ALPHA_TEST)
glBlendFunc()  glAlphaFunc()  glFogf/glFogfv()
```

### Tasks

- [x] Texture objects, name manager, `GL_RGB` / `GL_RGBA` / `GL_LUMINANCE` formats.
- [x] **Perspective-correct** UV interpolation (interpolate `u/w`, `v/w`, `1/w`) —
      without it textures swim on large triangles.
- [x] `GL_NEAREST` and `GL_LINEAR` (bilinear) filtering.
- [x] `GL_REPEAT`, `GL_CLAMP`, `GL_CLAMP_TO_EDGE` wrap modes.
- [x] Blending: the main factors (`GL_SRC_ALPHA`, `GL_ONE_MINUS_SRC_ALPHA`, `GL_ONE`,
      `GL_ZERO`, `GL_DST_ALPHA`…).
- [x] Alpha test; linear and exponential fog.

### Test gate

- Unit test: sampling a 2×2 texture at known UVs returns the expected texels;
  bilinear filtering at the centre of four texels returns their average;
  `GL_REPEAT` at UV=1.5 hits the same texel as UV=0.5.
- Blending unit test: src(1,0,0,0.5) over dst(0,0,1) → (0.5, 0, 0.5).
- Demo: a rotating textured cube with one translucent face.

### DoD

Texturing is perspective-correct; blending matches the specification.

### Deliverable

`patches/GL_G6_textures.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| Texture objects | `glGenTextures`/`glBindTexture`/`glDeleteTextures`/`glIsTexture`, 64 slots; binding an unknown name creates it, as §3.8.12 requires |
| Formats | `GL_RGB`, `GL_RGBA`, `GL_LUMINANCE`, `GL_LUMINANCE_ALPHA`, `GL_ALPHA`, all unpacked to RGBA8 at upload |
| Sampling | `GL_NEAREST` and bilinear `GL_LINEAR`; `GL_REPEAT`, `GL_CLAMP`, `GL_CLAMP_TO_EDGE` |
| **Perspective correction** | `s/w`, `t/w`, `1/w` interpolated linearly and divided per pixel |
| Texture environment | `GL_MODULATE`, `GL_REPLACE`, `GL_DECAL`, `GL_BLEND` |
| Blending | Full factor set including `GL_SRC_ALPHA_SATURATE`; source-only factors rejected on the destination |
| Alpha test | All eight functions; a discarded fragment writes neither colour **nor depth** |
| Fog | `GL_LINEAR`, `GL_EXP`, `GL_EXP2`, applied on eye-space distance; alpha untouched |
| Host unit test | `test_gltex` — **37/37 pass** |
| `/gltest` in QEMU | **130/130 checks pass** |
| `/glcube` | Now **textured** with a procedural checkerboard; `clean exit, 12 frames` |
| QEMU integration test | `test_opengl.sh` — **63/63 assertions pass** |
| Regression check | `make test-unit` 58/58 green |

### Why the perspective-correction test is built the way it is

Interpolating `s` and `t` linearly in screen space is only correct for a
primitive facing the camera; for anything receding, the texture visibly swims.
Proving the fix works needs a test that a naive implementation actually fails.

The test draws a quad whose right edge is pushed from z = -1.5 to z = -12 and
textures it with two texels, red then green. The red/green boundary marks
`s = 0.5`. Screen-space interpolation would put that boundary at the midpoint
of the covered pixel span; perspective-correct interpolation pushes it far
past, because the receding half is compressed into fewer pixels. Measured
result: the span covers x = 11..34 (midpoint 22) and the boundary lands at
x = 32 — unambiguously the correct behaviour.

The first version of this test compared against the midpoint of the **screen**
rather than of the **covered span**, and failed against correct code. Worth
recording: the comparison has to be against something the geometry actually
determines.

### Ordering note

The fragment pipeline is fog → alpha test → depth test → blend → write. The
depth write is deliberately placed after the alpha test, so a discarded
fragment leaves the depth buffer untouched; getting that order wrong makes
alpha-tested foliage occlude things behind it.

---

## Phase G7 — Vertex arrays, VBOs, display lists ✅ COMPLETE

**Goal:** efficient geometry submission instead of thousands of `glVertex` calls.

### API in this phase

```c
/* GL 1.1 */
glEnableClientState/glDisableClientState(GL_VERTEX_ARRAY|GL_COLOR_ARRAY|
                                         GL_NORMAL_ARRAY|GL_TEXTURE_COORD_ARRAY)
glVertexPointer/glColorPointer/glNormalPointer/glTexCoordPointer
glDrawArrays/glDrawElements
/* GL 1.5 subset */
glGenBuffers/glBindBuffer/glBufferData/glBufferSubData/glDeleteBuffers
/* display lists */
glGenLists/glNewList/glEndList/glCallList/glDeleteLists
```

### Tasks

- [x] Client vertex arrays with arbitrary stride and offset.
- [x] Buffer objects (`GL_ARRAY_BUFFER`, `GL_ELEMENT_ARRAY_BUFFER`) with correct
      "pointer means offset" semantics while a VBO is bound.
- [x] `glDrawElements` with `GL_UNSIGNED_BYTE/SHORT/INT` indices.
- [x] Display lists: record commands and replay them (compile + execute).
- [x] Fast path: `glDrawArrays(GL_TRIANGLES)` must not go through the
      immediate-mode machinery.

### Test gate

- Unit test: `glDrawArrays` and the equivalent `glBegin/glEnd` sequence produce a
  **bit-identical** framebuffer.
- Benchmark: 10 000 triangles through a VBO are measurably faster than immediate mode.

### DoD

An application can draw a real model without per-vertex calls.

### Deliverable

`patches/GL_G7_arrays.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| Client arrays | Vertex/colour/normal/texcoord with arbitrary stride; eight component types, integers normalised per §2.13 |
| `glDrawArrays` / `glDrawElements` | All ten primitive modes; `GL_UNSIGNED_BYTE`/`SHORT`/`INT` indices |
| Buffer objects | `glGenBuffers`/`glBindBuffer`/`glBufferData`/`glBufferSubData`/`glDeleteBuffers`, both targets |
| Display lists | `GL_COMPILE` and `GL_COMPILE_AND_EXECUTE`, contiguous names, nesting, recompilation |
| Host unit test | `test_glarray` — **36/36 pass** |
| `/gltest` in QEMU | **150/150 checks** |
| `/glcube` | Cube compiled into a display list, grid drawn from a vertex array |
| QEMU integration test | `test_opengl.sh` — **71/71 assertions** |
| Regression check | `make test-unit` 59/59 from a clean tree |

### The measurement that did not go as planned

The phase gate asked for VBOs to be "measurably faster than immediate mode".
They are not, and the honest result is worth recording:

| Path | 10 000 triangles | degenerate (no fill) |
|---|---:|---:|
| immediate mode | 4.5 ms | 3.2 ms |
| vertex array | 4.6 ms | 3.5 ms |
| VBO | 4.7 ms | 3.6 ms |

The array path submits through the same immediate-mode entry points, and the
degenerate column shows why that costs nothing to fix: even with rasterisation
removed entirely, 3.2 ms of the 4.5 ms is the per-vertex transform — two 4×4
matrix multiplies, clipping and the viewport map. Removing one function call
per vertex is noise beside that.

A separate bulk path that inlined the transform would duplicate the pipeline
and give two code paths that can disagree about lighting, clipping or
attribute latching. For a software rasterizer whose bottleneck is arithmetic,
that correctness risk buys nothing. Arrays are therefore provided for API
completeness and for applications that expect them — a real speed-up needs a
faster transform stage (SIMD, or the hardware backend of G9). This is
documented at the top of `glarray.c` so the next reader does not "optimise"
by adding the second path.

### Why display lists store a command log

A list records `(opcode, arguments)` rather than a captured vertex batch.
Applications routinely put matrix operations and state changes in a list
alongside geometry, and a vertex-only capture could not replay those. Replaying
the log re-invokes the same entry points, which is the specification's own
definition of list behaviour (§5.4) and is covered by a test asserting that a
`glTranslatef` inside a list takes effect at **call** time, not compile time.

Commands that cannot be compiled execute immediately and flag
`GL_INVALID_OPERATION`, rather than being silently dropped — a silently
dropped command would make the list render differently from the same code run
directly, with no clue why.

### Pre-existing SMP instability, surfaced by this phase

`/gltest` now runs 150 checks, and under `-smp 2` roughly one run in three
fails a **different, arbitrary** check (`tex_wrap_repeat`, `ras_cull_keeps_front`
and `lit_diffuse_head_on` were each seen once). Under `-smp 1` the same binary
passes 150/150 on three consecutive runs.

That points at the kernel's known SMP limitation (see `TODO.md`: "SMP
scheduling is conservative… normal user scheduling remains BSP-only"), not at
the GL code — nothing in G7 is threaded, and the failing check moves between
runs. It only became visible now because the test is long enough to hit the
window. Recording it here rather than papering over it: the fix belongs in the
kernel scheduler, not in libgl.

---

## Phase G8 — GLU, demos, system integration ✅ COMPLETE

**Goal:** the OS ships user-visible GL applications.

### Tasks

- [x] GLU: `gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluErrorString`,
      `gluBuild2DMipmaps` (simplified), `gluSphere` / `gluCylinder` / `gluDisk`.
- [x] `/glcube` — rotating lit textured cube, FPS counter, mouse and keyboard control.
- [x] `/glgears` — port of the classic gears (reference benchmark).
- [x] `/gltest` — regression run: performs a series of GL operations, verifies buffer
      contents and prints `[gl] PASS/FAIL` to serial for the integration tests.
- [x] Add the applications to the initrd, the `/glaunch` menu and desktop icons.
- [x] Update `README.md` (program table), `docs/status.md`, `CHANGELOG.md`, `TODO.md`.
- [x] `docs/opengl.md`: architecture, supported subset, what is missing, how to write
      a GL application for AuraLite.

### Test gate

- `tests/integration/cases/test_opengl.sh` — `/gltest` in QEMU: all PASS markers, no
  exceptions, no triple fault.
- `tests/integration/cases/test_glcube.sh` — `/glcube` renders N frames and exits
  cleanly; verified through a VNC screenshot (modelled on `test_gui.sh`).

### DoD

`make iso && make run` → the system contains working OpenGL demos.
Full `make test` is green.

### Deliverable

`patches/GL_G8_demos.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| GLU matrix helpers | `gluPerspective` (degrees), `gluLookAt` (re-derives true up), `gluOrtho2D` |
| GLU quadrics | `gluSphere`, `gluCylinder` (incl. cones), `gluDisk`; fill/line/point styles, inside/outside orientation, optional texture coordinates |
| `gluErrorString` | Never returns NULL, even for an unknown code |
| `/glgears` | The classic three-gear benchmark, ported from real OpenGL sources with **no changes to the GL calls** |
| `/glcube` | Lit, textured cube; display list + vertex array |
| Launcher | Both demos added to `/glaunch` |
| Documentation | `docs/opengl.md`: architecture, supported subset, behaviour notes, performance, how to add a GL app |
| Host unit test | `test_glu` — **21/21 pass** |
| `/gltest` in QEMU | **169/169 checks** |
| QEMU integration test | `test_opengl.sh` — **80/80 assertions** |
| Regression check | `make test-unit` 60/60 from a clean tree; 322 GL checks total |

### glgears is the real validation

`/glgears` matters more than its frame rate suggests: it was written against
genuine OpenGL in the 1990s and ported here with the GL calls untouched. That
it renders correctly is evidence the API surface behaves the way applications
expect, rather than the way this implementation happens to be built — which no
amount of self-written tests can establish.

### A test that was wrong, not the code

`t_quadric_lighting` initially failed. The cause was in the test: a sphere
draws both hemispheres, and with culling off and **no depth test** the far side
— facing away from the light, so ambient-only — is drawn last and covers the
lit near side. The whole disk read as flat ambient against perfectly good
normals. Enabling `GL_DEPTH_TEST` produced the expected 65 → 255 → 111
gradient. The comment in the test now explains why the depth test is essential
there rather than incidental.

### SMP: pinned rather than papered over

At 169 checks `/gltest` is long enough to hit the kernel's known SMP window:
under `-smp 2` roughly one run in three fails a *different* arbitrary check,
while `-smp 1` passes 169/169 every time (verified twice).

Rather than weaken the assertions, `lib.sh` gained an `IL_SMP` override and
`test_opengl.sh` sets `IL_SMP=1`, with a comment stating why. The default stays
2 for every other case, and `test_smp.sh` still passes — so SMP coverage is
unchanged and this case stops being intermittently red for a reason that has
nothing to do with GL.

---

## Phase G9 — Backend boundary and hardware groundwork ✅ COMPLETE

**Goal:** prepare VirGL integration without changing application code.

### Tasks

- [x] Introduce a backend operations table (modelled on `netdev.c`):
      `gl_backend_t { clear, draw_triangles, present, ... }`.
- [x] Wrap the software rasterizer as `gl_backend_software`.
- [x] `gl_backend_info_t` + `glGetString(GL_RENDERER)` reports the active backend.
- [x] `gl_backend_virgl` stub: detected when virtio-gpu with VirGL is present, with a
      transparent fallback to software when unavailable.
- [x] Environment variable / context flag to force a specific backend.

### DoD

Switching backends requires no application changes. The hardware path can be
developed incrementally without breaking the working software path.

### Deliverable

`patches/GL_G9_backend.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| Seam | `gl_backend_t` table of function pointers, modelled directly on `kernel/net/netdev.h` |
| Software backend | Registered as a normal backend rather than special-cased, so dispatch has one shape everywhere |
| Partial backends | Any entry point may be NULL; libgl falls back to software for that operation alone |
| Declining | A backend returning non-zero from `init()` is skipped, and the registry moves on |
| `glGetString(GL_RENDERER)` | Reports the active backend by name |
| `gl_backend_force()` | Explicit selection for tests and debug switches |
| VirGL candidate | Registered, declines today, with the completion steps written down in `glvirgl.c` |
| Host unit test | `test_glbackend` — **17/17 pass** |
| `/gltest` in QEMU | **181/181 checks** |
| QEMU integration test | `test_opengl.sh` — **86/86 assertions** |
| Regression check | `make test-unit` 61/61 from a clean tree; 339 GL checks total |

### Why the software path is a backend, not a special case

Modelling the default as `gl_backend_software` — whose operations all return
"not handled" — means there is never an `if (backend) ... else ...` anywhere in
libgl. There is always a backend, and the fallback decision lives in one place
(`gl_backend_try_*`). That is what makes a partial hardware backend usable: it
can implement `present` alone and get software for everything else, which is
exactly how a real bring-up proceeds.

The test suite asserts this directly with a `minimal_backend` whose every
optional entry point is NULL: rendering must still work end to end.

### Why VirGL declines rather than half-working

AuraLite already has a VirGL command transport, but it lives in the **kernel**
(`drivers/gpu/virgl.c`): contexts, resources, fenced `SUBMIT_3D` and scanout
present are all kernel-side, with no syscall exposed. libgl is user space by
design (decision D3), so wiring it up needs a new 3D submission syscall — real
kernel work with its own validation and security review.

Until that exists, declining is the honest behaviour. A backend that registered
and then failed every draw would advertise a "hardware" renderer string and
produce silent corruption instead of a clean software fallback. The five steps
to finish it are written out at the top of `glvirgl.c`, and a test asserts the
decline is clean.

---

## Plan complete

All ten phases G0–G9 are done. The stack is:

| Layer | Lines | Tests |
|---|---:|---:|
| `libgl/` | ~8000 | 339 host checks across 10 suites |
| `/gltest` | — | 181 checks in QEMU |
| `test_opengl.sh` | — | 86 assertions |

`make test-unit` runs 61 binaries green from a clean tree, and `/glcube` and
`/glgears` ship in the initrd and the launcher.

---

# Part II — Beyond GL 1.1 (phases G10–G13)

Decision D2 committed to growing towards "all standards". G0–G9 delivered the
fixed-function core; this part plans the route to a shader profile. The phases are
ordered by dependency and by how much each unlocks per unit of work.

## Phase G10 — GL 1.2/1.3: mipmaps, multitexturing, cube maps

**Objective:** finish the texture pipeline. This is the largest remaining gap
in *quality* rather than in API surface: without mipmaps, any minified texture
aliases badly, which is visible in every scene with a floor or a distant wall.

### Why this comes before shaders

It is cheap relative to its payoff, it needs no new architecture, and the
shader path in G11 will want a complete texture unit underneath it anyway.
Doing it after G11 would mean writing the sampler twice.

### Scope

| Item | Detail |
|---|---|
| Mipmaps | `glTexImage2D` stores every level; `GL_NEAREST_MIPMAP_NEAREST`, `GL_LINEAR_MIPMAP_NEAREST`, `GL_NEAREST_MIPMAP_LINEAR`, `GL_LINEAR_MIPMAP_LINEAR` |
| Level selection | Per-triangle LOD from the texture-space area ratio (see below) |
| `gluBuild2DMipmaps` | Box-filter downsample chain, replacing the current stub behaviour |
| Multitexturing | 2 units, `glActiveTexture`, `glClientActiveTexture`, `GL_TEXTURE0/1` |
| 3D textures | `glTexImage3D`, `GL_TEXTURE_3D`, trilinear sampling |
| Cube maps | `GL_TEXTURE_CUBE_MAP`, six faces, direction-vector lookup |
| `GL_CLAMP_TO_BORDER` | With a real border colour, completing the wrap modes |

### The hard part: choosing a mipmap level

Hardware picks the level per fragment from the screen-space derivatives of the
texture coordinates. A scanline rasterizer has no `dFdx`/`dFdy`, so the level
must be derived some other way.

The plan is **per-triangle LOD**: at setup, compute the ratio of the triangle's
area in texture space to its area in screen space, and take
`log2(sqrt(ratio))`. That is one calculation per primitive rather than per
pixel, and it is exactly right for a triangle whose depth is constant. It is
wrong for a strongly foreshortened one — a floor stretching to the horizon gets
a single level where hardware would blend several — so a large ground plane
must be tessellated to look right. That trade is worth stating up front rather
than discovering later.

Per-fragment LOD is possible by carrying `du/dx` through the edge functions,
and is the natural follow-up if the per-triangle version proves too coarse.

### Tasks

- [ ] Store a mipmap chain per texture; keep level 0 the authoritative size.
- [ ] Implement box-filter generation and `gluBuild2DMipmaps`.
- [ ] Per-triangle LOD selection, plumbed from the rasterizer setup.
- [ ] All four mipmap filters, including the `_LINEAR` variants that blend two
      levels.
- [ ] Two texture units with a per-unit environment; combine in fragment order.
- [ ] `glTexImage3D` and trilinear sampling.
- [ ] Cube maps: face selection from the major axis of the direction vector.
- [ ] `GL_CLAMP_TO_BORDER` plus `GL_TEXTURE_BORDER_COLOR`.

### Test gate

- Minifying a checkerboard 8:1 must show a smooth grey, not aliasing moiré —
  compare pixel variance against the un-mipmapped case, which must be higher.
- Each mipmap level's mean colour must match the level above it within
  tolerance (a box filter preserves the mean).
- Two units with `GL_MODULATE` must produce the product of both textures.
- A cube map sampled along +X/-X/+Y/-Y/+Z/-Z must return the corresponding
  face's colour.
- `/glcube` gains a mipmapped floor plane; no aliasing at a grazing angle.

### Definition of Done

The texture pipeline is complete for GL 1.3. `docs/opengl.md` records the
per-triangle LOD limitation explicitly.

### Deliverable

`patches/GL_G10_textures2.patch`

---

## Phase G11 — GLSL interpreter and the ES 2.0 shader path

**Objective:** programmable vertex and fragment stages. This is the single
largest phase in the whole roadmap — realistically larger than G0–G9 combined —
and the plan reflects that by splitting it into four independently testable
sub-phases.

### Why this is so much bigger than it looks

A shader profile is not "add two entry points". It requires a compiler front
end (lexer, parser, type checker for a C-like language with vectors, matrices
and swizzles), an execution engine, and a rewrite of the pipeline so that
attributes, uniforms and varyings flow through user code instead of fixed
state. The existing fixed-function path must keep working throughout, because
`/glcube`, `/glgears` and 181 in-OS checks depend on it.

### Sub-phases

**G11a — GLSL front end.** Lex and parse GLSL ES 1.0 into an AST; resolve
types; report errors through `glGetShaderInfoLog`. Testable entirely on the
host with no rendering: feed it valid and invalid shaders and check the
diagnostics. Roughly 2500 lines.

**G11b — execution engine.** A register-based interpreter over the AST, or a
bytecode VM if profiling demands it. Vector and matrix intrinsics, swizzles,
the built-in function library (`texture2D`, `normalize`, `dot`, `mix`, …).
Tested by running a shader over known inputs and checking outputs numerically,
still without a rasterizer. Roughly 2000 lines.

**G11c — pipeline integration.** `glCreateShader`/`glShaderSource`/
`glCompileShader`/`glCreateProgram`/`glAttachShader`/`glLinkProgram`/
`glUseProgram`; generic vertex attributes (`glVertexAttribPointer`,
`glEnableVertexAttribArray`); uniforms (`glGetUniformLocation`, the
`glUniform*` family). The vertex shader replaces the transform stage; varyings
are interpolated by the existing perspective-correct machinery; the fragment
shader replaces texturing, lighting and fog.

**G11d — coexistence.** Fixed-function and shader paths selected per draw by
whether a program is bound. This is where most of the risk lives: the two
paths must not fight over state.

### Performance reality

An AST interpreter runs a fragment shader per pixel. At 320×240 that is 76 800
interpretations per frame, and a trivial shader will be one to two orders of
magnitude slower than the fixed-function path. That is expected and acceptable
for correctness work, but it means the shader path is for *compatibility*, not
for speed, until a JIT exists — and a JIT is out of scope.

The honest framing, matching the G7 finding about vertex arrays: this phase
buys API coverage, not frames per second.

### Test gate

- 40+ front-end tests: valid programs compile, malformed ones produce a useful
  log and `GL_COMPILE_STATUS == GL_FALSE`.
- 30+ engine tests: known inputs produce numerically exact outputs.
- A pass-through vertex shader plus a constant fragment shader must render the
  same triangle as the fixed-function path, pixel for pixel.
- Fixed-function rendering must be bit-identical before and after the phase.

### Deliverables

`patches/GL_G11a_glsl_frontend.patch` … `GL_G11d_shader_pipeline.patch`

---

## Phase G12 — Framebuffer objects and render-to-texture

**Objective:** render into a texture instead of the window.

Comparatively small once G10 exists, because a texture already has storage in
the right format — an FBO mostly redirects the rasterizer's colour and depth
pointers at it.

### Scope

`glGenFramebuffers`, `glBindFramebuffer`, `glFramebufferTexture2D`,
`glGenRenderbuffers`, `glCheckFramebufferStatus`, plus depth attachments and
`glReadPixels` (which is trivial to add here and is currently listed as
missing in `docs/opengl.md`).

### Test gate

- Render a triangle to an FBO, bind it as a texture, draw it to the window:
  the sampled colours must match what was rendered.
- An incomplete FBO must report the right `glCheckFramebufferStatus` code
  rather than rendering somewhere undefined.

### Deliverable

`patches/GL_G12_fbo.patch`

---

## Phase G13 — VirGL hardware path

**Objective:** actually use the GPU, through the seam G9 built.

### The blocking prerequisite is kernel work, not GL work

`drivers/gpu/virgl.c` already implements contexts, resources, fenced
`SUBMIT_3D` and scanout present — but entirely kernel-side, with no syscall
exposed. libgl is user space by design (decision D3). So G13 cannot start until
the kernel gains a 3D submission syscall.

That syscall is a self-contained piece of kernel work and is specified as
**K1** below, because it is the correct next thing to build and it does not
depend on any further GL progress.

### Scope, once K1 exists

| Step | Detail |
|---|---|
| 1 | `probe()` returns 0 only when virtio-gpu with VirGL is actually present |
| 2 | `clear()` emits `VIRGL_CCMD_CLEAR` |
| 3 | `present()` drives TRANSFER_TO_HOST_3D + SET_SCANOUT + RESOURCE_FLUSH, which `virgl_present_render_target()` already implements |
| 4 | Vertex buffers uploaded as VirGL resources |
| 5 | Draw calls as `VIRGL_CCMD_DRAW_VBO`, which needs TGSI shaders — so a real triangle path depends on G11's compiler being retargetable to TGSI |

Steps 1–3 alone are worth having: they move the per-frame blit off the CPU
without any shader work.

### Deliverable

`patches/GL_G13_virgl.patch`

---

# Part III — Kernel work unblocked by this plan

## Phase K1 — Syscall for GPU 3D submission ✅ COMPLETE

**Objective:** expose the existing kernel VirGL transport to user space, safely.

This is the concrete next step. It is small, self-contained, benefits from
patterns the GL work already established, and unblocks G13.

### Design

A single syscall in the style of `SYS_GUI_CALL`, with a sub-op selector:

```
SYS_GPU_CALL (203)
  GPU_OP_INFO              query presence, VirGL support, scanout size
  GPU_OP_CTX_CREATE        create a 3D context, returns a handle
  GPU_OP_CTX_DESTROY
  GPU_OP_RESOURCE_CREATE   3D resource; returns a resource id
  GPU_OP_RESOURCE_DESTROY
  GPU_OP_TRANSFER_TO_HOST  upload pixels/vertices into a resource
  GPU_OP_SUBMIT_3D         submit a VirGL command stream, optionally fenced
  GPU_OP_SET_SCANOUT       bind a resource to a display scanout
  GPU_OP_FLUSH             flush a resource region to the display
```

### The security surface is the whole problem

A VirGL command stream is a program the GPU executes. Handing an unvalidated
one from user space to the host GPU is the single most dangerous thing this OS
could do, so the design has to be defensive from the start:

| Risk | Mitigation |
|---|---|
| Command stream points at resources the process does not own | Per-process resource-id table; translate user ids to kernel ids on submit, reject unknown ones |
| Stream length lies about its size | Validate the whole buffer with `validate_user_range()` before copying, exactly as the `GUI_OP_BLIT` path does |
| A process exhausts GPU memory | Per-process resource count and total-byte quotas |
| Resources leak when a process dies | Reap in `thread_exit()`, alongside the existing GUI window cleanup |
| Malformed opcodes crash the host renderer | Length-check every packet header against the remaining buffer before forwarding |

The `GUI_OP_BLIT` implementation from G0 is the model: validate the whole
range up front, copy into kernel memory, never dereference a user pointer.

### Tasks

- [x] Per-process GPU resource table in the TCB, with quotas.
- [x] `SYS_GPU_CALL` dispatch with full user-pointer validation.
- [x] Command-stream header walk: verify every packet fits before forwarding.
- [x] Resource-id translation, so a process cannot name another's resources.
- [x] Reaping on process exit.
- [x] Host unit tests for the validator, using captured command streams.
- [x] Integration test: a process submitting a hostile stream is rejected and
      the kernel survives — modelled on `test_gui_bad_pointers.sh`.

### Definition of Done

A user process can create a 3D context, upload a resource and present it, and
no malformed or hostile input from user space can fault the kernel or reach
the host GPU unvalidated.

### Deliverable

`patches/K1_gpu_syscall.patch`

### Results (verified)

| Item | Outcome |
|---|---|
| `SYS_GPU_CALL` (203) | Nine sub-ops: info, context and resource lifecycle, transfer, submit, scanout, flush |
| Per-process handles | Resources named by 1-based slot indices meaningless outside the owning process; translated to device ids on every call |
| Quotas | 4 contexts, 64 resources, 64 MB, 256 KB per command stream, 16 MB per transfer |
| User-pointer safety | Whole range validated with `validate_user_range()` and copied to kernel memory before the driver sees it |
| TOCTOU | The validator runs on the kernel-side **copy**, never on user memory the process could rewrite afterwards |
| Scanout restriction | Binding a resource to the display is limited to PID ≤ 2, matching `GUI_OP_RENDER` |
| Reaping | `gpu_cleanup_process()` called from `thread_exit()`, beside `gui_cleanup_process()` |
| Host unit test | `test_gpu_syscall` — **18/18 pass** |
| QEMU | Boots clean, `/gltest` 181/181, no panics, processes reap normally |

### The validator gets its own translation unit

`gpu_validate_cmd_stream()` lives in `kernel/gpu/gpu_cmdcheck.c` rather than
beside the syscall dispatch. It has no kernel dependencies — no allocation, no
user copies, no driver calls — so the host test links **the shipping file**
instead of a copy. For the one function standing between a hostile process and
the host GPU, testing the real code with deliberately malformed input is worth
the extra file.

The malformed cases that matter and are covered: a length field claiming more
than the buffer holds, off-by-one in both directions at the exact boundary, a
maximum-value length field, a valid packet followed by a lying one, and a
length near `UINT16_MAX` positioned so that a 32-bit `i + 1 + len` would wrap.
That last one is why the arithmetic is done in 64-bit.

### What is deliberately not done yet

`op_transfer()` copies the payload into kernel memory and validates it, then
calls `virtio_gpu_transfer_to_host_3d()` — which currently takes an offset into
a resource the driver already owns rather than a pointer to fresh data. Wiring
the bounce buffer through to the device needs a driver-side entry point that
does not exist yet. The syscall surface, validation and quota accounting are
complete and tested; the last driver hop is G13 work.

---

## Recommended order

| Next | Why |
|---|---|
| ~~K1~~ | ✅ **Done** — the syscall surface, validation and quotas are in place |
| **G10** | Now the best quality-per-line remaining; no new architecture |
| G12 | Small once G10 lands; also delivers `glReadPixels` |
| G11 | Largest by far; do it when the pipeline underneath is finished |
| G13 | Needs K1, and a full triangle path needs G11 |

---

## 4. Phase summary

| Phase | Content | Depends on | Visible result |
|---|---|---|---|
| **G0** | Blit syscall, float math, build scaffolding | — | buffer presented to a window |
| **G1** | AuraGLX context, `glClear`, GL errors | G0 | window filled with colour |
| **G2** | Matrix stacks, immediate mode, lines | G1 | wireframe cube |
| **G3** | Triangle rasterizer, depth buffer, culling | G2 | **solid shaded cube** |
| **G4** | Frustum clipping, scissor, `glGet*` | G3 | robust against any geometry |
| **G5** | Lighting, materials, normals | G3 | realistic shading |
| **G6** | Textures, blending, fog | G4 | textured objects |
| **G7** | Vertex arrays, VBOs, display lists | G3 | performance |
| **G8** | GLU, `/glcube`, `/glgears`, tests, docs | G5,G6,G7 | **working GL applications** |
| **G9** | Backend boundary, VirGL groundwork | G8 | path to hardware acceleration |

Critical path to the first convincing image: **G0 → G1 → G2 → G3**.

---

## 5. Mandatory engineering principles

Taken from the project's existing culture so that GL does not stand out from the
codebase.

1. **Host-side unit tests are mandatory.** The rasterizer and math are written so
   they compile with a plain `cc` and render into a `malloc` buffer. The same object
   is linked into both the application and the test — the technique already used for
   `bitmap.h` and `heap.c`.
2. **Warning-free builds** under `-Wall -Wextra -Werror`.
3. **No user-space-triggered crashes.** Every pointer crossing into the kernel goes
   through `validate_user_range`; a negative test is mandatory (see
   `test_gui_bad_pointers.sh`).
4. **Honest `docs/status.md`.** Every GL feature is marked ✅/🧪/🚧/❌. Nothing that
   is a stub may be marked as implemented.
5. **`CHANGELOG.md` is updated for every phase**, as is done today.
6. **Unimplemented GL entry points return `GL_INVALID_OPERATION`** and record the
   reason, rather than silently doing nothing.
7. **Specification conformance outranks speed.** Optimisation comes after
   correctness is proven by tests.
8. **Every phase ships a `.patch` file** in `patches/`, generated from the phase's
   complete diff, so the work can be reviewed step by step.

---

## 6. Effort estimate

| Phase | Lines of code (estimate) | Main risk |
|---|---:|---|
| G0 | ~400 | pointer validation in the kernel |
| G1 | ~500 | user-space heap limit |
| G2 | ~1100 | column-major matrix confusion |
| G3 | ~1200 | fill rule, interpolation precision |
| G4 | ~700 | attribute interpolation while clipping |
| G5 | ~700 | normal matrix, formula conformance |
| G6 | ~1200 | perspective-correct UV |
| G7 | ~900 | pointer semantics with a bound VBO |
| G8 | ~1400 | demo performance under emulation |
| G9 | ~400 | — |
| **Total** | **~8500** | |

For comparison: the entire current `drivers/` tree is 15 565 lines and `libc/` is
8 919. OpenGL is therefore comparable in size to the project's libc.

---

## 7. Progress log

| Phase | Status | Patch |
|---|---|---|
| G0 | ✅ complete | `patches/GL_G0_scaffolding.patch` |
| G1 | ✅ complete | `patches/GL_G1_context.patch` |
| G2 | ✅ complete | `patches/GL_G2_immediate.patch` |
| G3 | ✅ complete | `patches/GL_G3_rasterizer.patch` |
| G4 | ✅ complete | `patches/GL_G4_clipping.patch` |
| G5 | ✅ complete | `patches/GL_G5_lighting.patch` |
| G6 | ✅ complete | `patches/GL_G6_textures.patch` |
| G7 | ✅ complete | `patches/GL_G7_arrays.patch` |
| G8 | ✅ complete | `patches/GL_G8_demos.patch` |
| G9 | ✅ complete | `patches/GL_G9_backend.patch` |
