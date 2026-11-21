# GPU acceleration: what moves, what does not, and what to assume

`libgl` renders on the CPU. `docs/opengl.md` describes that implementation and
remains the reference for the software path. This document is about the **GPU
seam**: the narrow, deliberate boundary through which some work can be handed
to a virtio-gpu, and the rules a reader has to know before touching it.

The series that built it is `docs/plans/GPU_ACCEL_PLAN.md` (phases H0–H7).
Read this before changing anything on the seam; read the plan before changing
what the seam accepts.

---

## The shape of the seam

One hook, one unit of work:

```c
int (*draw)(struct aglx_context *ctx, const gl_draw_batch_t *batch);
```

A batch is **whole triangles from one `glDrawArrays` call**, and the fallback
unit is the whole draw — never one triangle inside it. A frame half drawn by
the GPU and half by the CPU is a tearing bug with no good failure mode, so the
decision is made once per draw call and is not revisited mid-draw.

Three properties follow, and all three are load-bearing:

1. **The batch carries CLIP-space positions** — the values the fixed-function
   transform already computed. The GPU's divide, clip and viewport therefore
   reproduce the CPU rasterizer's own arithmetic rather than reinventing it.
2. **The batch carries POST-LIGHTING colour.** Lighting does not move to the
   GPU. It is per-vertex, G7 measured the transform (not the shading) as the
   cost, and keeping it on the CPU means a lit draw and an unlit one produce
   colour the same way.
3. **A backend that says nothing gets nothing.** Every widened capability is
   opt-in through a flag; a backend that does not declare one is never handed
   a batch shape it does not understand.

## Batch formats are versioned, not mutated

| Format | Floats/vertex | Contents |
|---|---|---|
| `GL_BATCH_FMT_POS_COLOR` (0) | 8 | clip `xyzw`, colour `rgba` |
| `GL_BATCH_FMT_FULL` (1) | 12 | the above, then normal `xyz`, `texcoord0.s` |

Format 0 is what GL2 L6 shipped, byte for byte. Format 1 is requested only when
the backend declares `GL_BACKEND_DRAW_FULL`. `GL_BATCH_STRIDE(fmt)` is the one
definition of the width — the batch builder, the backend and the tests all use
it, and the setup stream's vertex-element description is derived from it rather
than restating it.

The plan's first H5 bullet says "eight floats become twelve" and then lists
fields that sum to thirteen (position 4 + colour 4 + normal 3 + texcoord 2).
Twelve won: the texcoord contributes its `s` lane and the `t` it would need is
not carried, which is sound precisely because the seam allows one 2D texture
unit — a draw that needs `t` is refused. Twelve keeps every vertex 4-float
aligned, which the chunk arithmetic and the vertex-element stride both assume.

## The eligibility screen: nine refusals, four of them conditional

`gl_backend_draw_eligible()` refuses a draw unless everything it needs can be
reproduced exactly. H5 took the count from thirteen to nine. Four states moved
out of the refusal list and into a **conditional** function — they are allowed
only when the backend declares the matching capability and the sub-state is one
the uploaded device state actually encodes:

| State | Allowed when | Refused because |
|---|---|---|
| depth test | backend has `GL_BACKEND_DEPTH`, function is `GL_LESS`, writes on | the DSA and the depth surface are uploaded with that exact comparison; any other function would test differently |
| blending | both factors map 1:1 onto a gallium factor | a substituted factor yields a plausible, wrong frame |
| culling | `GL_CCW` winding, `GL_BACK`/`GL_FRONT` | a winding flip would cull the other side |
| texture | one 2D unit, unit 0, `GL_MODULATE`, backend has `GL_BACKEND_TEXTURE` | a second unit, cube or 3D map is outside the canned shader |

The nine that never change and their reasons: non-triangle modes and partial
triangles (the canned primitive is whole triangles); a draw whose position does
not come from an array (immediate mode flushes per primitive, which is
sub-draw granularity); an active shader (fixed function only); stencil (no
stencil surface on the device side); alpha test, scissor and fog (fragment- or
raster-stage operations the canned pipeline does not perform); non-`GL_FILL`
polygon mode.

**Why the split is a function and not a longer list.** The plan promised a
count of *states*; four of them grew sub-conditions, and inlining those
sub-conditions into the outer screen would have pushed its statement count to
eighteen while the honest state count stayed nine. `h5_state_is_reproducible()`
holds the conditional four, the outer function holds the unconditional nine,
and both are pinned.

## Behaviour worth knowing

**The front framebuffer is not written when scanout owns the display.** H2
elides the back→front copy while the GPU is scanning out. Anything that reads
`front_fb` would therefore see a frame one behind — which is why the phase
asserts that nothing outside `graphics.c` reads it.

**A GPU-drawn frame cannot be read back by the guest.** `GPU_OP_TRANSFER` is
one-way: it calls `virtio_gpu_resource_upload()`, and there is no download
path in this tree's driver. This is not an oversight in the code — it is why
the H6 oracle states plainly what it can and cannot prove (see below). Do not
write a test that assumes a hardware frame will appear in guest memory.

**Gallium enums are not GL enums.** `GL_LESS` is `0x0201`; `PIPE_FUNC_LESS` is
`1`. `GL_SRC_ALPHA` is `0x0302`; `PIPE_BLENDFACTOR_SRC_ALPHA` is `5`. The
device state is uploaded in gallium terms, and sending the GL value is not
merely off by a constant — it selects a different comparison. The seam's own
tests assert the numeric words rather than that "something was set".

**A depth test without a depth surface is a lie.** A DSA object with the test
enabled and no `zsurf` in the framebuffer tests against nothing: the host
either ignores it or faults. The backend therefore creates a
`PIPE_FORMAT_Z32_FLOAT` resource, attaches it, clears it to `1.0` once, and
destroys it with its render target.

## The frame oracle, and its honest boundary

`lib/libgl/src/gloracle.{h,c}` render a deterministic scene — fixed geometry,
fixed pixel-space orthographic projection, no clock, no RNG, no allocation —
and hash it with the kernel's own `ksha256`, so the host lane and the guest
lane hash with the same code.

| Lane | Proves | Cannot prove |
|---|---|---|
| Host (`test_gloracle`) | determinism; the hash sees one pixel, a flipped row, a shifted scene, a swapped channel; the tolerance accepts an epsilon and rejects a band, at the exact boundary | anything about a GPU, which is absent |
| Guest (`/gltest`) | the scene renders and hashes in the guest — `93ea06fd…e87cb`, stable across boots and across two renders in one boot | the hardware frame's pixels: unreadable, per the upload-only ABI |
| Device (`test_virgl_gpu.sh`) | the oracle ran; the hash is stable; the pin is asserted when `H6_SOFTWARE_HASH` is set | the kernel of the matter, in an environment with no `virtio-gpu-gl-pci` |

The tolerance is a stated number, not a buried constant: **5 differing pixels
per thousand** (two at 64×64, room for the four edge pixels of a triangle's
perimeter to round differently) and a **per-channel epsilon of 1/255**. A
channel difference past the epsilon makes the whole pixel count as differing —
that is the rule that stops a half-brightness band from hiding inside "small
numeric noise". Both numbers live in `gloracle.h` and are quoted in the plan;
the checker compares the two so they cannot drift.

The scene deliberately does not texture: sampler filtering is exactly the kind
of thing that legitimately differs between implementations, and an oracle that
tolerated it would tolerate away real regressions.

## The TGSI back end, as far as it goes

H4 retargets the GLSL AST to a TGSI token stream. The subset that maps 1:1
onto opcodes works: `add`/`sub`/`mul`, `dp2`/`dp3`/`dp4`, `rsq`, `sqrt`, `abs`,
`flr`, `frc`, `ex2`, `lg2`, `min`, `max`, `normalize`, `length`,
`inversesqrt`, and the constructors. Anything else — matrices, `texture2D`,
control flow, compound assignment, user functions, `tan`/`asin`/`pow`/`mod`/
`sin`/`cos` — is refused **by name** with a diagnostic naming the construct,
and the interpreter draws the shader instead. A refusal that names its reason
is a readable failure; a wrong stream the device accepts and misinterprets is
not.

The retarget's tests carry an independently written disassembler, so the
encoding is checked against a second reading of the Gallium specification
rather than against the emitter's own constants. That is how H4 found that the
canned TGSI this tree has shipped since G13 **is not valid TGSI** — six
defects, recorded in the plan's H4 Results. It was never executed on hardware,
which is exactly why it survived.

## What a reader must not assume

- That a hardware draw happened. Without a device the seam declines and the
  software path runs unchanged; the counters read zero.
- That the GPU path is pixel-equivalent to the CPU path. It is asserted within
  a stated tolerance on the host; on a device the comparison does not yet
  exist, and the plan says so rather than implying it.
- That a widened capability is unconditional. Depth, blend, cull and texture
  are each gated on a backend flag *and* on the exact sub-state uploaded.
- That lighting moved. It did not, and the batch's colour field is the proof:
  it is post-lighting, computed on the CPU.
