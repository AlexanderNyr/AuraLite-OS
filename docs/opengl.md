# OpenGL on AuraLite OS

AuraLite ships a **software OpenGL 1.1 implementation** as a user-space
library, `libgl`, together with a GLU utility layer and an AuraGLX window
binding. It renders entirely on the CPU: no GPU, no host acceleration, and
therefore no dependency on QEMU flags, host drivers or a particular hypervisor.

See [`GL_PLAN.md`](../GL_PLAN.md) for the phase-by-phase development plan and
the reasoning behind each design decision.

---

## Quick start

```c
#include "auragui.h"
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/auraglx.h"

int main(void) {
    int wid = ag_window_create(60, 40, 336, 288, "demo", AG_WIN_DEFAULT);
    ag_window_show(wid);

    /* The context resolution need not match the window: it is blitted 1:1
     * into the top-left corner, so a smaller context is the cheap way to
     * keep a software rasterizer responsive. */
    aglx_context_t *ctx = aglxCreateContext(wid, 320, 240, AGLX_DEFAULT);
    aglxMakeCurrent(ctx);

    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 320.0 / 240.0, 1.0, 50.0);
    glMatrixMode(GL_MODELVIEW);

    for (;;) {
        ag_event_t ev;
        while (ag_poll_event(wid, &ev) > 0) {
            if (ev.type == AG_EVT_CLOSE_REQ) goto done;
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -6.0f);
        /* ... draw ... */
        aglxSwapBuffers(ctx);
    }
done:
    aglxDestroyContext(ctx);
    ag_window_destroy(wid);
    return 0;
}
```

Build by adding the program to `USER_GL_APPS` in the Makefile; those targets
link `libgl` in addition to `libauragui`.

---

## Architecture

```
application
    |
    |  GL / GLU entry points
    v
libgl  (user space, ~7500 lines)
    glimm      immediate mode, vertex attribute latching
    glarray    vertex arrays, buffer objects
    gllist     display lists (command log)
    glmatrix   matrix stacks
    gllight    per-vertex lighting
    glclip     frustum clipping (Sutherland-Hodgman / Liang-Barsky)
    glraster   edge-function triangle rasterizer, depth buffer
    gltexture  texture objects and sampling
    glfrag     alpha test, blending, fog
    glu        GLU helpers and quadrics
    auraglx    context lifecycle, presentation
    |
    |  ag_blit()  -- one syscall per frame
    v
kernel GUI compositor -> framebuffer
```

### Why user space

The kernel is built `-mno-sse -mno-mmx`, so kernel code cannot use float
freely — `drivers/framebuffer/render3d.c` needs a special Makefile rule just to
get float support. User space has no such restriction. Beyond that, GL state is
hundreds of kilobytes per context plus arbitrary application geometry; keeping
it out of the kernel keeps that attack surface out too. This mirrors Linux,
where Mesa is user space and only the DRM transport is in the kernel.

### The frame path

Rendering never touches the window. Everything lands in the context's own
colour buffer, and `aglxSwapBuffers()` is the single point that crosses into
the kernel — one `ag_blit` per frame. That keeps syscalls out of the
rasterizer's inner loops and means the compositor only ever observes complete
frames, so output is tear-free without extra work.

---

## Supported subset

### Implemented

| Area | Detail |
|---|---|
| Context | `aglxCreateContext` / `MakeCurrent` / `SwapBuffers` / `Resize` / `DestroyContext` |
| Buffers | Colour (XRGB8888) and optional depth (float); `glClear`, `glClearColor`, `glClearDepth` |
| Matrices | `GL_MODELVIEW` (32 deep) and `GL_PROJECTION` (8 deep); `glPushMatrix`/`glPopMatrix`, `glLoadIdentity`, `glLoadMatrixf`, `glMultMatrixf`, `glTranslatef`, `glRotatef`, `glScalef`, `glFrustum`, `glOrtho` |
| Immediate mode | All ten primitive modes; `glVertex2f/3f/4f(v)`, `glColor3f/4f/3ub/4ub(v)`, `glNormal3f(v)`, `glTexCoord2f(v)` |
| Rasterizer | Edge-function triangles with barycentric interpolation, Bresenham lines, points; top-left fill rule |
| Depth | All eight comparison functions, `glDepthMask` |
| Culling | `glCullFace` (FRONT/BACK/FRONT_AND_BACK), `glFrontFace` (CW/CCW) |
| Clipping | All six frustum planes, attributes interpolated at the cut |
| Lighting | 8 lights (positional, directional, spot), Blinn–Phong specular, distance attenuation, front/back materials, `GL_COLOR_MATERIAL`, `GL_NORMALIZE` |
| Texturing | 2D textures, `GL_RGB`/`RGBA`/`LUMINANCE`/`LUMINANCE_ALPHA`/`ALPHA`, nearest and bilinear, `GL_REPEAT`/`CLAMP`/`CLAMP_TO_EDGE`, `MODULATE`/`REPLACE`/`DECAL`/`BLEND`, **perspective-correct** interpolation |
| Fragment ops | Alpha test, blending (full factor set), fog (`LINEAR`/`EXP`/`EXP2`), scissor |
| Arrays | Vertex/colour/normal/texcoord arrays, arbitrary stride, eight component types; `glDrawArrays`, `glDrawElements`, `glArrayElement` |
| Buffer objects | GL 1.5 subset: `glGenBuffers`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glDeleteBuffers` |
| Display lists | `GL_COMPILE` and `GL_COMPILE_AND_EXECUTE`, nesting, recompilation |
| State queries | `glGetIntegerv`, `glGetFloatv`, `glGetBooleanv`, `glIsEnabled`, `glGetString`, `glGetError` |
| Attribute stack | `glPushAttrib` / `glPopAttrib`, 16 deep |
| GLU | `gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluErrorString`, `gluSphere`, `gluCylinder`, `gluDisk` |

### Not implemented

| Missing | Notes |
|---|---|
| Shaders (GLSL) | Needs an interpreter; planned as G11 in `GL_PLAN.md` |
| Mipmaps | `glTexImage2D` accepts `level != 0` but does not store it; minification uses the base image |
| 3D and cube-map textures | Planned as G10 |
| Multitexturing | Single texture unit only |
| Stencil buffer | `GL_STENCIL_BUFFER_BIT` is accepted by `glClear` and ignored |
| Accumulation buffer | Not present |
| `glReadPixels` / `glCopyTexImage` | Read back with `aglxGetColorBuffer()` instead |
| Evaluators, feedback, selection | Not present |
| `GL_TEXTURE` matrix mode | `glMatrixMode(GL_TEXTURE)` reports `GL_INVALID_OPERATION` rather than silently doing nothing |
| Hardware acceleration | The backend seam exists (G9) and a VirGL candidate is registered, but it declines: the kernel's VirGL transport has no user-space syscall yet |

---

## Behaviour worth knowing

These are places where the implementation is deliberately specific, and where
a wrong assumption would waste debugging time.

**Lighting is per-vertex.** GL 1.1 evaluates the lighting equation at each
vertex and Gouraud-interpolates across the primitive. A specular highlight
therefore only appears if a *vertex* is positioned to catch it — a large,
coarsely tessellated quad will not show one in its middle. This is correct
fixed-function behaviour, not a limitation of this rasterizer.

**Light positions are transformed at specification time.** `glLightfv(GL_POSITION)`
multiplies by the MODELVIEW matrix in force at that instant and stores eye
coordinates. Setting a light before or after the camera transform gives
different results, and a light meant to stay fixed in world space must be
re-issued when the camera moves.

**Window coordinates have a bottom-left origin.** The flip to the
framebuffer's top-left origin happens only when a pixel is addressed, so
`glScissor` and window coordinates read naturally.

**Texture row 0 is the bottom row**, matching GL's texture coordinate origin.
No vertical flip is applied at sample time.

**`glOrtho` negates the z axis.** With `glOrtho(l, r, b, t, -1, 1)`, an object
at object-space `z = +0.5` ends up at window depth 0.25 — *nearer* than one at
`z = -0.5`. This trips people up when writing depth tests by hand.

**Vertex arrays are not faster than immediate mode here.** Measured: 10 000
triangles cost 4.5 ms either way, and 3.2 ms even with rasterisation removed
entirely. The per-vertex transform dominates, so removing a call per vertex is
noise. Arrays exist for API completeness; a real speed-up needs a faster
transform stage. See the header comment in `libgl/src/glarray.c` for why a
separate bulk path was rejected.

**Display lists store a command log**, not captured geometry, so a
`glTranslatef` inside a list takes effect when the list is *called*. Commands
that cannot be compiled execute immediately and flag `GL_INVALID_OPERATION`
rather than being silently dropped.

**Unimplemented entry points report errors.** Where a feature does not exist,
the call records a GL error instead of quietly doing nothing, so `glGetError()`
tells the truth about what happened.

---

## Performance

Software rasterisation on an emulated CPU is slow, and the numbers reflect
that. Host figures (native, `-O2`):

| Workload | Cost |
|---|---|
| `glClear` + rotating lit cube, 320×240 | ~0.07 ms/frame |
| `gluSphere(24×18)`, 280×210, lit | ~0.26 ms/frame |
| 10 000 triangles, immediate mode | ~4.5 ms |
| 10 000 triangles, `glDrawArrays` | ~4.6 ms |

Under QEMU expect roughly an order of magnitude worse. Practical advice:

- Keep the context small. 320×240 costs a quarter of what 640×480 does, and
  the blit scales it into the window for free.
- Enable `GL_CULL_FACE`. For a closed model it removes about half the fill.
- Compile static geometry into a display list — it avoids rebuilding the
  command stream, though not the transform.
- Skip the depth buffer (`aglxCreateContext(..., 0)`) for 2D work: it saves
  `width × height × 4` bytes and a test per fragment.

---

## Backends

`libgl` selects a rendering backend through a small table of function pointers
(`GL/glbackend.h`), modelled on the kernel's `netdev` abstraction. The software
rasterizer is registered as an ordinary backend rather than special-cased, so
dispatch has one shape everywhere.

```c
const gl_backend_info_t *bi = gl_backend_info();
printf("%s (hardware=%d)\n", bi->name, bi->hardware);
/* also reported by glGetString(GL_RENDERER) */
```

Any entry point in a backend may be NULL, in which case libgl uses its software
path for that operation alone. A hardware backend can therefore be brought up
one entry point at a time — implementing only `present`, for instance, would
move the per-frame blit onto the GPU while everything else stays on the CPU.

A VirGL candidate is registered and **declines** at `init()`. AuraLite's VirGL
command transport lives in the kernel (`drivers/gpu/virgl.c`) with no syscall
exposed to user space, and libgl is user space by design. The steps to complete
it are listed at the top of `libgl/src/glvirgl.c`; the prerequisite is a kernel
syscall for 3D submission.

---

## Demos

| Program | What it shows |
|---|---|
| `/glcube` | Lit, textured, depth-buffered cube. Geometry in a display list, ground grid from a vertex array. |
| `/glgears` | The classic three-gear benchmark, ported from real OpenGL sources with no changes to the GL calls. |
| `/gltest` | Regression suite: 170+ checks printed to the serial console as `[gl] PASS/FAIL`. Used by `tests/integration/cases/test_opengl.sh`. |

Both demos read an optional frame limit from a file — `/tmp/glcube.frames` and
`/tmp/glgears.frames` — because the shell's `run` command uses `spawn()`, which
does not forward `argv`. This follows the convention `/apm` already uses.

```text
auralite# write /tmp/glgears.frames 20
auralite# run /glgears
```

Both also appear in the `/glaunch` application launcher.

---

## Testing

| Suite | Coverage |
|---|---|
| `tests/unit/test_glmath.c` | Vector and matrix math, 37 checks |
| `tests/unit/test_glstate.c` | Context lifecycle, error contract, clearing, 37 |
| `tests/unit/test_glimm.c` | Matrix stacks, immediate mode, transform pipeline, 51 |
| `tests/unit/test_glraster.c` | Fill correctness, depth, culling, fill rule, 43 |
| `tests/unit/test_glclip.c` | Frustum clipping and the attribute stack, 28 |
| `tests/unit/test_gllight.c` | The lighting equation and materials, 32 |
| `tests/unit/test_gltex.c` | Texturing, perspective correction, blending, fog, 37 |
| `tests/unit/test_glarray.c` | Arrays, buffer objects, display lists, 36 |
| `tests/unit/test_glu.c` | GLU helpers and quadrics, 21 |
| `tests/integration/cases/test_opengl.sh` | `/gltest` and `/glcube` under QEMU |

Every unit test links the **real** libgl sources rather than a copy, so a test
cannot drift away from the shipping implementation. Assertions inspect rendered
pixels wherever possible: a transform bug that cancels out in the maths but
puts colour in the wrong place still fails.

```bash
make test-unit                              # all host tests
./build/test_glraster                       # one suite
bash tests/integration/cases/test_opengl.sh # in QEMU
```

### Known issue: SMP

Under `-smp 2`, roughly one `/gltest` run in three fails a different, arbitrary
check; under `-smp 1` the same binary passes every time. This matches the
kernel's documented SMP limitation (`TODO.md`: scheduling remains BSP-only) and
is unrelated to libgl, which is single-threaded. The GL test simply runs long
enough to hit the existing window.

---

## Adding a GL application

1. Write it against `GL/gl.h`, `GL/glu.h` and `GL/auraglx.h`.
2. Add the object and ELF rules to the Makefile, and the target to
   `USER_GL_APPS` — those link `libgl`, unlike ordinary user programs.
3. Add a `cp` line to the initrd rule.
4. Optionally add it to the `apps[]` table in
   `userspace/gui-launcher/glaunch.c`.

If the program needs a frame limit for CI, read it from a file under `/tmp`
rather than from `argv`.
