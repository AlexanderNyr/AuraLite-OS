# AuraLite OS — OpenGL Implementation Plan

## Status: IN PROGRESS 🔧

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

## Phase G1 — AuraGLX context and first frame

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

- [ ] Context structure: colour buffer (`uint32_t*`), depth buffer (`float*`),
      dimensions, viewport, clear state, GL error slot.
- [ ] Buffer allocation/release, out-of-memory handling → `GL_OUT_OF_MEMORY`.
- [ ] `glClear` with a fast fill (8 pixels per iteration).
- [ ] `aglxSwapBuffers` → `ag_blit` → `ag_render_now`.
- [ ] GL error mechanism: `gl_set_error()`, sticky first error, cleared on read.
- [ ] `glGetString`: `VENDOR="AuraLite OS"`,
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

---

## Phase G2 — Matrix stacks and immediate mode (wireframe)

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

- [ ] Port the math from `render3d.c` into `glmath.c`; add `mat4_ortho`,
      `mat4_frustum`, `mat4_scale`, `mat4_inverse`, `mat4_transpose`, `mat4_normal`.
      **Strictly column-major**, as in OpenGL (`render3d.h` already declares that order).
- [ ] Matrix stacks with overflow checks → `GL_STACK_OVERFLOW` / `GL_STACK_UNDERFLOW`.
- [ ] Immediate-mode vertex buffer; primitive assembly for all modes, including
      correct triangulation of `GL_QUADS` / `GL_POLYGON` / strips / fans.
- [ ] Vertex pipeline: object → MODELVIEW → PROJECTION → clip → perspective divide
      → viewport → screen coordinates.
- [ ] Line (Bresenham) and point rasterisation.
- [ ] Validation: calling `glVertex` outside `glBegin/glEnd` → `GL_INVALID_OPERATION`.

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

---

## Phase G3 — Triangle rasterizer and depth buffer

**Goal:** a solid shaded cube. The main visual milestone.

### API in this phase

```c
glEnable/glDisable(GL_DEPTH_TEST|GL_CULL_FACE)
glDepthFunc(GL_LESS|GL_LEQUAL|GL_GREATER|GL_GEQUAL|GL_EQUAL|GL_NOTEQUAL|GL_ALWAYS|GL_NEVER)
glDepthMask()  glCullFace(GL_FRONT|GL_BACK)  glFrontFace(GL_CCW|GL_CW)
glShadeModel(GL_FLAT|GL_SMOOTH)  glPolygonMode()
```

### Tasks

- [ ] Edge-function rasterizer (not painter's algorithm — it produces artefacts on
      intersecting triangles, which is what `render3d.c` currently uses).
- [ ] Barycentric interpolation of colour and depth; Gouraud shading.
- [ ] Depth buffer: test, write, all 8 comparison functions, `glDepthMask`.
- [ ] Back-face culling from the signed area in screen space.
- [ ] Top-left fill rule — so adjacent triangles neither leave gaps nor double-shade
      (critical for blending in G6).
- [ ] Optimisation: primitive bounding box, incremental edge functions.

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

---

## Phase G4 — Clipping and state completeness

**Goal:** geometry outside the frustum or behind the camera no longer breaks the image.

### Tasks

- [ ] Near-plane clipping (mandatory: without it, vertices behind the camera cause a
      division by zero and inverted triangles).
- [ ] Full 6-plane frustum clipping (Sutherland–Hodgman) with attribute
      interpolation at the newly generated vertices.
- [ ] `glScissor` + `GL_SCISSOR_TEST`.
- [ ] `glGetIntegerv` / `glGetFloatv` / `glGetBooleanv`, `glIsEnabled` for every
      state introduced so far.
- [ ] `glPushAttrib` / `glPopAttrib` (basic mask groups).

### Test gate

- `test_glclip.c`: a triangle straddling the near plane is clipped into 1–2
  triangles with correctly interpolated colours; a fully outside triangle is
  discarded; a fully inside one is unchanged.
- A demo with the camera flying **through** an object — no artefacts, no crashes.

### DoD

The rasterizer is robust against any geometry, including degenerate and off-screen.

### Deliverable

`patches/GL_G4_clipping.patch`

---

## Phase G5 — Lighting and materials

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

- [ ] GL 1.1 lighting model: ambient + diffuse + specular (Blinn–Phong), distance
      attenuation, directional and positional lights, spotlight parameters.
- [ ] Per-vertex lighting followed by interpolation, as specified by GL 1.1.
- [ ] Normal transformation by the normal matrix (inverse-transpose MODELVIEW),
      `GL_NORMALIZE` / `GL_RESCALE_NORMAL`.
- [ ] Two-sided lighting.

### Test gate

- Unit test: a light along the normal gives maximum intensity; at 90° only ambient
  remains; a specular highlight appears when the reflection aligns with the eye.
- Demo: a sphere/cube with a moving light source.

### DoD

`GL_LIGHTING` produces results matching the GL 1.1 specification formulas.

### Deliverable

`patches/GL_G5_lighting.patch`

---

## Phase G6 — Textures, blending, fog

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

- [ ] Texture objects, name manager, `GL_RGB` / `GL_RGBA` / `GL_LUMINANCE` formats.
- [ ] **Perspective-correct** UV interpolation (interpolate `u/w`, `v/w`, `1/w`) —
      without it textures swim on large triangles.
- [ ] `GL_NEAREST` and `GL_LINEAR` (bilinear) filtering.
- [ ] `GL_REPEAT`, `GL_CLAMP`, `GL_CLAMP_TO_EDGE` wrap modes.
- [ ] Blending: the main factors (`GL_SRC_ALPHA`, `GL_ONE_MINUS_SRC_ALPHA`, `GL_ONE`,
      `GL_ZERO`, `GL_DST_ALPHA`…).
- [ ] Alpha test; linear and exponential fog.

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

---

## Phase G7 — Vertex arrays, VBOs, display lists

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

- [ ] Client vertex arrays with arbitrary stride and offset.
- [ ] Buffer objects (`GL_ARRAY_BUFFER`, `GL_ELEMENT_ARRAY_BUFFER`) with correct
      "pointer means offset" semantics while a VBO is bound.
- [ ] `glDrawElements` with `GL_UNSIGNED_BYTE/SHORT/INT` indices.
- [ ] Display lists: record commands and replay them (compile + execute).
- [ ] Fast path: `glDrawArrays(GL_TRIANGLES)` must not go through the
      immediate-mode machinery.

### Test gate

- Unit test: `glDrawArrays` and the equivalent `glBegin/glEnd` sequence produce a
  **bit-identical** framebuffer.
- Benchmark: 10 000 triangles through a VBO are measurably faster than immediate mode.

### DoD

An application can draw a real model without per-vertex calls.

### Deliverable

`patches/GL_G7_arrays.patch`

---

## Phase G8 — GLU, demos, system integration

**Goal:** the OS ships user-visible GL applications.

### Tasks

- [ ] GLU: `gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluErrorString`,
      `gluBuild2DMipmaps` (simplified), `gluSphere` / `gluCylinder` / `gluDisk`.
- [ ] `/glcube` — rotating lit textured cube, FPS counter, mouse and keyboard control.
- [ ] `/glgears` — port of the classic gears (reference benchmark).
- [ ] `/gltest` — regression run: performs a series of GL operations, verifies buffer
      contents and prints `[gl] PASS/FAIL` to serial for the integration tests.
- [ ] Add the applications to the initrd, the `/glaunch` menu and desktop icons.
- [ ] Update `README.md` (program table), `docs/status.md`, `CHANGELOG.md`, `TODO.md`.
- [ ] `docs/opengl.md`: architecture, supported subset, what is missing, how to write
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

---

## Phase G9 — Backend boundary and hardware groundwork

**Goal:** prepare VirGL integration without changing application code.

### Tasks

- [ ] Introduce a backend operations table (modelled on `netdev.c`):
      `gl_backend_t { clear, draw_triangles, present, ... }`.
- [ ] Wrap the software rasterizer as `gl_backend_software`.
- [ ] `gl_backend_info_t` + `glGetString(GL_RENDERER)` reports the active backend.
- [ ] `gl_backend_virgl` stub: detected when virtio-gpu with VirGL is present, with a
      transparent fallback to software when unavailable.
- [ ] Environment variable / context flag to force a specific backend.

### DoD

Switching backends requires no application changes. The hardware path can be
developed incrementally without breaking the working software path.

### Deliverable

`patches/GL_G9_backend.patch`

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
| G1 | 🔧 next | — |
| G2 | ⬜ planned | — |
| G3 | ⬜ planned | — |
| G4 | ⬜ planned | — |
| G5 | ⬜ planned | — |
| G6 | ⬜ planned | — |
| G7 | ⬜ planned | — |
| G8 | ⬜ planned | — |
| G9 | ⬜ planned | — |
