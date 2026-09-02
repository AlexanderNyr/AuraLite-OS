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
| Immediate mode | All ten primitive modes; `glVertex2f/3f/4f(v)`, `glColor3f/4f/3ub/4ub(v)`, `glNormal3f(v)`, `glTexCoord2f(v)`, `glTexCoord3f`, `glMultiTexCoord2f` |
| Rasterizer | Edge-function triangles with barycentric interpolation, Bresenham lines, points; top-left fill rule |
| Depth | All eight comparison functions, `glDepthMask` |
| Culling | `glCullFace` (FRONT/BACK/FRONT_AND_BACK), `glFrontFace` (CW/CCW) |
| Clipping | All six frustum planes, attributes interpolated at the cut |
| Lighting | 8 lights (positional, directional, spot), Blinn–Phong specular, distance attenuation, front/back materials, `GL_COLOR_MATERIAL`, `GL_NORMALIZE` |
| Texturing | 2D textures, `GL_RGB`/`RGBA`/`LUMINANCE`/`LUMINANCE_ALPHA`/`ALPHA`, nearest and bilinear, `GL_REPEAT`/`CLAMP`/`CLAMP_TO_EDGE`/`CLAMP_TO_BORDER`, `MODULATE`/`REPLACE`/`DECAL`/`BLEND`, **perspective-correct** interpolation |
| Mipmaps | Full chains, all four mipmap filters, `glGenerateMipmap`, `gluBuild2DMipmaps`, `GL_TEXTURE_BASE_LEVEL`/`MAX_LEVEL`; **per-triangle** LOD (see below) |
| Multitexturing | 2 units, `glActiveTexture`, `glClientActiveTexture`, per-unit enables, bindings and environment |
| 3D textures | `glTexImage3D`, `GL_TEXTURE_3D`, trilinear sampling, `GL_TEXTURE_WRAP_R` |
| Cube maps | `GL_TEXTURE_CUBE_MAP`, six faces, direction-vector lookup by major axis, mipmapped |
| Fragment ops | Alpha test, blending (full factor set), fog (`LINEAR`/`EXP`/`EXP2`), scissor |
| Arrays | Vertex/colour/normal/texcoord arrays, arbitrary stride, eight component types; `glDrawArrays`, `glDrawElements`, `glArrayElement` |
| Buffer objects | GL 1.5 subset: `glGenBuffers`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glDeleteBuffers` |
| Display lists | `GL_COMPILE` and `GL_COMPILE_AND_EXECUTE`, nesting, recompilation |
| State queries | `glGetIntegerv`, `glGetFloatv`, `glGetBooleanv`, `glIsEnabled`, `glGetString`, `glGetError` |
| Framebuffer objects | `glGenFramebuffers`, `glBindFramebuffer`, `glFramebufferTexture2D` (2D and cube faces, any mipmap level), `glFramebufferRenderbuffer`, `glCheckFramebufferStatus`; render-to-texture |
| Renderbuffers | `glGenRenderbuffers`, `glBindRenderbuffer`, `glRenderbufferStorage` (colour and depth), `glGetRenderbufferParameteriv` |
| Pixel readback | `glReadPixels` in `GL_RGB`/`RGBA`/`BGR`/`BGRA`/`ALPHA`/`DEPTH_COMPONENT`, from the window or from an FBO |
| Attribute stack | `glPushAttrib` / `glPopAttrib`, 16 deep |
| GLU | `gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluErrorString`, `gluSphere`, `gluCylinder`, `gluDisk` |

### Not implemented

| Missing | Notes |
|---|---|
| Geometry/tessellation/compute shaders | ES 2.0 has vertex and fragment stages only |
| Per-fragment mipmap LOD | The level is chosen per **triangle**, not per fragment (see below) |
| `GL_COMBINE` texture environment | The GL 1.3 programmable combiner is absent; the four GL 1.1 modes are present |
| More than 2 texture units | `GL_MAX_TEXTURE_UNITS` reports the real limit; raise `GL_MAX_TEXTURE_UNITS_IMPL` to change it |
| Stencil buffer | `GL_STENCIL_BUFFER_BIT` is accepted by `glClear` and ignored; `GL_STENCIL_ATTACHMENT` reports `GL_INVALID_OPERATION` rather than pretending |
| Accumulation buffer | Not present |
| `glCopyTexImage` / `glBlitFramebuffer` | Render into the texture directly with an FBO instead |
| Multiple colour attachments | `GL_MAX_COLOR_ATTACHMENTS` is 1: the fixed-function pipeline writes one colour, so a second would receive nothing |
| Evaluators, feedback, selection | Not present |
| `GL_TEXTURE` matrix mode | `glMatrixMode(GL_TEXTURE)` reports `GL_INVALID_OPERATION` rather than silently doing nothing |
| Hardware-accelerated **drawing** | The VirGL backend implements probe, clear and present (G13). `DRAW_VBO` needs the GLSL compiler retargeted to TGSI, which is a compiler back end and a phase in its own right |

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

**Mipmap level of detail is chosen per triangle, not per fragment.** This is
the one place where the implementation knowingly departs from what hardware
does, so it is worth understanding rather than discovering.

Hardware evaluates the LOD for every fragment from the screen-space
derivatives of the texture coordinates. A scanline rasterizer has no `dFdx` or
`dFdy`, so this implementation computes one level for the whole primitive, at
setup, from the ratio of its texture-space area to its screen-space area:

```
lod = log2( sqrt( texture-space area / screen-space area ) )
```

That is *exact* for a triangle at constant depth, and progressively wrong for
a strongly foreshortened one. A ground plane stretching to the horizon,
drawn as a single quad, gets one averaged level for the whole thing — too
blurry in the foreground and still aliased in the distance.

**The fix is to tessellate large receding surfaces.** `/glcube`'s floor is
split into a 16×16 grid for exactly this reason, and the comment in
`userspace/demos/glcube/glcube.c` says so. Tessellation is cheap; a per-fragment
derivative is not, in a rasterizer whose bottleneck is already arithmetic.

Per-fragment LOD is possible by carrying `du/dx` through the edge functions
and is the natural follow-up if the per-triangle version proves too coarse.

**Texture units combine in order.** Unit 0's output becomes unit 1's incoming
fragment colour, so `GL_MODULATE` on both units yields the product of the two
textures. `glTexEnv`, `glTexParameter`, `glTexImage*` and `glBindTexture` all
act on the unit selected by `glActiveTexture`; the *client* selector used by
`glTexCoordPointer` is separate and set by `glClientActiveTexture`. `glTexCoord`
itself always writes unit 0 — `glActiveTexture` does not redirect it.

**Framebuffer objects redirect four pointers, and nothing else.** The
rasterizer has only ever known about `ctx->color`, `ctx->depth`, `ctx->width`
and `ctx->height`. Binding an FBO points those at a texture or renderbuffer;
binding framebuffer 0 points them back at the window. Not one line of
`glraster.c` changed to gain render-to-texture.

Consequences worth knowing:

- **`aglxSwapBuffers()` always presents the window**, even while an FBO is
  bound — framebuffer 0 *is* the window, by definition.
- **An FBO's size is its attachment's size.** Binding a 64×64 target makes the
  effective viewport bounds 64×64 until it is unbound; set the projection and
  `glViewport` to match, and restore them afterwards.
- **No depth attachment means no depth buffer.** `GL_DEPTH_TEST` then silently
  does nothing, exactly as in a context created without `AGLX_DEPTH`. It does
  *not* fall through to the window's depth buffer. Attach a depth
  renderbuffer if the off-screen pass needs depth.
- **Attachments resolve at bind time, not attach time.** A texture may be
  re-uploaded at a new size while attached; the FBO sees the current state.
  Deleting an attached texture makes the FBO incomplete rather than dangling.
- **An incomplete FBO refuses to draw.** `glClear` and `glBegin` report
  `GL_INVALID_FRAMEBUFFER_OPERATION` instead of falling back to the window,
  because silently rendering somewhere else is the hardest kind of bug to see.

**Row order differs between the window and a texture.** The window's
framebuffer stores row 0 at the top, so writing a GL pixel flips y. A texture
stores row 0 at the bottom, which is already GL's convention, so no flip
applies. An implementation that flipped unconditionally would render correctly
to the window and *upside-down* into a texture — which is exactly what
happened first here, and is why `/glcube`'s inset panel exists as a visible
check.

The same convention makes `glReadPixels` return rows bottom-first, so a
readback fed straight into `glTexImage2D` round-trips without a flip.

**A texture rendered into is forced opaque when the FBO is unbound.** The
rasterizer writes `0x00RRGGBB` — no alpha — and the sampler reads
`0xAARRGGBB`, so without this a rendered texture would sample as fully
transparent and `GL_MODULATE` would multiply it to black. The fixup is one
pass over the attachment at unbind time; see the performance note below.

**A mipmap min filter on a texture with no chain falls back to level 0.**
That is the specification's incomplete-texture rule, and it is why the GL
default of `GL_NEAREST_MIPMAP_LINEAR` still draws a plain `glTexImage2D`
texture correctly. Re-uploading level 0 discards any chain built from the
previous image, since the smaller levels would otherwise still describe it.

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

Mipmap filtering cost, measured on a 16×16-tile textured floor at 320×240
(the `/glcube` ground plane):

| Filter | Cost |
|---|---|
| `GL_NEAREST_MIPMAP_NEAREST` | ~2.6 ms/frame |
| `GL_LINEAR` (no mipmaps) | ~3.2 ms/frame |
| `GL_LINEAR_MIPMAP_NEAREST` | ~3.9 ms/frame |
| `GL_LINEAR_MIPMAP_LINEAR` | ~5.9 ms/frame |

Worth reading carefully: `GL_NEAREST_MIPMAP_NEAREST` is **faster** than
un-mipmapped `GL_LINEAR`, because it reads one texel instead of four and the
smaller levels sit in cache. Trilinear (`GL_LINEAR_MIPMAP_LINEAR`) costs
roughly 1.9× un-mipmapped, since it samples eight texels across two levels.
If a scene is fill-bound, `GL_LINEAR_MIPMAP_NEAREST` is usually the right
compromise: most of the quality, about two-thirds of the cost.

Mipmaps also cost memory: a full chain is 4/3 of the base image.

Framebuffer objects, measured at 320×240 with 200 triangles per frame:

| Operation | Cost |
|---|---|
| Rendering into the window | 3.75 ms/frame |
| Rendering into an FBO | 3.72 ms/frame |
| `glReadPixels`, full 320×240 `GL_RGB` | 0.18 ms |

Rendering into an FBO costs **the same** as rendering into the window: it is
the same rasterizer writing to a different address.

The bind/unbind pair is not free, though, and its cost scales with the
attachment's area, because unbinding runs the alpha fixup over the whole
colour attachment:

| Attachment | Bind + unbind |
|---|---|
| 64×64 | 1.3 µs |
| 128×128 | 5.1 µs |
| 256×256 | 20.1 µs |
| 512×512 | 81.7 µs |

At one bind pair per frame this is noise. In a loop that switches targets per
object it is not — batch everything that shares a target together, which is
good practice on real hardware for entirely different reasons anyway.

Under QEMU expect roughly an order of magnitude worse. Practical advice:

- Keep the context small. 320×240 costs a quarter of what 640×480 does, and
  the blit scales it into the window for free.
- Enable `GL_CULL_FACE`. For a closed model it removes about half the fill.
- Compile static geometry into a display list — it avoids rebuilding the
  command stream, though not the transform.
- Skip the depth buffer (`aglxCreateContext(..., 0)`) for 2D work: it saves
  `width × height × 4` bytes and a test per fragment.

---

## The GLSL front end (phase G11a)

A GLSL ES 1.0 compiler front end lives in `libgl/src/glsl_*.c`: a lexer, a
recursive-descent parser and a type checker, producing a typed AST. It is
**not yet reachable from the GL API** — `glCreateShader` and friends arrive in
G11c — but it is complete, tested and runs on the target.

### What it accepts

The GLSL ES 1.0 language: all the scalar, vector, matrix and sampler types,
structs, arrays, functions with `in`/`out`/`inout` parameters, the full
statement set, and the built-in function library (`sin`, `dot`, `mix`,
`texture2D`, `lessThan`, …). `#version`, `#extension`, `#line`, `#pragma` and
`precision` declarations are accepted and ignored; there is no preprocessor, so
`#define` is refused rather than silently dropped.

### Diagnostics are the product

A shader that fails to compile at run time leaves the application with nothing
but the info log, so the log is treated as the deliverable rather than an
afterthought. Every diagnostic carries a line number and says what the rule
is, not just that it was broken:

```
ERROR: 0:3: cannot initialise 'float' with 'int' (GLSL ES has no implicit conversions)
ERROR: 0:7: swizzle 'z' selects a component beyond 'vec2'
ERROR: 0:12: relational operators need scalar operands; use lessThan()/greaterThan() for vectors
ERROR: 0:1: vertex shader never writes gl_Position
```

Of the 167 host tests, most are negative cases asserting on the *message*, not
merely on the failure.

### Rules worth knowing

**There are no implicit conversions at all.** `float f = 1;` is an error, and
so is `v * 2` for a `vec3` — the scalar must be `2.0`. This surprises everyone
arriving from C, so the diagnostic names the rule explicitly.

**Relational operators are scalars only.** `a < b` on two vectors is an error
pointing at `lessThan()`; `==` on vectors is legal and yields one `bool`, while
the component-wise form is `equal()`.

**Swizzles come from three disjoint alphabets** — `xyzw`, `rgba`, `stpq` — and
mixing them in one swizzle is an error. A swizzle with a repeated component
reads fine but is not assignable.

**Stage rules are enforced.** `attribute` is refused in a fragment shader,
`discard` in a vertex shader; a vertex shader that never writes `gl_Position`
and a fragment shader that neither writes `gl_FragColor` nor discards are both
diagnosed, because either renders nothing and the mistake is otherwise
invisible.

### Cost

| Shader | Compile time | Arena |
|---|---|---|
| 22-line Blinn–Phong fragment shader | 0.037 ms | 140 KB |
| 300-statement synthetic shader | 0.61 ms | 600 KB |

Compilation is fast enough to be irrelevant next to a single rendered frame.
The arena is one megabyte per compilation, freed in a single call when the unit
is destroyed; 112 KB of the floor is the type checker's symbol table.

---

## The GLSL execution engine (phase G11b)

An AST-walking interpreter in `libgl/src/glsl_exec.c` runs the tree the front
end produces. It is still not reachable from the GL API — that is G11c — but
it computes correct results for the whole language.

### How a shader reaches the outside world

Through three callbacks in a `glsl_env_t`: read a variable, write a variable,
sample a texture. The interpreter never touches a texture object, a uniform
store or a vertex array directly. That is what lets the whole engine be tested
numerically with no GL context, and it means G11c attaches the real pipeline
without changing a line of the interpreter.

### Why an interpreter and not a bytecode VM

The plan allowed for a VM "if profiling demands it". It does not: a second IR
would add a translation step and a second set of bugs to remove one switch
dispatch per node, while the real cost is the per-component float arithmetic
and the fact that a fragment shader runs once per pixel at all. Both
strategies land in the same place. Only a JIT would change that, and a JIT is
out of scope.

### Semantics worth knowing

**`mod()` takes the sign of the divisor**, unlike C's `fmod`: `mod(-1.0, 3.0)`
is `2.0`, not `-1.0`. Shaders that wrap a coordinate depend on this.

**Integer division truncates towards zero** and `1/2` is `0`. Values are
stored as floats internally, but the integer operators behave as integers.

**`matN(s)` builds a diagonal**, not a fill: `mat4(1.0)` is the identity.
Matrices are column-major, matching `glUniformMatrix`, so `m[3]` is the
translation column.

**Undefined maths yields finite values.** Division by zero gives `0.0`,
`normalize` of a zero vector gives a zero vector, `sqrt` of a negative gives
`0.0`. GLSL leaves all of these undefined and hardware produces infinities and
NaNs; a NaN in a colour propagates through blending and is very hard to trace
back, so a defined finite answer is the safer choice here.

**`&&`, `||` and `?:` do not evaluate what they do not need**, which is
observable when the skipped expression has a side effect.

### Bounded, because a shader is untrusted input

| Limit | Value | Why |
|---|---|---|
| Loop iterations per invocation | 100 000 | `while (true) {}` is a legal program. Hardware has a watchdog; here it would hang the compositor. |
| Call depth | 16 | GLSL ES forbids recursion, but a shader can still write it. |
| Argument nesting | 24 | `max(dot(a, b), 0.0)` is two deep. |
| Variables / storage | 128 / 4096 floats | Bounded so the interpreter state is a fixed size. |

Each is reported as a diagnostic in the info log, never as a fault.

### Cost

| Shader | Per invocation |
|---|---|
| Constant colour | 0.27 µs |
| Texture modulate | 0.44 µs |
| Blinn–Phong with a helper function | 1.85 µs |

At 320×240 that is **20 ms per frame for a trivial fragment shader** — against
0.07 ms for the entire fixed-function path drawing a lit cube. The plan
predicted one to two orders of magnitude and that is what it is.

This is worth stating plainly: **the shader path buys API coverage, not frames
per second.** A vertex shader is affordable — a few thousand invocations per
frame — but a fragment shader running per pixel is not, at any resolution
worth using, until a JIT exists.

---

## The shader pipeline (phase G11c)

Shaders are reachable from the GL API and draw pixels:
`glCreateShader`, `glShaderSource`, `glCompileShader`, `glCreateProgram`,
`glAttachShader`, `glLinkProgram`, `glUseProgram`, `glVertexAttribPointer`,
`glEnableVertexAttribArray`, `glBindAttribLocation`, `glGetAttribLocation`,
`glGetUniformLocation` and the `glUniform*` family including the matrix forms.

### What replaces what

| Stage | Fixed function | With a program bound |
|---|---|---|
| Transform | `MODELVIEW` × `PROJECTION` | the vertex shader |
| Interpolation | colour, normal, texture coordinates | those **plus varyings** |
| Fragment | texturing, lighting, fog, alpha test | the fragment shader |

Clipping, culling, the depth test, the scissor box and blending are
**untouched**: they work on window coordinates and a colour, and a shader
changes neither contract. That is why the phase is a few hundred lines rather
than a rewrite.

### What linking does

There is no code generation — both shaders keep their AST. Linking builds
three tables, once, so the interpreter's by-name variable lookups become index
arithmetic:

- **Uniforms.** A uniform declared in *both* shaders is one uniform with one
  location, as the specification requires; disagreeing about its type is a
  link error.
- **Varyings.** Matched by name and type. A varying the fragment shader reads
  and the vertex shader never declares is a **link error**, because it would
  otherwise silently read zeros and render black with nothing to point at.
- **Attributes.** Locations assigned, honouring `glBindAttribLocation`.

The tables are rebuilt from scratch by every link, so a stale location cannot
survive a relink.

### Behaviour worth knowing

**A sampler defaults to texture unit 0**, so a single-texture shader works
without any `glUniform1i`.

**A shader ignores `glEnable(GL_TEXTURE_2D)`** and samples whatever is *bound*
to the unit. The enables are a fixed-function concept with no meaning under
ES 2.0.

**A disabled attribute array supplies `glVertexAttrib4f`'s value**, defaulting
to `(0,0,0,1)` — a valid homogeneous point rather than four zeros that would
collapse every vertex to the origin.

**A short array leaves the rest at the default**, so `attribute vec4 aPos` fed
from a 3-component array reads `w` as 1.0.

**Using an unlinked program is `GL_INVALID_OPERATION`**, not a silent fallback
to fixed function — which would render the scene wrongly with no indication
why. Deleting the bound program *does* revert to fixed function.

**A shader that hits a runtime limit paints the fragment magenta** and records
why in the info log. An obviously wrong colour is far easier to notice than a
stale one.

### Cost

Full-screen quad at 320×240 (76 800 fragments):

| Path | Cost |
|---|---|
| Fixed function | 0.92 ms/frame |
| Constant-colour shader | 12.1 ms/frame |
| Lambert-lit shader | 53.8 ms/frame |
| Vertex stage only (4 vertices) | 1.2 µs/draw |

So a fragment shader costs **13–58× the fixed-function path**. That is better
than the plan's "one to two orders of magnitude" prediction, but the shape of
the advice is unchanged:

- **A vertex shader is affordable.** A few thousand invocations per frame at
  ~0.3 µs each is noise.
- **A fragment shader is not, at full-screen resolution.** Use one on small
  areas, or at a small context size, or accept single-digit frame rates.
- **The shader path buys API coverage, not frames per second.**

---

## Fixed function and shaders together (phase G11d)

Both paths are complete and coexist. Which one runs is decided per draw by
whether a program is bound. This section is what an application needs to know
about mixing them.

### The rules

**A shader replaces shading, not the framebuffer.** These still apply to a
shaded fragment exactly as they do to a fixed-function one:

`GL_DEPTH_TEST` · `glDepthMask` · `GL_CULL_FACE` · `GL_SCISSOR_TEST` ·
`GL_BLEND` · framebuffer objects · `glReadPixels`

**Fixed-function shading state is ignored entirely.** None of these reaches a
shaded fragment, because none has a meaning under ES 2.0:

`GL_LIGHTING` · `GL_FOG` · `GL_ALPHA_TEST` · the texture environment ·
`glShadeModel` · `MODELVIEW`/`PROJECTION` · `glVertexPointer` and the other
fixed-function arrays

A vertex shader outputs clip coordinates directly and reads *generic*
attributes, addressed by number through `glVertexAttribPointer`.

**Three combinations are refused rather than invented:**

| Combination | Result |
|---|---|
| `glBegin` with a program bound | `GL_INVALID_OPERATION` |
| `glUseProgram` inside `glBegin`/`glEnd` | `GL_INVALID_OPERATION` |
| `glUseProgram` inside a display list | `GL_INVALID_OPERATION` |

`glDrawArrays` and `glDrawElements` are exempt from the first: they open a
batch on the application's behalf and feed it through the shader path.

The reasoning is worth stating. `glVertex` is not an attribute, so a vertex
shader has nothing to read from it. Before G11d the two half-combined — the
fixed-function matrices placed the geometry and the fragment shader coloured
it — which is a hybrid no GL implementation produces. An application that
forgot `glUseProgram(0)` would have seen it render, look right, and draw
nothing recognisable on real hardware. Refusing is the honest answer.

### Switching

- `glUseProgram(0)` restores fixed function immediately, within a frame.
- Deleting the bound program also reverts to fixed function.
- Generic attribute arrays are *context* state and survive a program switch,
  so geometry can be bound once and drawn with several programs.
- Uniform values belong to the program and survive unbinding it.
- Shaders and programs are per-context, like textures and buffers.

### What G11d found

Four real defects, each invisible to the phase that introduced it:

1. **Shaded points and lines were not shaded.** They wrote the vertex colour,
   which the shader path leaves at white, so a shaded `GL_LINE_LOOP` came out
   *white*. Every G11c test drew triangles, so nothing exercised the point and
   line rasterizers with a program bound. `glPolygonMode(GL_LINE)` had the
   same problem for the same reason.
2. **Immediate mode silently hybridised** with a shader, as described above.
3. **`glUseProgram` inside `glBegin`/`glEnd`** was accepted — half a triangle
   through one program, half through another.
4. **`glUseProgram` inside `glNewList(GL_COMPILE)` executed immediately.**
   Compiling a list silently changed the current program, and the next
   unrelated draw call used a program the application never bound.

---

## The VirGL hardware backend (phase G13)

`libgl/src/glvirgl.c` reaches a real virtio-gpu through `SYS_GPU_CALL`: it
creates a 3D context, allocates a render target, and presents finished frames
by uploading them and driving `SET_SCANOUT` + `RESOURCE_FLUSH`.

### What moves to the GPU, and what does not

**Does:** the present. The finished frame goes to the scanout directly instead
of travelling through the compositor's blit path.

**Does not:** the drawing. `DRAW_VBO` needs shaders expressed as TGSI, and the
GLSL compiler from G11 produces an AST that it interprets. Retargeting it to
TGSI is a compiler back end — a phase in its own right, not a corner of this
one. Shipping a half-working draw path would be worse than not shipping it.

### It declines rather than half-working

`probe()` fails unless a virtio-gpu with VirGL is present *and* every setup
step succeeded. A backend that registered and then failed per-frame would put
"hardware" in `GL_RENDERER` and produce silence or corruption; declining leaves
the software path in place and the renderer string truthful. `/gltest` asserts
that `GL_RENDERER` agrees with whichever backend is actually active.

### Worth knowing

`clear()` emits a hardware `CLEAR` and then **returns "not handled"** on
purpose. The CPU rasterizer draws into its own buffer and knows nothing about
the GPU's render target, so the software clear must still run — returning
"handled" would skip it and present the previous frame's pixels.

### What it is worth today

Not speed. The software rasterizer costs milliseconds per frame and `ag_blit()`
costs tens of microseconds, so moving the present is a small fraction of a
frame. The value is that the seam is proved end to end — a real context, a real
backed resource, a real command stream — so the remaining work is bounded.

### The virtio-gpu init hang (RES-40, closed)

G13 recorded a hang: booting with `-device virtio-gpu-pci` stopped after
`found modern GPU` and never reached the shell. **That hang no longer
reproduces.** Residue ledger R1 closed RES-40: a current boot answers
`GET_DISPLAY_INFO`, reaches the shell, and
`tests/integration/cases/test_virgl_gpu.sh` runs with
`ENABLE_FULL_ASSERTS=1`. This architecture doc had gone stale against
`TODO.md`; `GL2_PLAN.md` L0 is the correction. The original trail, and
what was ruled out, remain in `TODO.md`.

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
| `/glcube` | Lit, textured, depth-buffered cube. Geometry in a display list, ground grid from a vertex array, a **mipmapped floor** tessellated 16×16 to demonstrate per-triangle LOD, and an inset **render-to-texture panel** showing a second view of the scene through an FBO. |
| `/glgears` | The classic three-gear benchmark, ported from real OpenGL sources with no changes to the GL calls. |
| `/gltest` | Regression suite: 373 checks printed to the serial console as `[gl] PASS/FAIL`. Used by `tests/integration/cases/test_opengl.sh`. |

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
| `tests/unit/test_gltex2.c` | Mipmaps, multitexturing, 3D textures, cube maps, 36 |
| `tests/unit/test_glarray.c` | Arrays, buffer objects, display lists, 36 |
| `tests/unit/test_glu.c` | GLU helpers and quadrics, 21 |
| `tests/unit/test_glbackend.c` | The backend seam and the VirGL candidate, 17 |
| `tests/unit/test_glfbo.c` | Framebuffer objects, renderbuffers, `glReadPixels`, 36 |
| `tests/unit/test_glsl.c` | The GLSL ES 1.0 front end: lexing, parsing, types, diagnostics, 167 |
| `tests/unit/test_glslexec.c` | The execution engine, checked numerically, 179 |
| `tests/unit/test_glprog.c` | The shader pipeline, checked against pixels, 107 |
| `tests/unit/test_glcoexist.c` | The two paths side by side and their limits, 59 |
| `tests/unit/test_glvirgl.c` | The VirGL backend: declining cleanly, and the wire format, 44 |
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

### Known issue: SMP — RESOLVED

This section used to record a real corruption: under `-smp 2` roughly one
`/gltest` run in three failed a different, arbitrary check.  The dissection
(FIXES R2) proved it was NOT a GL bug and NOT the then-suspected scheduler
limitation: the kernel switched no FPU/SSE state, so a thread migrated
mid-computation resumed with another CPU's xmm registers.  MATURITY M1 landed
the cure — eager `fxsave`/`fxrstor` in the context switch with a per-TCB
512-byte area — and `gltest` passes 373/373 under `-smp 4`; the `IL_SMP=1`
pin was removed from `test_opengl.sh`, and `test_fpu_smp.sh` stands guard.
(The old text blamed "TODO.md: scheduling remains BSP-only", which was stale
twice over — APs run user threads, and the R5 receipt pins it.)

---

## Adding a GL application

1. Write it against `GL/gl.h`, `GL/glu.h` and `GL/auraglx.h`.
2. Add the object and ELF rules to the Makefile, and the target to
   `USER_GL_APPS` — those link `libgl`, unlike ordinary user programs.
3. Add a `cp` line to the initrd rule.
4. Optionally add it to the `apps[]` table in
   `userspace/apps/gui-launcher/glaunch.c`.

If the program needs a frame limit for CI, read it from a file under `/tmp`
rather than from `argv`.
