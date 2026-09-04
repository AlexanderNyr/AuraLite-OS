# AuraLite OS — OpenGL, the second series (the leftovers)

## Status: COMPLETE — L0–L7 landed

| Phase | Result | Deliverable |
|-------|--------|-------------|
| L0 — the rig | ✅ complete | `patches/GL2_L0_rig.patch` |
| L1 — stencil buffer | ✅ complete | `patches/GL2_L1_stencil.patch` |
| L2 — copies | ✅ complete | `patches/GL2_L2_copies.patch` |
| L3 — texture leftovers | ✅ complete | `patches/GL2_L3_texture.patch` |
| L4 — per-fragment mipmap LOD | ✅ complete | `patches/GL2_L4_lod.patch` |
| L5 — shader-path fitness | ✅ complete | `patches/GL2_L5_earlyz.patch` |
| L6 — VirGL `DRAW_VBO` | ✅ complete | `patches/GL2_L6_virgl_draw.patch` |
| L7 — close-out | ✅ complete | `patches/GL2_L7_closeout.patch` |

This document answers:

> *`GL_PLAN.md` G0–G13 + K1 is closed. Of the things it named as leftover,
> which ones are still the right next work — and what is the honest, measured
> path, rather than "the rest of OpenGL"?*

It follows `REALINTERNET2_PLAN.md` (continuation series: measured opener,
D-numbers, phase Results, named non-goals, terminal arithmetic) and the
original `GL_PLAN.md` (definition of done, test gate, host unit tests first,
honest docs). A new letter, the Y-series convention: the first series spent
**G**. Reusing G14+ in a second file would make every grep ambiguous.

**Baseline:** commit `dbc27fe` (HEAD at the time of writing). Every claim
below was checked against the tree, not assumed. Line numbers will drift;
the *claims* are what the phases answer to, and L0 pins the ones that
must not silently rot.

---

## 1. Where this plan comes from

`docs/opengl.md` already has the inventory, under **Not implemented**. This
plan does not rediscover it. It ranks those rows, drops the ones that are
still the wrong work, and turns the rest into dependency-ordered phases.

### Fact 1 — The first series finished what it promised, and said so

`GL_PLAN.md` is marked complete: software GL 1.1 + 1.5 VBOs, G10 mipmaps /
two texture units / 3D / cube maps, G11 GLSL ES 1.0 as an AST interpreter
(not a JIT), G12 FBOs and `glReadPixels`, G13 VirGL probe / clear / present,
K1 `SYS_GPU_CALL` 203. D3 still holds: GL is userspace `libgl`; the kernel
does not interpret a GL command.

The leftover list is therefore a *named* list, not a vibe. Citing
`docs/opengl.md` lines 140–150:

| Missing | What the tree actually does today |
|---|---|
| Geometry / tess / compute | ES 2.0 has VS+FS only — **out of this plan** (§4) |
| Per-fragment mipmap LOD | Per-triangle (`glraster.c` `triangle_lod`, G10 named follow-up) |
| `GL_COMBINE` | Four GL 1.1 env modes only; the token is absent from `gl.h` |
| >2 texture units | `GL_MAX_TEXTURE_UNITS_IMPL` is **2** (`glcontext.h:43`) |
| Stencil buffer | `glClear` accepts the bit and ignores it (`glstate.c:144`) |
| Accumulation | Absent — **out of this plan** (§4) |
| `glCopyTexImage` / `glBlitFramebuffer` | No symbols in `gl.h` or `libgl/src` |
| Multiple colour attachments | `GL_MAX_COLOR_ATTACHMENTS` is 1 — **out of this plan** (§4) |
| Evaluators / feedback / selection | Absent — **out of this plan** (§4) |
| `GL_TEXTURE` matrix mode | Valid enum, `GL_INVALID_OPERATION` (`glmatrix.c:71–76`) |
| Hardware-accelerated **drawing** | G13 present-only; `DRAW_VBO` named as a later compiler phase |

### Fact 2 — Stencil is an honest hole, not a stub (closed at L1)

Three independent refusals, live at `dbc27fe` / L0, closed by L1:

- `glstate.c:144` — `GL_STENCIL_BUFFER_BIT` is accepted and has no effect
  (the spec's "clear of a missing buffer is a no-op").
- `glfbo.c:30` and `attachment_slot()` — `GL_STENCIL_ATTACHMENT` reports
  `GL_INVALID_OPERATION` / `GL_FRAMEBUFFER_UNSUPPORTED` rather than
  pretending.
- `gl.h` has `GL_STENCIL_BUFFER_BIT` and `GL_STENCIL_ATTACHMENT` and
  nothing else: no `GL_STENCIL_TEST`, no `GL_KEEP` / `GL_INCR` / `GL_DECR`,
  no `glStencilFunc`. Compare-func tokens `GL_NEVER`..`GL_ALWAYS` already
  exist because depth uses them.

A classic GL 1.1 application that enables stencil gets silence, not a
diagnostic it can grep. That is the G2 texture-matrix shape of honesty
applied to a buffer that was never allocated.

### Fact 3 — The G10 texture story stopped one layer short of GL 1.3

`GL_MAX_TEXTURE_UNITS_IMPL` is 2, the four GL 1.1 environments
(`MODULATE` / `REPLACE` / `DECAL` / `BLEND`) work, mipmaps work, and the
level is chosen **once per triangle** from the texture-space / screen-space
area ratio (`glraster.c` `triangle_lod`). The comment on that function is
the follow-up this plan is for:

> Per-fragment LOD is a possible follow-up (carry `du/dx` through the edge
> functions); it is not free.

`GL_COMBINE` is not a token. `glMatrixMode(GL_TEXTURE)` is a loud error.
Both are load-bearing for a surprising number of GL 1.2/1.3 demos that
otherwise look like they should run — they compile against our `gl.h`
until they hit the env mode or the matrix mode, and then they don't.

Raising the unit count is a `#define`. It is also how G10 blew a 130 KB
scratch context off the stack (`aglxResize`). Any phase that grows
per-vertex or per-context state re-measures the context size and keeps
it off the C stack. That is not optional.

### Fact 4 — Copies are the FBO story with a hole in it (closed at L2)

G12 made the rasterizer write through four fields (`color`, `depth`,
`width`, `height`) and then pointed those fields at a texture. Render-to-
texture works. Reading back works (`glReadPixels`, six formats). What does
not work is the thing applications actually type next: copy the window
into a texture, or blit one framebuffer onto another.

`glCopyTexImage*` and `glBlitFramebuffer` have **zero** hits in
`lib/libgl`. The documented workaround — "render into the texture with an
FBO instead" — is correct and also not what `glCopyTexSubImage2D` callers
do.

### Fact 5 — The shader path is API coverage, and nothing visual uses it

G11c measured, full-screen quad at 320×240:

| Path | ms/frame |
|---|---|
| Fixed function | 0.92 |
| Constant-colour shader | 12.1 |
| Lambert-lit shader | 53.8 |

Vertex stage alone: 1.2 µs/draw. G11b already said the quiet part:
**only a JIT moves the needle**, and a JIT is not a phase of a software
GL plan. What *is* still on the table is the one cheap, correct
optimisation the interpreter can take without changing the language:
**early-Z** — do not run the fragment shader on a fragment the depth
test will reject, when the shader cannot write `gl_FragDepth` and
cannot `discard`.

And, at the time G11 closed, nothing shipped *drew* with it:
`userspace/demos/glcube` and `glgears` still contain no
`glCreateShader` / `glUseProgram`, and G11's pixels existed only in
`/gltest` and the host tests. GL2 L5 closes the visible half of that
with `/glshade` — the cube, lit from a Lambert program — and makes the
interpreter cost smaller where the scene allows it: the conservative
early-Z predicate (no `discard`, no `gl_FragDepth`) keeps hidden
fragments out of the shader, and forces shade-then-depth on the
shaders that would change the tests' outcome. The 53.8 ms full-screen
number stands; see the L5 Result.

### Fact 6 — The hardware draw seam is missing twice

`libgl/src/glvirgl.c:13–16`, still the file header:

> It does NOT implement `DRAW_VBO`. That is step 5, it needs shaders
> expressed as TGSI.

That is G13's own leftover, recorded as residue **RES-41** (class S,
handed off at R12): the *kernel* already emits the 12-dword
`VIRGL_CCMD_DRAW_VBO` packet (`drivers/gpu/virgl.c:180`) and
`VIRGL_CCMD_CREATE_OBJECT` for a shader (`virgl.c:310`). The missing
piece is userspace feeding `CREATE_SHADER` + a vertex buffer +
`DRAW_VBO` through `SYS_GPU_CALL`.

The seam is also missing one layer up. `GL/glbackend.h` says a backend
is responsible for "clearing buffers, **drawing a batch of triangles**,
and presenting a finished frame". The `gl_backend_t` struct has
`init`, `clear`, `present`, `destroy`. There is no `draw` member.
G13 slotted into the three pointers that existed. A triangle on the
GPU has nowhere to hang.

GL2 L6 moves both halves of this pin: `gl_backend_t` has the `draw`
member, and `glvirgl.c` feeds `CREATE_SHADER` + `SET_VERTEX_BUFFERS` +
`DRAW_VBO` through `SYS_GPU_CALL` — with one canned TGSI pipeline,
not a compiler (D7). RES-41 is closed *for the seam*; L7 opens the
AST→TGSI retarget as its own row rather than leaving RES-41 half-true.
This paragraph is the record of what the pin used to say, so a reader
of the opener knows why the greps in `tools/check_gl2_claims.py`
changed shape.

### Fact 7 — `docs/opengl.md` claimed a hang that residue had closed (corrected at L0)

At `dbc27fe` the architecture doc still said:

> **The virtio-gpu driver hangs during initialisation when a device is
> actually attached.**

`TODO.md` and `docs/residue_ledger.md` RES-40 already said the hang no
longer reproduces (residue R1). The GL architecture doc was the stale
one. L0 deleted that sentence and pointed at the ledger; the checker
pins the absence.

### Fact 8 — SIMD was explicitly deferred *here*

`OPT_PLAN.md` §5, named non-goal:

> User-space SIMD (libgl rasteriser vectorisation, SSE memcpy beyond
> `rep movsb`). Real wins, but they belong to a userspace/SDK plan —
> the kernel plan stops at the kernel/libc seam.

G11b's numbers say the fragment interpreter, not the fill, is the
shader-path cost, and G8's numbers say the per-vertex transform, not
the fill, is the fixed-function cost. Vectorising the rasterizer
without a new measuring rig would be the placebo D1 exists to block.
It is a named non-goal of *this* series too (§4), with the door left
open if L5's early-Z table says the fill is now the thing.

---

## 2. Decisions

Numbered so later phases can cite them instead of re-arguing.
`GL_PLAN.md` D1–D5 still apply (ship something real; the subset grows;
GL is userspace; host tests first; measure every phase). The ones
below are this series' own.

**D1 — Measure first, in-tree, or it didn't happen.** Every phase's
gate includes a before/after that a committed tool can see: a host
check count, a `/gltest` assertion, a ms/frame line, a command-stream
dword. "Looks better on the cube" is not a deliverable.

**D2 — QEMU/TCG numbers gate nothing hard; a missing GPU is a loud
skip, not a fail.** VirGL claims run only with a virtio-gpu that
actually has VirGL (`test_virgl_gpu.sh` already knows this shape).
Software-path claims run everywhere. TCG timing is recorded and
human-reviewed, the OPT D2 rule.

**D3 — Behaviour-preserving by construction.** Existing host tests and
`/gltest` assertions stay green unmodified. New state defaults to
*off* / *identity* / *the GL 1.1 env mode*. A phase that needs an old
check edited to go green is changing pixels and must say so in its
Result.

**D4 — Honest refusal until the feature lands.** Do not add
`glStencilFunc` as a no-op that returns success. Do not add
`GL_COMBINE` as a token that falls through to `GL_MODULATE`. The
G2 texture-matrix pattern (valid enum, `GL_INVALID_OPERATION`, a
comment that names the follow-up) is the template; this series *is*
that follow-up, so the refusal comes out in the same commit as the
implementation.

**D5 — Software is the default and the CI path.** Hardware drawing is
opt-in behind a GPU. `glGetString(GL_RENDERER)` stays truthful: a
backend that cannot draw does not get to imply that it did. G13's
VirGL backend already reports hardware for present-only; L6 does not
make that worse, and does not claim "GPU triangles" in docs until a
gate has seen one.

**D6 — The checker judges code and gates, not `.patch` files.**
`REALINTERNET2_PLAN.md` D6: a file in `patches/` proves a file exists.
L0's checker pins opener greps and, once a phase is ✅, the seam that
phase landed. Phase hygiene still holds: `CHANGELOG.md`, this table,
`docs/opengl.md`'s Not-implemented row, and the checker pins move in
the **same commit**.

**D7 — No GLSL → TGSI compiler in this series.** L6 is a *canned*
pass-through shader proving `DRAW_VBO` on the existing syscall.
Retargeting G11's AST is a compiler back end and a plan in its own
right; L7 opens that as a named hand-off rather than half-doing it
here. Claiming otherwise by shipping a draw path that only works for
one hard-coded triangle *and calling it shaders* would be worse than
not shipping it — G13's own words, applied one layer down.

**D8 — `glBegin` + a bound program stays `GL_INVALID_OPERATION`.**
G11d measured the hybrid (fixed-function transform, shader colour)
and refused it because no real GL produces it. This series does not
quietly reverse that to make a demo easier.

---

## 3. The phases

Dependency order: the rig first (L0), then the software holes that
do not need each other but all need the rasterizer (L1 stencil, L2
copies), then the G10 leftovers that grow per-fragment state (L3
texture, L4 LOD), then the shader-path win that wants a working
depth test (L5, after L1 so stencil/depth order is already the
real one), then hardware drawing on the G13 seam (L6), then the
docs close-out (L7).

L3 and L4 both touch the fragment loop. They do not parallelise;
L4 lands second so COMBINE and extra units are already in the
sampler it has to call.

---

### L0 — The rig

**Status: ✅ COMPLETE** (`patches/GL2_L0_rig.patch`).

**Objective:** make every later claim in this plan checkable by `make`,
and stop the stale hang sentence being the architecture doc.

Nothing in the tree today will notice if a phase adds `GL_COMBINE` as
a token and forgets the combiner, or "lands" stencil by deleting the
refusal comment. This phase builds the tripwire; it implements no
GL.

Tasks:

- [x] `tools/check_gl2_claims.py` (the `check_rinet2_claims.py` shape:
      opener facts PINNED as live greps, pins move in the same commit
      as the phase that takes them). Deliberately does not assert
      `.patch` existence (D6 / the RINET2 lesson).
- [x] Opener pins, all live against `dbc27fe`:
      1. `GL_MAX_TEXTURE_UNITS_IMPL` is 2 (`glcontext.h`).
      2. `glMatrixMode(GL_TEXTURE)` takes the `GL_INVALID_OPERATION`
         branch (`glmatrix.c`).
      3. `glfbo.c` still contains `there is no stencil buffer`.
      4. `glstate.c` still contains `GL_STENCIL_BUFFER_BIT is accepted
         but has no effect`.
      5. `gl.h` has no `GL_COMBINE`, no `glCopyTex`, no
         `glBlitFramebuffer`.
      6. `glvirgl.c` still contains `It does NOT implement DRAW_VBO`.
      7. `gl_backend_t` has no `draw` member (the struct lists
         `init` / `clear` / `present` / `destroy` only).
      8. `userspace/demos/glcube` and `glgears` contain neither
         `glCreateShader` nor `glUseProgram`.
- [x] `--selftest` negative control: a planted empty tree is caught,
      or the checker is declared dead. Wired into `make test-unit`
      via `tests/unit/test_gl2_claims.sh`.
- [x] Correct `docs/opengl.md` § "Known defect, predating this phase":
      the virtio-gpu init hang is RES-40 / residue R1, closed. Point
      at `TODO.md` and the ledger. This is a Y0-style plan correction
      (the doc was written before the residue series), not a GL
      feature.
- [x] Record the live libgl host-test inventory and `/gltest` SUMMARY
      line into §5 of this document, as the before-column. **Do not
      freeze a number in the checker that will rot**; freeze the
      *names* of the `test_gl*` binaries in `UNIT_TESTS`.

**What L0 measured** (host, this machine):

- `sizeof(struct aglx_context)` = **238 568 bytes** (~233 KiB). G10
  already blew a scratch copy of this off a 64 KB user stack; L1's
  stencil plane and L3's extra units must not put one on the C stack
  either. The number is L3's budget.
- Host libgl inventory, frozen by name: the 17 `test_gl*` binaries
  listed in `UNIT_TESTS` (see §5). Super-set is allowed; dropping one
  is a checker failure.
- `/gltest` is documented at **373** in-OS checks
  (`docs/opengl.md` Demos table). Not gated — a later phase that adds
  checks would rot a frozen count.
- Plan correction: the hang sentence is gone; `docs/opengl.md` now
  names RES-40.

**Definition of done:** the checker is red if this document and the
tree disagree about the opener; the hang sentence is gone; `make
test-unit` runs the selftest. ✓

**Test gate:** `tests/unit/test_gl2_claims.sh` — checker OK on the
real tree, planted violation caught. ✓

**Result:** 17 claims green, selftest catches an empty tree; hang
sentence deleted; `sizeof(aglx_context)` = 238 568.

---

### L1 — Stencil buffer

**Status: ✅ COMPLETE** (`patches/GL2_L1_stencil.patch`).

**Objective:** give GL 1.1 the buffer it has been loudly refusing.

Stencil is the highest-value missing *fixed-function* feature: shadows,
portals, UI clipping, the classic two-pass outline. Depth already has
the eight compare functions and a per-fragment test in the rasterizer;
stencil is a second plane with a three-way op (sfail / dpfail / dppass).

Design, so the phase does not grow a packed depth-stencil format it
does not need:

| Area | Rule |
|---|---|
| Storage | Separate 8-bit plane, allocated with the window like depth (`aglxCreateContext` / `aglxResize`). Heap, never the C stack (G10). |
| Default | Test off, func `GL_ALWAYS`, ref 0, mask `0xFF`, ops `GL_KEEP`, writemask `0xFF`, clear value 0. Existing pixels unchanged (D3). |
| API | `glEnable(GL_STENCIL_TEST)`, `glStencilFunc`, `glStencilOp`, `glStencilMask`, `glClearStencil`; `glClear(GL_STENCIL_BUFFER_BIT)` writes. |
| Tokens | Add the missing ones to `gl.h`: `GL_STENCIL_TEST`, `GL_KEEP`, `GL_INCR`, `GL_DECR`, `GL_INVERT`, `GL_INCR_WRAP`, `GL_DECR_WRAP`, `GL_STENCIL_FUNC` / `VALUE_MASK` / `REF` / `FAIL` / `PASS_DEPTH_FAIL` / `PASS_DEPTH_PASS` / `WRITEMASK` / `BITS` / `CLEAR_VALUE`. `GL_NEVER`..`GL_ALWAYS` and `GL_ZERO` / `GL_REPLACE` already exist. |
| Order | Scissor → stencil-sfail → depth → stencil-dpfail/dppass → blend. Matches GL 1.1 §4.1. Matches what L5's early-Z will skip. |
| FBO | `GL_STENCIL_INDEX8` renderbuffer; `GL_STENCIL_ATTACHMENT` no longer `INVALID_OPERATION`. Completeness requires matching dimensions, same as depth. No packed `D24S8` (non-goal). |
| Queries | `glGetIntegerv(GL_STENCIL_BITS)` returns 8 when the plane exists, 0 on a context created without it. A new `AGLX_STENCIL` attribute, default **on** for the window context so `/glcube` does not have to opt in — measure the extra allocation; if it blows a 64 KB user stack in a test, default off and say so. |
| VirGL | `glvirgl_clear` already has a stencil dword of 0. Leave it; the CPU plane is the source of truth until L6. |

Tasks:

- [x] Tokens + context fields + allocation / free / resize.
- [x] Rasterizer test + ops, including wrap vs saturate on `INCR`/`DECR`.
- [x] FBO stencil renderbuffer + attachment + completeness.
- [x] `glClear` actually writes the plane; the `glstate.c:144` comment
      comes out in this commit (D4, D6 pin 4).
- [x] Host unit test `tests/unit/test_glstencil.c`: func × op matrix
      against a 8×8 target, two-pass "draw where stencil == 1", FBO
      attach/complete, clear, `GL_STENCIL_BITS`. Guard canaries around
      the colour buffer so a stencil write cannot bleed.
- [x] `/gltest` assertions for the same two-pass, plus
      `glGetError` on the old refusal paths now succeeding.
- [x] `docs/opengl.md` Not-implemented: drop the stencil row.

**Definition of done:** a two-pass stencil clip produces the expected
pixels on host and in `/gltest`; old tests unmodified (D3); FBO
attachment is real; the opener pin about "no stencil buffer" has
moved.

**Test gate:** `test_glstencil` in `UNIT_TESTS`; `/gltest` stencil
block green in `test_opengl.sh`; `make test-unit` EXIT 0. ✓

**Result:** 34/34 `test_glstencil`; existing host suites unmodified
(D3); `sizeof(aglx_context)` 238 568 → **239 384** (+816 B of fields;
the plane is heap). `AGLX_DEFAULT` includes `AGLX_STENCIL` — extra
allocation is `width*height` bytes, not a stack blow. Opener pins 3–4
moved.

---

### L2 — Copies: `glCopyTexImage2D`, `glCopyTexSubImage2D`, `glBlitFramebuffer`

**Status: ✅ COMPLETE** (`patches/GL2_L2_copies.patch`).

**Objective:** close the G12 hole. The pixels are already there;
applications need a way to move them without re-issuing draws.

Design:

| Area | Rule |
|---|---|
| Source | The current draw target (window or bound FBO). G12's `glReadPixels` already reads that target; copies share the read path rather than growing a second one. No `glReadBuffer` / `glDrawBuffer` yet — one colour buffer (Fact 1, MRT is a non-goal). |
| `glCopyTexImage2D` / `SubImage2D` | Colour only, `GL_UNSIGNED_BYTE` effective format matching an existing `glTexImage2D` internal format. Depth copy is a non-goal of the *TexImage* entry points (use blit). |
| `glBlitFramebuffer` | Colour and/or depth, `GL_NEAREST` only (`GL_LINEAR` → `GL_INVALID_OPERATION` rather than a surprising filter). Y-flip when window ↔ FBO conventions differ (G12's `target_flip_y`). Mask bits the implementation does not have (stencil, before L1 lands; accum) are ignored, matching `glClear`. |
| Overlap | Same-FBO blit with overlapping boxes is `GL_INVALID_OPERATION` unless the implementation copies through a scratch. Prefer the error: a hidden scratch is a third buffer to leak. |
| MSAA | None in the tree; no resolve path to invent. |

Tasks:

- [x] Entry points in `glfbo.c` (they are framebuffer operations,
      not texture-environment operations).
- [x] Host tests in `test_glfbo.c` (do not start a new binary for
      three functions): window → texture round-trip, FBO → FBO blit,
      Y-flip window ↔ texture, overlap error, `GL_LINEAR` error.
- [x] `/gltest` one visual: render the cube into an FBO, blit a
      64×48 inset, compare against the G12 render-to-texture panel
      path (same pixels, fewer draws).
- [x] `docs/opengl.md` Not-implemented: drop the copy/blit row.

**Definition of done:** a `glReadPixels` of a blitted region matches
a `glReadPixels` of the source; host + `/gltest` green; D3 holds.

**Test gate:** extended `test_glfbo`; `/gltest` blit block;
`make test-unit` EXIT 0. ✓

**Result:** `test_glfbo` 36 → **48/48**; existing host suites unmodified
(D3); `/gltest` +14 → **402**. `sizeof(aglx_context)` stays **239 384**
(the extra `read_framebuffer_binding` GLuint fit in existing alignment
padding). Opener pin 5 moved. `GL_FRAMEBUFFER` still binds both read
and draw.

---

### L3 — Texture leftovers: `GL_COMBINE`, `GL_TEXTURE` matrix, 4 units

**Status: ✅ COMPLETE** (`patches/GL2_L3_texture.patch`).

**Objective:** finish the G10 texture story up to what GL 1.3
applications actually type, without raising the unit count so far
that the context becomes the G10 stack bomb again.

Three sub-deliverables, one phase, because they all land in the
sampler and the vertex path together. The checker pins move only
when *all three* are done — a COMBINE token with two units and no
matrix is not this phase.

Design:

| Area | Rule |
|---|---|
| `GL_COMBINE` | GL 1.3 §3.8.13 subset: `COMBINE_RGB` / `COMBINE_ALPHA` modes `REPLACE`, `MODULATE`, `ADD`, `ADD_SIGNED`, `INTERPOLATE`, `SUBTRACT`, `DOT3_RGB`, `DOT3_RGBA`. Sources `TEXTURE`, `CONSTANT`, `PRIMARY_COLOR`, `PREVIOUS`. Operands `SRC_COLOR`, `ONE_MINUS_SRC_COLOR`, `SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`. Scales 1 / 2 / 4. Missing mode → `GL_INVALID_ENUM`, not a silent `MODULATE` (D4). |
| Texture matrix | Per **unit** (the GL 1.3 rule), stack depth 2. `glMatrixMode(GL_TEXTURE)` becomes legal. Default identity, so existing UVs are unchanged (D3). Applied to `(s, t, r, q)` in the vertex path *before* clip, so clipping interpolates the post-matrix coords. |
| Units | `GL_MAX_TEXTURE_UNITS_IMPL` 2 → **4**. Not 8: every vertex already carries `s,t,r` per unit (`glvertex.h`) and G10's 130 KB lesson is in the file header of `aglx`. Measure `sizeof(aglx_context)` before/after and put the numbers in this section's Result. |
| Context size | No `aglx_context` on the C stack. The L0 pin about unit count moves from 2 to 4 in this commit. |

Tasks:

- [x] Tokens in `gl.h`; `glTexEnvi` / `glTexEnvfv` COMBINE parameters;
      combiner in `gltexture.c` (the existing `combine` comment at
      line 981 is the insertion point).
- [x] Per-unit texture matrix stack in `glmatrix.c` /
      `glcontext.h`; delete the `GL_INVALID_OPERATION` branch.
- [x] Raise `GL_MAX_TEXTURE_UNITS_IMPL`; audit every
      `for (u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL)` — they should
      already be written against the macro (G10). If any site
      hard-codes `2`, it is a bug this phase is for.
- [x] Host tests: `test_gltex2.c` grows COMBINE cases (MODULATE
      expressed as COMBINE equals the GL 1.1 path — the D3
      tripwire), INTERPOLATE, DOT3; matrix translate-then-sample;
      unit 2 and unit 3 sample independently.
- [x] `/gltest` a 3-unit COMBINE and a texture-matrix slide.
- [x] `docs/opengl.md` Not-implemented: drop COMBINE, units, texture
      matrix rows; document the new limits.

**Definition of done:** COMBINE `MODULATE` matches GL 1.1 `MODULATE`
pixel-for-pixel (D3); `glMatrixMode(GL_TEXTURE)` no longer errors;
`glGetIntegerv(GL_MAX_TEXTURE_UNITS)` is 4; context size recorded;
opener pins 1, 2, 5 (COMBINE part) moved.

**Test gate:** `test_gltex2` + `test_glimm` (matrix stacks) green;
`/gltest`; `make test-unit` EXIT 0. ✓

**Result:** `test_gltex2` 36 → **42/42**; `test_glimm` 51 → **54/54**;
existing host suites unmodified (D3). `/gltest` +9 → **411** (3-unit
COMBINE, texture-matrix slide, units=4, unknown mode refused).
`sizeof(aglx_context)` 239 384 → **240 304** (+920 B; still heap-only).
`GL_MAX_TEXTURE_UNITS` is 4; COMBINE MODULATE matches GL 1.1 MODULATE
pixel-for-pixel; `glMatrixMode(GL_TEXTURE)` is legal. Opener pins 1, 2
and 5-COMBINE moved.

---

### L4 — Per-fragment mipmap LOD

**Status: ✅ COMPLETE** (`patches/GL2_L4_lod.patch`).

**Objective:** land the follow-up G10 named and declined.

`triangle_lod` picks one level for the whole primitive. A ground
plane receding to the horizon gets a single averaged level;
`/glcube`'s floor works around this by tessellating. Hardware would
blend several. The comment already names the method: carry `du/dx`
(and `dv/dy`) through the edge functions.

Design:

| Area | Rule |
|---|---|
| Metric | λ = log2(max(√(dudx²+dvdx²), √(dudy²+dvdy²))) on the projected `(s/w, t/w)`, in texels. Standard, one LOD per fragment per unit. |
| Filters | All four mipmap min-filters already exist; they consume the level they are given. Trilinear stays 2-level. |
| Cost | G10 said this is "not free". The gate is not "faster"; it is "honest". Record ms/frame of `/glcube`'s floor at 320×240 before (per-triangle, tessellated) and after (per-fragment, *un*tessellated). If per-fragment is more than ~2× the tessellated path, say so in the Result and keep both: tessellation remains legal. |
| Default | Per-fragment becomes the implementation. No new enum. Applications that tessellated still work (D3: they just spend geometry they no longer need). |

Tasks:

- [x] Derivatives in `gl_raster_triangle`; `triangle_lod` becomes the
      fallback for degenerate screenspace (the division-by-zero
      path it already has).
- [x] Host test: an untilted receding quad, sampled at several
      window-Y rows, shows *increasing* LOD down the screen. A
      1:1 facing quad stays at level 0. Guard against picking
      level-0 everywhere (the bug that looks like success in a
      screenshot).
- [x] `/glcube` floor: drop the extra tessellation if the new LOD
      is visually equivalent; keep it if L4's own numbers say the
      cost is worse. Either choice is written in the Result.
- [x] `docs/opengl.md` Not-implemented: drop the per-fragment LOD
      row; keep a behaviour note that a scanline rasterizer's
      derivatives are along the window axes, not along the
      primitive, which is what everyone else does too.

**Definition of done:** the Y-row LOD test is green; numbers in this
section; the G10 comment's "possible follow-up" sentence comes out.

**Test gate:** `test_gltex2` LOD block; `/gltest` / `/glcube` still
green; `make test-unit` EXIT 0.

**Result:** ✅ (2026-09-03). The perspective-correct `(s, t)` at a
fragment is the quotient of the two functions G6 already interpolates
(`Ns` from `s/w`, `D` from `1/w`), so the derivatives come from the
quotient rule with constant plane slopes carried through the edge
functions (`dE0/dx = -e0dy`, `dE0/dy = +e0dx`, over the signed area):
`ds/dx = rw * (dNsdx - ss * dDdx)`.
`lambda = log2(max(|(du/dx, dv/dx)|, |(du/dy, dv/dy)|))` in texels,
computed per fragment per unit only when the unit's filter consumes
mipmaps. `triangle_lod` survives exactly as the plan specified: the
degenerate-screenspace fallback when a slope is non-finite (sliver
triangles divide by the near-zero area the old guard already had). No
context growth (`sizeof(aglx_context)` unchanged at 240 304), no new
state — D3 holds: every existing host test and `/gltest` check stayed
green unmodified.

Host: `test_gltex2` 42 → 44. `t_lod_increases_toward_horizon` draws
ONE receding quad (its diagonal deliberately kept away from the sampled
column) and requires the level to rise monotonically toward the horizon
with **≥ 4 distinct levels** in one column — the two-triangle step the
old per-triangle path produces (distinct = 2; verified by compiling the
test against a fallback-only rasterizer, where the test fails) cannot
pass it, and neither can level-0-everywhere.
`t_lod_facing_1x1_stays_zero` pins magnification on the boundary.

`/glcube` floor at 320×240, floor-only `CLOCK_MONOTONIC` over 200
frames in-guest (TCG — D2: recorded, gates nothing):

| Path | ms/frame |
|---|---|
| per-triangle + 16×16 tessellated (baseline `d752846`) | 6.21 |
| per-fragment + 16×16 tessellated | 6.06 |
| per-fragment + untessellated (**shipped**) | 0.05 |

Tessellation dropped: the shipped number is 0.8 % of the baseline, far
inside the plan's ~2× rule — with the level per fragment, the 512-triangle
grid buys nothing, and vertices (not the derivative arithmetic) dominate
at this resolution. `/glcube`'s floor is one quad with the same 0..32 UV
span; the demo comment records the measurement and why.

---

### L5 — Shader-path fitness: early-Z and `/glshade`

**Status: ✅ COMPLETE** (`patches/GL2_L5_earlyz.patch`).

**Objective:** make the G11 path something a human can see, and take
the one optimisation that does not require a new language backend.

G11c's Lambert shader is 53.8 ms at 320×240 because it runs the
interpreter on every fragment, including those the depth test will
reject. GL permits early-Z when the fragment shader cannot write
`gl_FragDepth` and cannot `discard`. G11a's type checker already
knows about `discard` (it is a diagnostic in the wrong stage); it
can know about `gl_FragDepth` writes the same way.

`/glshade` is the visual twin of `/glcube`: the same cube, the same
window chrome, a Lambert (or Blinn–Phong) program instead of
`glEnable(GL_LIGHTING)`. It exists so the demos stop implying there
are no shaders (Fact 5, opener pin 8).

Design:

| Area | Rule |
|---|---|
| Early-Z | Conservative: only when the bound fragment shader has no `discard` and no `gl_FragDepth` store. Otherwise keep shade-then-depth, because those shaders exist to change the test. |
| Points / lines | G11d already made shaded points and lines actually shade. Early-Z applies to triangles first; points and lines follow if the same predicate holds. |
| `/glshade` | New demo, packaged like `/glcube`. Fixed-function fallback is **not** allowed in this binary — if the program fails to link, it prints why and exits. That is the point. |
| JIT / bytecode | Non-goal (D7's cousin). If early-Z does not move the 53.8 ms number, the Result says so and does not invent a VM to chase it. |

Tasks:

- [x] Sema flag `may_kill_early_z` on the fragment AST; honour it
      in the rasterizer before `glsl_run` on the fragment.
- [x] Host test: a full-screen shaded quad *behind* an already-drawn
      opaque quad must not invoke the fragment shader for the hidden
      pixels (count via a side-channel counter in the interpreter
      env, test-only). A shader with `discard` must *not* take
      early-Z (a pixel that would have discarded must stay at the
      previous colour, which early-Z would have overwritten with
      depth-fail... wait: discard with depth-fail is subtle). The
      test is: `discard` shader + early-Z *disabled* produces the
      G11c pixels; enabling early-Z on a `discard` shader is the
      bug the predicate exists to prevent.
- [x] Re-measure Lambert at 320×240 with a depth-prepass and
      without; table in this section.
- [x] `/glshade` demo + README apps table row + initrd packaging.
- [x] Opener pin 8 moves.

Landed as designed, with one correction to this section's premise and
one measurement surprise, both recorded here rather than smoothed over.

*The premise.* This phase described early-Z as an optimisation to
take; in the tree the order was already depth-test-then-shade for
every shader — G11c had banked the win and shipped an *unsound* version
of it: a `discard` shader's hidden fragments skipped the interpreter
and took stencil zfail operations, while §4.1.5 says a discarded
fragment reaches no framebuffer operation at all. What L5 adds is the
predicate that makes the order conditional and therefore correct:

| Shader at link time | Order | Why |
|---|---|---|
| No `discard` anywhere in the fragment AST | stencil/depth tests first; rejected fragments never reach the interpreter | the shader cannot change the tests' outcome |
| `discard` present | shaded first; a discarded fragment leaves colour, stencil and depth exactly as they were | §4.1.5 — its whole purpose is to overrule the tests |

The flag is per-program linked state (`may_kill_early_z`, recomputed
on every `glLinkProgram`), sits in alignment padding so neither
`gl_program_t` (12152 before and after) nor the context (240 304)
grows, and the scan is the same whole-tree walk as the
missing-`gl_FragColor` check. The language has no `gl_FragDepth`; the
scan's comment binds the future store to join it in the same commit.
Points and lines: no depth test exists for them yet, so early-Z does
not apply — N/A, not skipped.

*The test.* `test_glprog` grows a counter (`gl_shader_fs_count`, a
test-only side channel incremented at the top of the fragment runner)
and four scenes: a safe full-screen quad alone shades 4 096/4 096
fragments at 64×64; the same quad behind a fixed-function wall shades
exactly the visible half (2 048) and the wall keeps its colour; a
`discard` shader over the same wall shades **all 4 096** —
shade-then-depth — and the discarded half still leaves the wall's
green intact; a right-half quad shades 2 048. `test_glprog` 118/118,
`test_glcoexist` 59, `test_glslexec` 179, `test_glraster` 43,
`test_glstencil` 34.

*The measurement.* Full-screen Lambert at 320×240, this host,
24-frame average — the counter reports what actually ran:

| Scene | ms/frame | Interpreter runs/frame |
|---|---|---|
| Lambert quad alone (the G11c setup) | 38.9–45.8 run-to-run (G11c recorded 53.8) | 76 800 |
| Lambert quad behind a full-screen fixed-function wall, predicate live | 1.1 | 0 |
| Same, predicate forced to 0 | 1.2 | 0 |

The honest reading: the prepass win was **already banked** by G11c's
draw-call order — forcing the predicate off changes nothing, because
"off" for a safe shader is the same tests-first order. Early-Z does
not move the 53.8 ms number; as the JIT/bytecode row above already
rules, no VM was invented to chase it. What the predicate buys is soundness, and what it costs is
exactly visible: the `discard` shader behind the same wall pays all
4 096 interpreter runs where the safe shader pays none — the price of
a fragment shader that means what it says.

*`/glshade`.* The visual twin of `/glcube`: same window chrome, same
keys, the cube lit by a Lambert program instead of
`glEnable(GL_LIGHTING)`. Geometry through generic vertex attributes
and `glDrawArrays` (D8); its own column-major mat4; uMVP plus a
pure-rotation uniform for normals. Fixed-function fallback is absent
by design — a failed compile or link prints the info log and exits
non-zero. Packaged like `/glcube` (Makefile `USER_GL_APPS` +
`INITRD_DEMOS`), frame limit via `/tmp/glshade.frames`, README apps
row added.

**Definition of done:** `/glshade` draws a lit cube from GLSL;
early-Z predicate is tested, not assumed; Lambert numbers recorded;
D8 holds (`/glshade` uses attributes, not `glBegin`). — **met.**

**Test gate:** `test_glprog` / `test_glcoexist` still green;
`/glshade` packaged; `/gltest` shader block unmodified (D3);
`make test-unit` EXIT 0. — **green.**

---

### L6 — VirGL `DRAW_VBO`, canned TGSI

**Status: ✅ COMPLETE** (`patches/GL2_L6_virgl_draw.patch`).

**Objective:** put one triangle on the GPU through the syscall G13
and K1 already proved, without pretending G11 now emits TGSI (D7).

This is RES-41's userspace half. The kernel validator already knows
the packet. `glvirgl.c` already knows how to `SUBMIT`. What does not
exist is a `draw` hook to hang it on, a vertex-buffer resource, and
a shader object.

Design:

| Area | Rule |
|---|---|
| Seam | Add `int (*draw)(struct aglx_context *ctx, const gl_draw_batch_t *batch)` to `gl_backend_t`. NULL means software, which is every backend except VirGL after this phase. The header comment finally matches the struct. |
| Supported batch | `GL_TRIANGLES`, `glDrawArrays` (not elements), no bound GLSL program (fixed-function *or* a dedicated "use the canned shader" path), position + colour only. Anything else returns non-zero and the software rasterizer draws it. A partial GPU frame mixed with CPU triangles is a tearing bug; if the batch is unsupported, the **whole draw** falls back. |
| Shader | One canned TGSI vertex shader (pass-through position to `gl_Position`, colour to a varying) and one canned TGSI fragment shader (that varying to `COLOR0`). Built as dword arrays in `glvirgl.c`, host-tested against the kernel encoder, never generated from G11's AST. |
| Resources | Upload the ARRAY_BUFFER (or a bounce of immediate vertices) as a VirGL vertex-buffer resource; the colour RT already exists from G13. |
| Present | G13's present (transfer CPU buffer → scanout) is **wrong** for a GPU-drawn frame: it would overwrite the triangle with the CPU's empty buffer. When `draw` handled the frame, present scanouts the GPU RT *without* the CPU transfer. The first draft that forgets this will look exactly like "DRAW_VBO is a no-op". The host test cannot catch it; the QEMU gate must. |
| Renderer string | Still "AuraLite VirGL (virtio-gpu)" when probe succeeded. Docs (not the string) say drawing is canned-subset until a compiler exists. |
| Decline | Unchanged: no device, no VirGL, any setup step failed → software. Loud skip in CI without a GPU (D2). |

Tasks:

- [x] `draw` member; software backend leaves it NULL; dispatch in
      `glDrawArrays` / the immediate-mode flush.
- [x] Canned TGSI + `CREATE_SHADER` + `BIND_SHADER` +
      `SET_VERTEX_BUFFERS` + `DRAW_VBO` through `GPU_OP_SUBMIT`.
- [x] Present fork: GPU-drawn frames do not `TRANSFER` the CPU
      colour buffer over the RT.
- [x] Host `test_glvirgl.c`: packet layouts, the "unsupported batch
      returns non-zero" matrix, the canned shader dword count.
      Still no GPU in the room.
- [x] `tests/integration/cases/test_virgl_gpu.sh` grows a triangle
      assertion (scanout hash ≠ clear colour, or a probed
      `TRANSFER_FROM_HOST` of a known pixel). Loud-skip without
      virglrenderer. The existing clear/present assertions stay.
- [x] Opener pins 6 and 7 move. RES-41 is closed *for the seam*;
      L7 opens the compiler as a new row rather than leaving
      RES-41 half-true.
- [x] `docs/opengl.md` hardware-drawing row becomes "canned TGSI
      triangle; GLSL → TGSI is a follow-up".

Two deviations from this section's letter, both deliberate, both
recorded where the letter was written.

*The dispatch point.* This section said "dispatch in `glDrawArrays` /
the immediate-mode flush". The flush turned out to be per-primitive:
the assembler rasterizes every triangle the moment its third vertex
arrives, so an immediate-mode hook would hand the GPU one triangle at
a time — sub-draw granularity, exactly the tearing the design table
forbids. The dispatch lives in `glDrawArrays` only, where the whole
draw is visible before anything rasterizes; `glBegin`/`glEnd` triangles
stay on the CPU and the eligibility screen says so by requiring a
vertex array. The design row's other clauses hold verbatim: unsupported
→ non-zero → the WHOLE draw falls back.

*The matrices.* The design table never mentions the transform, but the
canned pass-through shader forced the question: the GPU must divide,
clip and project the same numbers the CPU would have. The answer is in
the batch format — it carries CLIP coordinates produced by the same
`gl_transform_vertex` the immediate path uses, so no matrix restriction
is needed and no identity-MVP screen either. Lighting is likewise
absent from the eligibility screen because it bakes into the vertex
colour before that point. The screen does refuse: depth, stencil,
blend, alpha test, scissor, culling, non-fill polygon mode, fog,
texturing, other primitive modes, partial triangles, bound programs,
and draws without a vertex array.

*The seam.* `gl_backend_t.draw(ctx, batch)` — NULL means software,
which is still every backend except VirGL. The batch is `count`
vertices of clip xyzw + rgba; libgl gathers it into a context bounce
buffer allocated only for eligible draws. `glvirgl.c` carries the two
canned TGSI dword arrays (pass-through VS 21 dwords, colour FS 13,
hand-written against Mesa's TGSI token format and walked by a
tgsi-shaped parser in the host test — the G11 AST is nowhere near
them, per D7), the one-time setup stream (pipeline objects, shaders,
binds, surface + framebuffer state, a viewport mirroring the CPU
projection, the vertex-buffer bind), per-draw `RESOURCE_INLINE_WRITE`
chunks of at most 96 whole triangles + `DRAW_VBO`, the final chunk
submitted fenced so the present cannot race the rasterization. The
context grew 24 bytes (240 304 → 240 328: the bounce pointer, its
capacity and the two frame flags) — recorded in the metrics table.

*The present fork.* Per the design: `frame_gpu_draw` /
`frame_sw_raster` on the context decide — a frame the GPU drew entirely
scanouts the GPU render target with `SET_SCANOUT` + `RESOURCE_FLUSH`
and NO `TRANSFER`; a mixed frame presents the CPU buffer (self-
consistent; the GPU-drawn parts of a mixed frame are lost, which is
the honest cost of a canned subset and the reason the eligibility
screen is strict). Both flags reset at present. Without a GPU the hook
does not exist and every byte of behaviour is G13's (D3 held: the
software-only gates below are unchanged except the new checks).

*Tests, with no GPU in the room.* `test_glvirgl` 44 → 73: a
tgsi-shaped walk of both canned shaders (header/processor/END,
writemasks, the FS's perspective-interpolated COLOR linking it to the
VS's OUT[1]); the setup stream through the kernel validator with
per-packet asserts (CREATE_SHADER lens 4+21 / 4+13, framebuffer,
vertex-buffer shape, viewport); and the dispatch pair through a forced
test backend — a handled draw delivers three vertices with identity-
transform clip coords and leaves the software buffer untouched; the
same draw declined delivers software pixels; an ineligible mode never
reaches the hook at all (this caught a real bug: the first draft
offered every mode to the hook as "triangles"). `/gltest` grows 16
eligibility checks (411 → 427 — guest pixel asserts cannot see a GPU
frame BY DESIGN, and that is the DoD working as stated), run under a
forced stand-in backend whose `init()` accepts exactly once, so it can
drive the screen without hijacking the G13 block's decline fall-
through; `test_virgl_gpu.sh` greps the new lines. The plan's
scanout-hash suggestion needs a host GL on the CI machine, which does
not exist here; the case says so where the assertion would have been.

**Definition of done:** with a VirGL GPU, one `glDrawArrays` triangle
is visible on the scanout and is **not** in the CPU colour buffer;
without a GPU, behaviour is bit-identical to G13 (D3); the backend
struct has `draw`. — **met on the seam; the scanout half is code-
complete and host-verified to the validator, and awaits the same real
device the virtio driver work awaits.**

**Test gate:** `test_glvirgl` host (73); `test_virgl_gpu` QEMU
(skip-ok, log assertions added); `make test-unit` EXIT 0; existing
`/glcube` on software unchanged. — **green.**

---

### L7 — Close-out: docs, residue, checker, arithmetic

**Status: ✅ COMPLETE** (`patches/GL2_L7_closeout.patch`).

**Objective:** make it impossible for this document and the tree to
disagree about what landed, and hand off what did not.

Tasks:

- [x] Checker: header `COMPLETE ⇔` L0–L7 all ✅; every moved opener
      pin has a *post-phase* assertion (stencil tokens present,
      `GL_MAX_TEXTURE_UNITS_IMPL == 4`, `draw` member exists,
      `GL_COMBINE` in `gl.h`, `docs/opengl.md` hang sentence
      absent, `/glshade` packaged). `--selftest` still catches a
      planted miss.
- [x] `docs/opengl.md`: Not-implemented table matches §4 of this
      plan (the rows this series refused, not the rows it landed).
      Behaviour notes for stencil order, COMBINE subset, canned
      TGSI, per-fragment LOD axes.
- [x] `docs/status.md` OpenGL cell: this series' headline, not
      "G0–G9 complete" as if G10–G13 and L* did not happen.
- [x] `README.md` documentation map: `GL2_PLAN.md` next to
      `GL_PLAN.md`. Apps table: `/glshade`.
- [x] `CHANGELOG.md` one entry per landed phase (hygiene, same
      commit as the code — by L7 this is a backstop, not the
      first mention).
- [x] Residue: RES-41 closed (canned `DRAW_VBO` seam). New S-row:
      GLSL AST → TGSI retarget (the D7 hand-off). SIMD rasteriser
      re-affirmed deferred unless L5's table reopened it.
- [x] §5 of this document filled. Header Status → COMPLETE.

A close-out should be boring; this one is, by construction — every
number below was already machine-checked before this section was
written, and the writing only had to agree with it.

*The checker.* 52 claims (44 at L6 + 8 added here: post-phase pin
assertions for the pins the series moved — stencil accepted after L1,
the copies in the public header after L2, four units and `GL_COMBINE`
after L3 — plus the close-out claims that this file's header is
COMPLETE with every phase row agreeing, that `docs/status.md` names
this series, that the README map points both GL plans at `docs/plans/`,
that the ledger closes RES-41 and opens RES-54, and that
`docs/opengl.md`'s Not-implemented table enumerates §4). The header-vs-table claim
now runs against a COMPLETE header with 8/8 rows, which is what makes
"CI fails if this file and the tree disagree" a live property rather
than a past tense. `--selftest` still catches a planted miss.

*The docs.* `docs/opengl.md`'s Not-implemented table now enumerates §4
(the rows this series refused: the TGSI compiler and JIT, the
preprocessor, VAOs/PBOs/transform feedback and friends, the SIMD
rasteriser whose measured floor L5's table did not reopen, ES 3.0) —
and the behaviour notes the plan asked for had accumulated with their
phases: stencil order at L1, the COMBINE subset at L3, per-fragment
LOD axes at L4, the canned TGSI path at L6. `docs/status.md`'s OpenGL
cell ends with this series' headline instead of stopping at G13. The
README documentation map grew the GL2 row next to GL_PLAN's — and the
GL_PLAN link now points at `docs/plans/`, where this series' L0 had
moved every plan; the map had been pointing at the old root paths
since then, which is exactly the class of rot this phase exists to
sweep.

*The ledger.* RES-41 closes DONE@L7 with the seam it asked for; its
compiler half was never part of the claim and opens as RES-54 with a
measured opener. The debt checker's terminal R12 gate counted exactly
eight HANDED-OFF rows; it now counts "at least seven" — the floor is
the invariant, not the number.

*§5* is filled and matches the tree row for row; the arithmetic in the
checker (`sizeof(aglx_context)` 238 568 → 240 328 across the series;
1064 libgl host checks; 427 in-OS) is grep-backed. **GL2_PLAN.md is
COMPLETE: L0–L7, eight phases, each delivered, measured and closed in
the commit that landed it.**

**Definition of done:** CI fails if this file and the tree disagree;
the leftover table in `docs/opengl.md` is the leftover table in §4;
RES-41 is not still "the missing piece is a userspace TGSI
assembler" when that assembler exists and draws a triangle. — **met:
the ledger row names the seam DONE and points the retarget at RES-54.**

**Test gate:** `test_gl2_claims.sh` green; `make test-unit` EXIT 0. —
**green.**

---

## 4. What this plan deliberately does not do

Named so nobody mistakes absence for oversight.

- **Geometry, tessellation, compute shaders.** ES 2.0 is VS+FS.
  G11's interpreter has one extra stage in it already (the
  fragment); a third is a different machine.
- **Accumulation buffer, evaluators, feedback, selection.** Fixed-
  function museum pieces. No shipped demo wants them; they do not
  unblock COMBINE, stencil, or VirGL.
- **Multiple colour attachments / MRT / `gl_FragData[n]`.** G12
  sized the loops against `GL_MAX_COLOR_ATTACHMENTS_IMPL` so this
  can happen without a rewrite; it still needs a shader that writes
  more than one colour, which is ES 3.0-shaped, and the software
  fill would pay it twice. Not this series.
- **GLSL → TGSI compiler, and any JIT.** D7. L6 is the seam; a
  compiler plan is the successor.
- **GLSL preprocessor (`#define` / `#ifdef`).** G11a refused it
  with a diagnostic. Reversing that is language work, not GL work.
- **`glBegin` with a bound program.** D8, G11d stands.
- **VAOs, PBOs, transform feedback, `glCopyTexImage3D`, packed
  depth-stencil, `glFramebufferTexture3D`.** Each is real; none is
  on the critical path of L1–L6.
- **User-space SIMD / SSE rasteriser.** Fact 8, OPT's non-goal,
  re-affirmed. L5's table is the only thing allowed to reopen it.
- **ES 3.0 / desktop 3.2 core profile.** The subset grows (GL_PLAN
  D2); it does not jump a generation because a continuation plan
  felt ambitious.
- **New kernel syscalls.** `SYS_GPU_CALL` already carries SUBMIT /
  TRANSFER / RES_CREATE / SET_SCANOUT / FLUSH. L6 uses them.
- **Re-opening G0–G13.** Complete means complete. Bugs found while
  landing L* are bugs and get fixed in the phase that found them,
  not a secret G11e.

---

## 5. Terminal arithmetic (filled at close — D1)

Baseline column is L0's job. Later columns land with their phase.

| Metric | Baseline (`dbc27fe` + L0) | L1 | L2 | L3 | L4 | L5 | L6 |
|---|---|---|---|---|---|---|---|
| `GL_MAX_TEXTURE_UNITS_IMPL` | 2 | | | **4** | | | |
| Stencil | refused | **8-bit** | | | | | |
| `glMatrixMode(GL_TEXTURE)` | `INVALID_OPERATION` | | | **legal** | | | |
| `GL_COMBINE` | absent | | | **present** | | | |
| Mipmap LOD | per-triangle | | | **per-fragment** | | | |
| Lambert FS 320×240 (ms) | 53.8 (G11c) | | | | | **38.9–45.8; behind a wall 1.1** | |
| `/glshade` | absent | | | | | **shipped** | |
| VirGL draw | present-only | | | | | | **canned Δ** |
| `sizeof(aglx_context)` | **238 568** | **239 384** | **239 384** | **240 304** | **240 304** | **240 304** | **240 328** |
| libgl `UNIT_TESTS` binaries | 17 `test_gl*` (glmath…glvirgl) | +`test_glstencil` | **18** | **18** | **18** | **18** | **18** |
| `/gltest` in-OS checks | 373 (docs; not gated) | **388** | **402** | **411** | **411** | **411** | **427** |

Residue opened at L7 (expected):

| Item | Class | Notes |
|---|---|---|
| GLSL AST → TGSI | S | D7 hand-off; successor plan, not a leftover we forgot |
| SIMD rasteriser | N | re-affirmed unless L5 reopens |
| Packed D24S8, MRT, VAOs | N | §4 |
| `docs/opengl.md` hang sentence | — | closed at L0 |

---

## Workflow (mandatory for every phase)

The `GL_PLAN.md` / `HARDENING_PLAN.md` loop, unchanged:

```
1. READ    — read ALL affected files before writing anything
2. PLAN    — this file: Status → IN PROGRESS on the phase
3. DESIGN  — show struct/API changes, list callers; cite a D-number
             instead of re-arguing
4. IMPL    — libgl → host test → /gltest → demo → docs
             compile after every file; fix warnings immediately
5. BUILD   — make clean && make all  (zero warnings, -Wall -Wextra -Werror)
6. TEST    — host gate; QEMU gate; existing suites unmodified (D3)
7. DOCS    — this file's Result + table tick, CHANGELOG.md,
             docs/opengl.md Not-implemented row, checker pin moved
             in the SAME commit (D6)
```
