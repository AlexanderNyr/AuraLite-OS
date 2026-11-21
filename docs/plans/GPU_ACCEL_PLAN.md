# AuraLite OS — GPU Acceleration Plan

## Status: IN PROGRESS 🚧 — H0 (the rig) and H1 (the present accounting) landed; H2–H7 planned

| Phase | Result | Deliverable |
|-------|--------|-------------|
| H0 — the rig | ✅ complete | `patches/GPU_H0_rig.patch` |
| H1 — the present accounting | ✅ complete | `patches/GPU_H1_present.patch` |
| H2 — 2D scanout as the default path | ⬜ planned | `patches/GPU_H2_scanout.patch` |
| H3 — texture resources on the GPU | ⬜ planned | `patches/GPU_H3_textures.patch` |
| H4 — the TGSI back end | ⬜ planned | `patches/GPU_H4_tgsi.patch` |
| H5 — hardware fixed-function draws | ⬜ planned | `patches/GPU_H5_draws.patch` |
| H6 — the VirGL frame oracle | ⬜ planned | `patches/GPU_H6_oracle.patch` |
| H7 — CI lane, docs, close-out | ⬜ planned | `patches/GPU_H7_closeout.patch` |

This document answers:

> *`GL_PLAN.md` G0–G13 and `GL2_PLAN.md` L0–L7 are both closed. Between them
> they proved the VirGL seam end to end — a real context, a real backed
> resource, a real command stream accepted by a real device — and shipped
> exactly one canned triangle through it. The GPU is now the most
> under-used capable thing in the tree: the 2D scanout path runs on every
> flip in every QEMU VM, `SYS_GPU_CALL` has nine sub-ops with a tested
> validator, and **the 3D path can draw exactly one kind of triangle that
> no shipped demo uses**.
> What is the honest, measured plan to change that — and what stops the
> plan from becoming "port Mesa"?*

It follows `GL2_PLAN.md` and `OPT_PLAN.md`: measured opener, numbered
decisions, dependency-ordered phases, a test gate and a definition of done per
phase, Results filled with numbers rather than adjectives, named non-goals,
and terminal arithmetic. A new letter — **H** — because `G` was spent by the
GL series, `L` by its continuation, and re-using either would make every grep
ambiguous. The residue ledger's row numbering is untouched: this plan *closes*
rows, it does not renumber them.

**Baseline:** commit `366825f` (HEAD at the time of writing, branch `main`).
Every claim below was checked against the tree or measured in QEMU on a
2-vCPU TCG host, not assumed. Line numbers drift; the *claims* are what the
phases answer to, and H0 pins the ones that must not silently rot.

Baseline artefacts, measured on this machine (Debian 13, QEMU 10.0.13,
clang 19, rustc 1.98.0, no `/dev/dri`):

```
build/kernel.elf        28 775 984 bytes
build/initrd.tar        13 342 720 bytes
build/auralite.iso      84 934 656 bytes
boot → "shell active"   175 ticks (~1 767 ms @ 99 Hz)   [virtio-gpu, fast]
boot → "shell active"   195 ticks (~1 969 ms @ 99 Hz)   [Bochs VGA, fast]
```

---

## 1. Where this plan comes from

### Fact 1 — The GPU path that actually runs today is 2D, and it works

Booting with `-device virtio-gpu-pci` (no `gl=on`) reaches the shell and
prints, verbatim:

```
[virtio-gpu] found modern GPU at PCI 0:3.0
[msi] virtio-gpu: MSI-X vector 96 (1 msg, 3 entries)
[virtio-gpu] scanout 0: 1280x800 flags=0x0
[virtio-gpu] ready: virgl=0 enabled=0 size=1280x800
[msi] virtio-gpu: 1 interrupt(s) delivered on vector 96
[boot] virtio-gpu acceleration ready: virgl=0 size=1280x800
[3d] backend=virtio-gpu-2d-ready+software-zbuffer flags=0xb gpu=Virtio GPU (1af4:1050) zbuf=0x0
```

That is a **working virtio-gpu 2D driver with MSI-X**, initialised before the
compositor, wired into `gfx_flip()` and `gfx_flip_rect()`
(`drivers/framebuffer/graphics.c:1616`, `:1661`). The present path is
`RESOURCE_CREATE_2D` + `ATTACH_BACKING` + `TRANSFER_TO_HOST_2D` +
`RESOURCE_FLUSH` (`drivers/gpu/virtio_gpu.c:781–823`).

The 3D path is **not** running in this configuration: `virgl=0`. The driver
negotiates `VIRTIO_GPU_F_VIRGL` only when the device offers it
(`virtio_gpu.c:862`), and QEMU offers it only for `virtio-gpu-gl-*` with a
host GL context. This host has no DRM render node, so
`-device virtio-gpu-gl-pci` refuses to start:

```
qemu-system-x86_64: egl: no drm render node available
qemu-system-x86_64: egl: render node init failed
```

**Consequence for this plan:** every VirGL claim is *loudly skipped* on this
machine (D2 below, inherited from GL2's D2), and the 2D path is the one that
must be made honest, measured and default-on, because it is the only GPU
path a CI runner and a normal QEMU user will ever execute.

### Fact 2 — The 2D present is a double copy, and nothing measures it

`gfx_flip()` (`graphics.c:1603–1619`) does two full-screen memory moves per
frame:

1. `back_fb → front_fb` — one `memcpy` per scanline (pitch bytes), the
   framebuffer copy;
2. `back_fb → gpu_fb` inside `virtio_gpu_present()` — another full-screen
   copy into the virtio-gpu backing store (`virtio_gpu.c:797–802`), then
   `TRANSFER_TO_HOST_2D` of the whole rectangle.

At the 1280×800 scanout geometry that is **4 096 000 bytes copied twice** per
frame — **8 192 000 bytes**, plus one control-queue round trip, plus a
host-side texture upload. At the 1024×768 geometry the compositor actually
draws, the same pair is 3 145 728 × 2 = **6 294 656 B**; **H1 later measured
the real figure at 6 094 848 + 6 094 848 = 12 189 696 B for a 15-present
frame set and found the ratio exactly 2.000×** (see H1 Results). The
compositor's own counters (`PERF_COMPOSITOR_PIXELS_FLIPPED`) count the
*kernel-side* flip, not the GPU transfer, and — before H1 — there was no
counter at all for the virtio-gpu hop:
`grep -c 'GPU\|VIRGL\|VIRTIO_GPU' kernel/lib/perfstat.h` returned **0**.

So the honest statement about the current GPU acceleration is: *it may be a
pessimisation on a machine where the front framebuffer is also memory, and
nobody can say, because nothing counts it.* That is exactly the class of
claim `OPT_PLAN.md`'s rule exists to forbid.

### Fact 3 — The 3D seam is real, and it has exactly one triangle in it

`SYS_GPU_CALL` (203) is implemented, documented and tested
(`kernel/gpu/gpu_syscalls.c`, 437 lines; `gpu_cmdcheck.c`, the validator,
64 lines; `tests/unit/test_gpu_syscall.c`, 18 checks). It carries nine
sub-ops, per-process handles, quotas (4 contexts / 64 resources / 64 MB /
256 KB per stream), TOCTOU-free validation on a kernel-side copy, and reaping
on process exit.

`lib/libgl/src/glvirgl.c` (623 lines) drives it: probe → context →
render-target resource → `CLEAR` → canned `CREATE_SHADER`/`DRAW_VBO`
(L2 L6) → present fork. `gl_backend_t` has a `draw` hook and libgl's
`gl_backend_draw_eligible()` screens every draw against the state the canned
pipeline cannot reproduce.

What the canned pipeline accepts, precisely (`glbackend.h:gl_draw_batch_t`,
`glvirgl.c:gl_virgl_canned_setup`): whole `GL_TRIANGLES` batches of
**clip-space xyzw position + rgba colour only**. No lighting, no texture, no
normal, no fog, no blending, no depth, no scissor, no cull. Anything else
takes the whole draw back to software.

In other words: the GL stack has a **GPU that can draw flat, untextured,
unlit triangles** — while `/glcube`, the OS's own flagship demo, draws lit,
textured, depth-tested, mipmapped geometry and is therefore 100% software on
every machine, GPU or not.

### Fact 4 — The missing piece is named, owned, and measured: RES-54

`docs/residue_ledger.md` RES-54, class S, OPEN, handed off by GL2 D7:

> GLSL AST → TGSI retarget … The canned triangle GL2 L6 shipped is
> hand-written TGSI dword arrays in `glvirgl.c`, and G11's compiler still
> produces an interpreted AST with no back end.

The GLSL front end (G11a) produces a **typed AST** with a complete type
checker; the execution engine (G11b) walks it. A TGSI back end is a
*consumer of that AST*, not a new compiler: emit interface declarations from
the linked uniform/varying/attribute tables, walk the expression tree once,
emit `TEMP`/instruction tokens, done.

Two hard facts bound it, both measured in G11c and GL2 L5:

| Path (full-screen quad @ 320×240) | ms/frame |
|---|---:|
| Fixed function | 0.92 |
| Constant-colour shader (interpreted) | 12.1 |
| Lambert-lit shader (interpreted) | 53.8 |
| Lambert-lit shader, behind a wall (early-Z) | 1.1 |

The interpreter is **13–58× the fixed-function path**; early-Z recovered an
order of magnitude in the case that mattered. A TGSI back end moves the
fragment stage off the CPU entirely, which is the only fix that scales with
resolution — a JIT would not, which is why D4 below refuses one again.

### Fact 5 — The 2D path has one latent hazard worth naming now

`submit_2desc()` (`virtio_gpu.c:578–606`) and `submit_desc_chain()`
(`:608–623`) both busy-wait for `vq_used->idx` to move, with
`__asm__("pause")` and a `10 000 000`-iteration timeout. The control queue is
serialised by this spin, and **nothing in the tree creates a second virtqueue
for the cursor queue (queue 1)**, which virtio-gpu uses for cursor updates.
That is fine today — the compositor never issues a cursor update — but it
becomes a visible stall the moment H2 puts a hardware cursor on the scanout.

This is recorded as an *expected* residue row, not a bug: the plan does not
open the queue until a phase needs it.

### Fact 6 — `docs/opengl.md` and `docs/status.md` are honest, and say "no"

Both already refuse to claim hardware drawing:

- `opengl.md`, *Not implemented*: "Hardware-accelerated **drawing** beyond the
  canned triangle … General hardware drawing needs the GLSL compiler
  retargeted to TGSI — a compiler back end and a successor plan (ledger
  RES-54)."
- `status.md`, OpenGL row: 🧪 with "one named remainder — the GLSL AST → TGSI
  retarget, RES-54, owned by RESIDUE3 C9."

This plan is the successor those two sentences promise. It must not weaken
either sentence until a phase makes it false — and when a phase does, both
move in the **same commit** (D6).

### Fact 7 — The desktop is already data-oriented, which is what makes H2 possible

The compositor is Rust (`kernel/gui/compositor.rs`, 1 233 lines; `dirty.rs`,
338; `render.rs`, 417; `framebuffer.rs`, 145) behind a narrow C boundary, and
it already computes a **dirty-rect union** per frame (OPT O4) and presents
through exactly two functions: `gfx_flip()` and `gfx_flip_rect()` — `kernel/gui/framebuffer.rs:122,129`.

That is the whole seam a damage-driven GPU present needs. A compositor that
flipped scattered rectangles itself would need surgery; this one hands two
calls a rectangle it already computed.

---

## 2. Decisions

Numbered so later phases cite them instead of re-arguing. `GL_PLAN.md`
D1–D4 and `GL2_PLAN.md` D1–D8 still apply. The ones below are this series'
own.

**D1 — The 2D path is the GPU acceleration that ships; VirGL drawing is the
one that is proved.** Ranked by *who can run it*: a CI runner and a default
QEMU invocation have a `virtio-gpu-pci` with `virgl=0`. A plan that spends
all its phases on a path those machines cannot enter has optimised for the
author's desktop. So the order is: make 2D honest and default (H1, H2), then
grow the 3D draw surface (H3–H5) behind caps that decline cleanly, then put
the frame-equivalence oracle in place that *both* paths answer to (H6).

**D2 — A missing GPU is a loud SKIP, not a fail; a missing `virgl=1` is a
second loud skip.** `test_virgl_gpu.sh` already has this shape. Software-path
claims and 2D-path claims run everywhere; VirGL-draw claims run only on a host
with a render node, and the log says which. No phase may report "hardware
drawing works" based on a run where the backend declined — GL_PLAN G13's own
rule, applied to a whole series.

**D3 — Every frame claim carries a byte count and a time, or it is not a
claim.** The counters go into `perfstat` (the OPT O0 rig) so `/proc/perf`
carries them, because that is the only measurement channel this tree already
trusts (`test_perf_smoke` parses the names). New counters are an interface
change and are named like the existing ones.

**D4 — No JIT, no Mesa, no Gallium state tracker, no Vulkan.** The TGSI
back end is a *tree walk that emits tokens*, sized like the front end that
already exists (2 400 lines) rather than like a device driver. A JIT of the
interpreted AST is refused again for the reason GL2 D7 gave: it moves the
same per-component float work into machine code and still runs once per pixel,
whereas a GPU draw removes the pixel work entirely. "Port Mesa" is not a plan,
it is a fork.

**D5 — The software path stays the default and the CI path; hardware is
opt-in behind a probe.** `glGetString(GL_RENDERER)` stays truthful (G13/GL2
D5). The 2D scanout is the one exception in the other direction: it is
*already* on by default when a virtio-gpu is present, and H1/H2 must justify
that with numbers or turn it off.

**D6 — The checker judges code and gates, not `.patch` files.** `REALINTERNET2_PLAN.md` D6. Phase hygiene still holds:
`CHANGELOG.md`, `docs/opengl.md`'s Not-implemented row, `docs/status.md`'s
cells, the residue ledger rows this series closes/opens, and the checker pins
move in the **same commit** as the phase.

**D7 — Hardware draws must be pixel-checkable against software, or they do
not ship.** A GPU path that renders "something plausible" is the worst
outcome: it looks right on the cube and is wrong on the next scene. H6 is the
phase that makes this enforceable: a deterministic scene rendered twice (GPU
and software) and compared within a *stated* tolerance, with the tolerance
justified per feature (fixed-point rasterisation differences are real; colour
bands that move with resolution are not).

**D8 — No new syscalls this series.** `SYS_GPU_CALL` already carries
SUBMIT / TRANSFER / RES_CREATE / SET_SCANOUT / FLUSH. H3–H5 use them. If a
phase believes it needs a new sub-op, that is a signal the design is wrong,
not the ABI.

---

## 3. The phases

Dependency order: rig first (H0), then the present accounting that decides
whether the 2D path is a win at all (H1), then the 2D scanout made into the
default damage-driven path (H2), then the three 3D steps in ascending risk
(textures H3, shader back end H4, fixed-function draws H5), then the oracle
that both paths answer to (H6), then close-out (H7). H3 does not depend on
H4; H4 does not depend on H3; both are prerequisites for H5's usefulness and
neither is allowed to land after H5.

### H0 — The rig

**Status: ✅ COMPLETE** — `tools/check_gpu_claims.py`, 23 pins,
`tests/unit/test_gpu_claims.sh` wired into `make test-unit`; the three
findings of the first run are in §8.

**Objective:** make every later claim in this plan checkable by `make`, and
pin the opener facts that must not silently rot.

Nothing in the tree today notices if a phase claims a GPU win with no counter
to prove it. This phase builds the tripwire; it implements no GPU work.

**Tasks:**

- [x] `tools/check_gpu_claims.py` (the `check_gl2_claims.py` shape: opener
      facts pinned as live greps, pins move in the same commit as the phase
      that takes them; `--selftest` negative control wired into
      `make test-unit` via `tests/unit/test_gpu_claims.sh`). 23 pins,
      phase-gated: a pin binds while the phase that moves it is not
      ✅ COMPLETE.
- [x] Opener pins, all live against `366825f`:
      1. `perfstat.h` contains **no** GPU/virgl counter (the H1 target).
      2. `graphics.c`'s `gfx_flip()` still calls `virtio_gpu_present()` on a
         second full-screen copy.
      3. `glbackend.h`'s `gl_draw_batch_t` is still `{data, count}` with the
         `position + colour` vertex format in its comment, and
         `gl_backend_draw_eligible()` still contains all **13** refusal
         conditions (counted, not estimated: mode, whole-triangles, array
         source, shader, depth, stencil, blend, alpha, scissor, cull,
         polygon mode, fog, texture units).
      4. `glvirgl.c` still contains the `canned_vs_tgsi` dword array (the H4
         target: the array is *replaced* by generated tokens, and the pin
         moves when it does).
      5. `check_gpu_claims.py` pins RES-54's status as OPEN until H4/H5 flip
         it.
      6. `virtio_gpu.c` still busy-waits in `submit_2desc()` (Fact 5).
- [x] `perfstat` gains the plan's counter *names* as part of H1, not H0;
      H0 pins their absence so H1 cannot forget (pin 1: zero GPU/virgl
      matches, and all 8 H1 names absent).
- [x] Record the live baseline into §5 of this document as the before-column:
      boot ticks, `/proc/perf` compositor counters, `sizeof(aglx_context)`,
      the `test_glvirgl` inventory. The `/gltest` SUMMARY line is the one
      cell still open — it needs an in-OS run after the next demo build, and
      §5 marks it as such rather than inventing a number.

**Test gate:** `tools/check_gpu_claims.py --selftest` catches a planted
violation; `make test-unit` EXIT 0.

**Definition of Done:** CI fails if this file and the tree disagree.

**Deliverable:** `patches/GPU_H0_rig.patch`

**Results:** the rig is live and it earned its keep on the **first run** —
it caught three things the plan had written loosely, before any GPU code
existed:

1. `gl_backend_draw_eligible()` has **15** `return 0;` statements, not 13.
   Two are preconditions (`!ctx`, no backend) and **13** are state refusals —
   the plan's number was right about the refusals and would have been wrong
   about the function. The checker now pins both (13 refusals, 15
   statements, and the split), so a future phase cannot "fix" the count by
   folding a precondition in.
2. The status vocabulary: the plan opened as `PROPOSED`, which no other plan
   in `docs/plans/` uses for a not-yet-started series (`PLANNED` is the
   tree's word). The checker accepts both and the header moved to
   `IN PROGRESS 🚧` when H0 landed, which is the form the 18 existing plans
   use.
3. The debt ratchet (`tools/check_residue_claims.py`) went red on the new
   file the moment it existed — 8 residue-marker lines in a plan that had
   landed nothing. That is the ratchet working: the baseline moved in the
   same change (`tools/residue_baseline.txt` gains `GPU_ACCEL_PLAN.md 8`),
   which is exactly what RESIDUE3 C0 asks of every new plan, and the whole
   20-checker suite is green again (42 claims in the residue checker alone).

The rig also carries the pins that make H1–H7 falsifiable: zero GPU
counters in `perfstat.h`, the second full copy in `gfx_flip()`, the
`{data, count}` batch with **no** `format` field, `canned_vs_tgsi[21]` +
`canned_fs_tgsi[13]` = **34** dwords and no `glsl_tgsi.c`, RES-54 still
`| RES-54 | S | OPEN |`, and the bounded busy-wait in `submit_2desc()`.
Every one of them flips in the commit of the phase that makes it false.

**Test gate (met):** `tools/check_gpu_claims.py` OK — 23 claims;
`--selftest` PASS (doctored tree detected); `bash
tests/unit/test_gpu_claims.sh` EXIT 0 and `make -n test-unit` shows the
wrapper in the recipe.

---

### H1 — The present accounting

**Status: ✅ COMPLETE** — 8 counters live in `/proc/perf`, the three copies
charged separately, `/proc/fb` added, `gpubench` in the initrd, host test
18/18. The measurement **changed the plan's premise**; see Results.

**Objective:** answer, with numbers, whether moving the flips to the GPU is
faster than not doing it — before H2 makes it the default.

This phase implements **no rendering**. It measures.

**Tasks:**

- [x] New `perfstat` counters, named in the existing style:
      `gpu_presents_full`, `gpu_presents_rect`, `gpu_pixels_transferred`,
      `gpu_transfer_bytes`, `gpu_flush_bytes`, `gpu_present_fallbacks`,
      `gpu_control_queue_waits`, `gpu_control_queue_timeouts`.
      Each documented next to its enum entry, because "a counter whose meaning
      drifts is worse than no counter" (`perfstat.h`'s own words).
- [x] Charge the **three** copies separately: the `back→front` memcpy, the
      `back→gpu_fb` memcpy inside `virtio_gpu_present_rect()`, and the
      transfer/flush command pair. Today only the first is visible at all.
      **Measured:** the compositor's own `compositor_pixels_flipped` and the
      driver's `gpu_transfer_bytes` agree to the byte — 15 rect presents
      flipped 1 523 712 px and copied 6 094 848 B into `gpu_fb`, the *same*
      6 094 848 B the back→front copy moved. The two copies are equal by
      construction and now provably so.
- [x] A committed benchmark: `userspace/tests/gpubench` — **changed shape
      during the phase, for a stated reason.**  A user program cannot drive
      `gfx_flip_rect()`: the only syscall that reaches the flip is
      `GUI_OP_RENDER`, and it is gated to pid ≤ 2.  So `gpubench` measures
      the half it *can*: the plain `back→front` row copy at the live
      geometry (read from the new `/proc/fb`), and reports the GPU half from
      the counters that the kernel's own flips charged.  The fixed-format
      table (`GPUBENCH-cpu`, `-gpu`, `-fallback`, `-ratio`) is an interface,
      the membench precedent.  A bench that spawned a private present path
      would have measured a path nothing ships.
- [x] The control-queue wait measured directly — **counted, not timed,
      and the reason is recorded.**  `submit_2desc()` has no TSC→tick
      conversion in the tree (`grep tsc_hz` = 0 hits), so a nanosecond
      charge would have been a guessed constant, which is the class of
      number this plan exists to forbid.  The counter therefore splits the
      **outcome**: `gpu_control_queue_waits` (the used ring advanced inside
      the 10 000 000-iteration budget) and `gpu_control_queue_timeouts` (it
      did not).  Measured: 32 waits / 15 rect presents on the reference
      lane, i.e. **2.13 control-queue round trips per present**, timeouts
      **0**.  The round-trip count is what makes Fact 5's cursor-queue
      decision decidable; the duration would not have changed the decision.
- [x] A documented **decision table** — in Results below and in §5.  The
      headline is not the one the plan expected, and it is a **negative**
      result, which is exactly what D1 authorised.

**Test gate:** `/proc/perf` exposes every new name and `test_perf_smoke`'s
parser accepts it; `gpubench` prints its table under QEMU with and without
`-device virtio-gpu-pci`, deterministically; host unit test for the counter
arithmetic (charged bytes = width×height×4 exactly, per call).

**Definition of Done:** the plan can state, per geometry, which path is
faster *on the measured host*, and the numbers are in §5.

**Deliverable:** `patches/GPU_H1_present.patch`

**Honest expectation, stated before the phase runs:** the 2D path will very
likely **lose** on full-screen frames in TCG — two memory copies and a
synchronous control-queue round trip cost more than one `memcpy` to a
memory-backed `front_fb`. It may win on *rect* presents, where the transfer
is small and the host-side texturing is amortised, and it may win on any host
where the framebuffer is slow (real hardware with uncached MMIO, a
write-combining path). The phase's job is to find out, and §5 is allowed to
say "the 2D path is a pessimisation above N pixels on this host; it stays
opt-in". That outcome is a **pass**, not a failure of the plan.

**Results — measured 2026-11-21, QEMU 10.0.13, 2-vCPU TCG, ISO `399b4c1`
build.** Both runs walked the same 15 rect presents and flipped the same
**1 523 712 pixels**, so the comparison is like-for-like:

| | no virtio-gpu (`-vga std`) | with virtio-gpu (`-vga std` + `-device virtio-gpu-pci`) |
|---|---:|---:|
| `compositor_frames_partial` | 15 | 15 |
| `compositor_pixels_flipped` | 1 523 712 | 1 523 712 |
| back→front bytes copied | 6 094 848 | 6 094 848 |
| `gpu_transfer_bytes` (back→gpu_fb) | — | 6 094 848 |
| **total bytes copied per frame set** | **6 094 848** | **12 189 696** |
| ratio | 1.00× | **2.00×** |
| control-queue round trips | — | 32 (2.13 per present) |
| failed presents (`gpu_present_fallbacks`) | — | 0 |

**The 2D present path is a 2.000× pessimisation on copied bytes on this
host, and it does not buy anything back.** The pre-registered branch is
therefore taken: the path stays **opt-in**, and H2 ships as *threshold +
opt-in*, not as a default flip. Concretely:

1. **Why it loses.** `gfx_flip*()` copies back→front, then
   `virtio_gpu_present_rect()` copies back→`gpu_fb` — the same bytes,
   twice, because the front buffer still has to hold the pixels for a
   memory-scanout display. Nothing in the present path *replaces* a copy;
   it adds one. The only thing that could change that is H2's elision
   (the scanout resource *becomes* the front buffer), which is why H2 is
   still worth doing — but as an elision, not as a parallel path.
2. **And it is invisible on the machine where the 3D path would run.** The
   measured arrangement — a `-vga std` framebuffer *plus* a virtio-gpu — is
   what makes both paths alive at once, and that is not a configuration any
   user runs. `-device virtio-gpu-pci -vga none` (the real single-GPU
   configuration) hands the kernel **no linear framebuffer at all**:
   `[vbe] no 32-bpp linear-framebuffer mode`, `[fb] VGA text 80x25`, and
   every `compositor_*` counter reads **0** on all four reads — the desktop
   never draws a pixel, so there is nothing to accelerate. This is Fact 1's
   asymmetry, now measured rather than inferred.
3. **The counter rig paid for itself nonetheless.** `gpu_present_fallbacks`
   stayed at **0** across every lane, which is the machine-checkable form of
   D2: the "the 2D path works" claim is now a claim about the path, not about
   a fallback. And `gpu_control_queue_timeouts` = 0 with 2.13 round trips
   per present means Fact 5's busy-wait is **not** the bottleneck — the
   cursor queue is therefore **dropped from H2's scope on a measured
   ground**, exactly as Fact 5 said it would be if the numbers allowed it.

**Test gates (met):** `test_perfstat_gpu` **18/18** host checks against the
real `kernel/lib/perfstat.c`; `/proc/perf` lists all eight names on a live
boot; `/proc/fb` prints `1024x768 pitch=4096`; `gpubench` reports
`GPUBENCH-counters 8/8` and runs to `GPUBENCH-DONE` on the boot lane.

---

### H2 — 2D scanout as the default damage-driven path

**Objective:** make the 2D path *earn* its default by presenting exactly the
damage the compositor computed, and by not paying for copies it does not need.

**Tasks:**

- [ ] Skip the redundant copy: when the virtio-gpu path is active and the
      resource is up to date, `gfx_flip_rect()` must not write `front_fb` at
      all — the scanout resource *is* the front buffer. (The front-fb memcpy
      exists for a memory framebuffer; with a scanout there is nothing to
      read it back from.) This removes `gpu_present_fallbacks`' largest cause
      and roughly halves the copied bytes per frame.
- [ ] Damage coalescing: the compositor already unions dirty rects
      (`dirty.rs`); present that union with one `RESOURCE_FLUSH` instead of
      the current one-per-`gfx_flip_rect`. Measured where the cliff is — a
      union that spans more than X % of the screen should degrade to one
      full-screen transfer, and X is a measured number from H1's table, not a
      guess.
- [ ] Asynchronous present for the non-fenced path: issue
      `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` without blocking on the used
      ring when the previous flush has completed, and only spin when it has
      not. Measured with `gpu_control_queue_waits`.
- [ ] Cursor queue (queue 1), *only if* H1's numbers say the control-queue
      spin is material. Fact 5 exists because this is where it would hurt; if
      the measurement says it does not, this item is dropped with a sentence,
      not quietly skipped.
- [ ] `docs/status.md`'s "virtio-gpu 2D scanout" cell gains the measured
      numbers and the threshold.

**Test gate:** `test_virgl_gpu.sh`'s 2D section asserts the reduced copy count
(charges are counters, so the assertion is on values, not on prose);
`test_perf_smoke` unchanged; the desktop still boots to the shell on a
`virtio-gpu-pci` machine and a `-vga std` machine with identical pixel output
(the H6 oracle's frame-hash, in its 2D form).

**Definition of Done:** with a virtio-gpu attached, a full-screen repaint
transfers *one* screen's pixels and copies *one* screen's worth of bytes
(measured: `gpu_pixels_transferred` per frame == 1 024 000 at 1280×800, not
2 048 000 as today plus the front-fb copy); without one, the code path is
unchanged and the counters read zero.

**Deliverable:** `patches/GPU_H2_scanout.patch`

---

### H3 — Texture resources on the GPU

**Objective:** the first genuinely *new* hardware capability since G13 —
uploading a GL texture once and sampling it on the device, instead of
re-uploading every bound texture as a full-frame blit.

Today `glvirgl_present()` uploads the **whole CPU colour buffer** per frame
(`GPU_OP_TRANSFER` of `win_width × win_height × 4`). Textures live entirely
in `libgl`'s memory and are sampled by the software rasterizer. A hardware
draw that wanted a texture would therefore have to (a) exist — H5 — and
(b) have the texture on the device already.

**Tasks:**

- [ ] Resource-per-texture upload with a *dirty* discipline:
      `glTexImage2D`/`glTexSubImage2D` mark a mip level dirty; the backend
      uploads only dirty levels, at `TRANSFER` sizes H1's table says are
      worth it.
- [ ] Mip chains as one resource with `last_level` = the chain length
      (the driver already takes `last_level` in `RESOURCE_CREATE_3D`).
- [ ] `VIRGL_OBJECT_SAMPLER_VIEW` + `SET_SAMPLER_VIEWS` packet builders;
      wrap modes and filters map onto `SAMPLER_STATE` fields — the token
      constants for these come from `drivers/gpu/virgl.h` (the G13 lesson:
      one definition of the wire format in the tree).
- [ ] A **host test that walks the encoding against the kernel validator**
      (`gl_virgl_canned_setup`'s existing seam): sampler-view packets must
      pass `gpu_validate_cmd_stream()` and a deliberately over-long view
      must be rejected.
- [ ] `test_glvirgl` gains the texture-upload inventory; `/gltest` gains a
      check that a texture bound through the backend produces the *same*
      sampled colours as the software sampler on a 2×2 probe (compare in
      software; the GPU path is skipped without a device).

**Test gate:** host `test_glvirgl` green including the new packets;
`sizeof(aglx_context)` re-measured (G10 blew the stack once — any phase that
grows per-context state re-measures it and keeps it off the C stack, GL2's
own rule).

**Definition of Done:** a texture uploaded once can be sampled by a
subsequent hardware draw **without a per-frame re-upload** — visible in
`gpu_transfer_bytes` (the number must stop scaling with frame count).

**Deliverable:** `patches/GPU_H3_textures.patch`

---

### H4 — The TGSI back end

**Objective:** close RES-54. Turn G11's typed AST into a TGSI token stream,
so a shader an application writes can execute on the GPU.

This is the largest phase by far and the one the plan is really about. It is
bounded on both ends by things that already exist:

| Input | Output |
|---|---|
| `glsl_parse()`'s typed AST, its symbol tables, G11c's linked uniform / varying / attribute tables | `TGSI` dwords in the format `canned_vs_tgsi` already demonstrates |

**Scope, stated as a subset and defended:**

- [ ] Vertex + fragment ES 1.0 shaders: arithmetic, swizzle, matrix ops,
      `if`/`for`/`while` with constant bounds, user functions **inlined**
      (TGSI has no calls; inlining is how the real drivers do it), `out`
      parameters as register copies.
- [ ] Interface emission from the linked tables: attributes → `DCL IN[n]`,
      uniforms → constant-buffer slots with a **layout the backend chooses
      once and pins in a unit test** (whether Gallium wants
      `CONST[0]`-relative or a UBO depends on the virglrenderer version — the
      phase measures and documents, rather than guessing, because Fact 4's
      whole lesson is that a guessed protocol constant produces a stream the
      device *accepts* and misinterprets).
- [ ] Fragment outputs: `gl_FragColor` only; `discard` → `KILL`; the L5
      early-Z predicate stays on the CPU side and forces the shade-then-depth
      order exactly as it does now (a `KILL` shader keeps that order).
- [ ] Built-in library: the subset that maps 1:1 onto TGSI opcodes
      (`add`, `mul`, `dp3`, `dp4`, `rsq`, `sqrt`, `min`, `max`, `frc`, `flr`,
      `cnd`, `lit`-free forms of clamping, `tex2D` → `TEX`). Everything
      without a 1:1 mapping is **refused at retarget time with a diagnostic
      naming the function**, not emulated by a slower path that silently
      changes precision — the G11a rule, applied to the back end.
- [ ] The retarget's own host test suite: for each shader in a committed
      corpus, **disassemble** the emitted tokens back into text (a 150-line
      disassembler in the test) and compare against the source-normalised
      form. A back end whose only test is "the GPU drew something" cannot be
      debugged.
- [ ] The interpreter stays. A shader the back end refuses still runs on the
      AST — the honest fallback, and the thing that makes the back end safe
      to grow incrementally.

**Test gate:** token-level tests (disassembly round-trip) on the host with no
GPU; `test_glvirgl` walks every emitted stream through
`gpu_validate_cmd_stream()`; the Lambert test shader from `/glshade` compiles
to TGSI and its tokens are pinned in the test as a golden file (so a
refactor that changes the encoding without changing behaviour is a *deliberate*
diff, not a silent one).

**Definition of Done:** a GLSL shader that the interpreter executes today has
a TGSI encoding that the kernel validator accepts, and the encoding is
readable as TGSI text by an independent disassembler. **Not** "it rendered" —
that is H6's job.

**Deliverable:** `patches/GPU_H4_tgsi.patch`

**Risk, named before it happens:** the TGSI dialect virglrenderer accepts is
versioned, and this host cannot run one (Fact 1). The mitigation is
structural, not optimistic: the golden-file test pins the dialect this tree
emits, `virgl_cmd_create_shader_tgsi()`'s shape stays the single definition,
and a host with a render node runs the *same* golden file through a real
device in H6's lane. If the dialect is wrong, it is wrong loudly and in
one place.

---

### H5 — Hardware fixed-function draws

**Objective:** widen `gl_draw_batch_t` beyond position+colour so the OS's own
demos can actually take the GPU path — and keep the whole-draw fallback
intact.

**Tasks:**

- [ ] A second vertex format, versioned rather than mutating the first:
      `position (clip xyzw) + colour (rgba) + normal (xyz) + texcoord0 (st)`
      per vertex — eight floats become twelve. `gl_draw_batch_t` gains a
      `format` field beside `{data, count}`; the canned L6 path keeps
      answering as format 0, so nothing that works today changes shape.
- [ ] Lighting on the GPU for the fixed-function path is **out of scope**:
      fixed-function lighting is per-vertex and already cheap (G7 measured
      the per-vertex transform, not the shading, as the cost). The batch
      carries the **post-lighting vertex colour** exactly as it does today —
      what changes is that a textured, depth-tested draw is now eligible.
- [ ] Depth: attach a depth buffer to the FBO's `DSA` object and let the GPU
      do the depth test when `GL_DEPTH_TEST` is on and the backend supports
      it. The eligibility screen gains "depth is ours" instead of "depth is
      off".
- [ ] Blending: the `BLEND` object's fixed-setup path exists
      (`virgl_cmd_create_blend_basic`); extend to the GL factors that map
      1:1 (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`/`ONE`/`ZERO`). Anything else
      declines.
- [ ] `gl_backend_draw_eligible()` shrinks by exactly the state now
      reproduced — depth, blend, cull and texture are the four the widened
      batch reproduces, so the count goes 13 → 9 — and **each removal
      carries the test that would have failed before it**. The screen stays
      conservative in every other dimension.
- [ ] `/glcube` gains a `--gpu`-style switch (a `/tmp` flag file, the
      existing convention) that forces the hardware path and prints which
      draws fell back and why, once per frame budget.

**Test gate:** host tests for the new eligibility table (every state the batch
cannot reproduce must still decline — enumerated as a test, not as a hope);
`/gltest` asserts the widened batch is *byte-identical* between formats 0 and
1 for a position+colour draw; the integration lane with a device asserts the
log line naming the fallback reasons.

**Definition of Done:** `/glcube`'s own geometry — lit, textured,
depth-tested — can take the GPU draw path when a device is present, and the
frame-equivalence oracle of H6 says the pixels match software within its
stated tolerance. Without a device, nothing changes and the counters read
zero.

**Deliverable:** `patches/GPU_H5_draws.patch`

---

### H6 — The VirGL frame oracle

**Objective:** make "the GPU path is correct" a machine-checked statement
instead of a screenshot someone looked at.

**Tasks:**

- [ ] A deterministic scene: fixed geometry, fixed camera, fixed shader,
      fixed frame count, no clock, no RNG — the `/gltest`'s discipline raised
      to a whole frame.
- [ ] Render it twice in one boot: once through the forced software backend,
      once through VirGL. Hash both frames (the existing SHA-256 in
      `kernel/lib/sha256.c`, already used by OTA).
- [ ] A **stated tolerance model**: allowed differing-pixel fraction, with
      per-feature justification (vertex-position fixed-point rounding and
      rasterisation tie-breaking are legitimately different between
      implementations; a colour band that moves is not). The tolerance is a
      number in this document, not a constant buried in a test.
- [ ] The comparison runs on the host **and** in the guest: the host lane
      uses the same software backend compiled for `cc` (the project's
      existing technique — the same object linked into the kernel and the
      test) and a TGSI-accepting software consumer is **not** attempted;
      the guest lane is the real device.
- [ ] `test_virgl_gpu.sh` gains the oracle assertions behind
      `ENABLE_FULL_ASSERTS` and the skip path stays loud (D2).

**Test gate:** with no GPU: the scene renders, hashes, and the lane SKIPs with
the hash printed (so a later run can diff hashes across a refactor). With a
device: both hashes computed, the tolerance asserted, the differing-pixel
count printed whether it passes or fails.

**Definition of Done:** a regression in the GPU path — a wrong vertex format,
a wrong uniform slot, a wrong sampler — fails a committed test rather than
"looking slightly off on the cube". This is the phase that makes H3–H5
maintainable rather than merely landed.

**Deliverable:** `patches/GPU_H6_oracle.patch`

---

### H7 — CI lane, docs, close-out

**Objective:** the plan leaves the tree honest and the next reader informed.

**Tasks:**

- [ ] `docs/opengl.md`'s Not-implemented row for hardware drawing moves to
      Implemented **exactly as far as H5 reached**, with the eligibility list
      spelled out. If H5 declined to do lighting on the GPU, the row says so.
- [ ] `docs/status.md`: the OpenGL, virtio-gpu 2D and `SYS_GPU_CALL` cells
      carry this series' numbers and thresholds; the 🧪 glyphs stay until a
      gate has seen a real device.
- [ ] Residue ledger: RES-54 flips to DONE@H4/H5 with the phase name; any
      newly-found debt (the cursor queue if H2 opened it, the TGSI dialect if
      H4 found a version split) becomes a measured row.
- [ ] `CHANGELOG.md` entry per phase, in the existing style.
- [ ] `docs/GPU_ACCELERATION.md` — the architecture and *behaviour worth
      knowing* doc, in `opengl.md`'s shape: what moves to the GPU, what does
      not and why, what a reader must not assume (e.g. "the front
      framebuffer is not written when scanout owns the display").
- [ ] The checker's pins move to the post-series truth; `--selftest` still
      has teeth.

**Test gate:** `test_gpu_claims.sh` green; `make test-unit` EXIT 0;
`test_virgl_gpu.sh` green with and without a device.

**Definition of Done:** the leftover table in `docs/opengl.md` is the
leftover table in §4 of this document; RES-54 is not still "the missing
piece" once the back end draws a triangle.

**Deliverable:** `patches/GPU_H7_closeout.patch`

---

## 3a. Recommended order, and the risk table

Dependency order is §3's; **recommended execution order** is the same list,
for one reason stated plainly: H1 may decide that H2 is not worth doing on
this host, and that decision costs one phase instead of five. The 3D work
(H3–H5) is real work with real value on a render-node host, but no phase of
it can be validated here (Fact 1), so it ships behind a loud skip and a
golden file, and a host with a device validates it later.

| Order | Phase | Why here | Blocker if skipped |
|---:|---|---|---|
| 1 | H0 | the rig | every later number is unverifiable |
| 2 | H1 | measures before deciding | H2 would land blind |
| 3 | H2 | the only GPU path CI can run | the desktop gains nothing |
| 4 | H3 | unlocks a textured hardware draw | H5's texture refusal stays |
| 5 | H4 | unlocks any shader on the GPU | RES-54 stays open |
| 6 | H5 | the demos' own geometry | eligibility stays at 13 refusals |
| 7 | H6 | makes H3–H5 maintainable | GPU regressions become "looks off" |
| 8 | H7 | honesty sweep | docs drift |

### Risks, and the mitigation that is structural rather than hopeful

| Risk | Where it bites | Mitigation (in the plan, not in a wish) |
|---|---|---|
| No host GL context on the reference machine | H3, H4, H5, H6 cannot run their happy path here | D2: loud SKIP + host-side golden files; the `test_virgl_gpu.sh` shape already exists |
| TGSI dialect differs by virglrenderer version | H4's encoding | H4's golden file pins the dialect; the kernel validator is the first gate; H6 runs it on a real device |
| H1 finds the 2D path a pessimisation | H2's premise | §5 is allowed to say so; H2 then ships as *opt-in + threshold*, and that is a pass (D1) |
| The front-fb elision changes a readback | H2 | `front_fb` has no readers outside `graphics.c` (verified) and the fb-console path is opt-in; the pixel-identity gate is the arbiter |
| H5's eligibility table grows by accident | every later scene | each removal needs the test that would have failed before it; the table is enumerable and counted (13 today) |
| A tolerance model chosen for convenience | H6 | D7: per-feature justification, published in §5, and the differing-pixel count is printed on pass as well as fail |
| Scope creep into "port Mesa" | all phases | §4 names it |

---

## 4. What this plan deliberately does not do

Named so nobody mistakes absence for oversight.

- **A JIT for the GLSL interpreter.** D4. It moves the same per-pixel work
  into machine code; a GPU draw removes it.
- **A Gallium state tracker / Mesa port.** The tree implements the slice of
  GL it needs; adopting a state tracker would replace `libgl` with a
  dependency and every host test with a build system.
- **Vulkan, compute shaders, `LAUNCH_GRID`.** `VIRGL_CCMD_LAUNCH_GRID` is a
  constant in `virgl.h` and nothing else; compute is a different machine and
  none of the OS's own programs want it.
- **Multiple devGL contexts / multi-GPU.** One context, one render target,
  checked in `aglx_bind()`. A second GL context still falls back to software,
  as GL2 recorded.
- **Hardware compositing of *windows* (the compositor blitting window
  surfaces on the GPU).** H2 fixes the present; moving `render.rs`'s inner
  loops to the GPU would need every widget's draw as GPU work, which is a
  compositor rewrite, not an acceleration. Named here so the question does
  not reopen in every phase review.
- **The cursor queue**, unless H1's measurement demands it (Fact 5).
- **Real-hardware vendors** (Intel/AMD/NVIDIA). The project's GPU story is
  virtio-gpu; a bare-metal GPU driver is a different project with a different
  plan and a hardware support matrix.
- **New syscalls.** D8.
- **Re-opening G0–G13 / L0–L7.** Complete means complete. Bugs found while
  landing H* are fixed in the phase that found them.

---

## 5. Terminal arithmetic (baseline filled by H0, columns by each phase)

Baseline column is H0's job; later columns land with their phase. **Numbers
below marked † are measured; the rest are blanks the phases must fill.**

| Metric | Baseline (`366825f`) | H1 | H2 | H3 | H4 | H5 | H6 |
|---|---|---|---|---|---|---|---|
| GPU counters in `perfstat.h` | **0** † | **8** ✅ | 8 | 10 | 10 | 12 | 12 |
| Bytes copied per presented rect, measured @1024×768 (15 presents) | **6 094 848** † | **12 189 696 (2.000×)** ✅ | target 6 094 848 | 6 094 848 | 6 094 848 | 6 094 848 | 6 094 848 |
| `gpu_transfer_bytes` per steady-state frame | not counted † | **= the flipped bytes, to the byte** ✅ | 1 screen | texture-dirty only | unchanged | unchanged | unchanged |
| Control-queue round trips per present | not counted † | **2.13** (timeouts 0) ✅ | 2.13 | 2.13 | 2.13 | 2.13 | 2.13 |
| Canned TGSI dwords in `glvirgl.c` | **34** † (hand-written) | 34 | 34 | 34 | **0** (generated) | 0 | 0 |
| GLSL shaders expressible as TGSI | **0** † | 0 | 0 | 0 | corpus | corpus | corpus |
| `gl_draw_batch_t` floats | **8** † | 8 | 8 | 8 | 8 | **12** | 12 |
| Eligibility refusals in `gl_backend_draw_eligible()` | **13** † | 13 | 13 | 13 | 13 | **≤9** (measured) | ≤9 |
| `sizeof(aglx_context)` | **240 328** † | 240 328 | 240 328 | re-measured | re-measured | re-measured | re-measured |
| `test_glvirgl` checks | **73** † | 73 | 73 | ≥90 | ≥110 | ≥130 | ≥130 |
| `test_gpu_syscall` checks | **18** † | 18 | 18 | 18 | 18 | 18 | 18 |
| `/gltest` in-OS checks | 427 *(pre-series; re-run owed — H0 left it open)* | 427 | 427 | ≥435 | ≥460 | ≥480 | ≥490 |
| Plan-claims checker pins | **0** (no checker existed) | 25 † | 25 | 25 | 25 | 25 | 25 |
| Boot ticks to shell (virtio-gpu, fast) | **175** † | 175 | measured | measured | measured | measured | measured |

H1 filled its column with measurements, and the numbers **rewrote two
rows of this table** — which is the point of the phase:

- 「Bytes copied」was stated at 1280×800 (4 096 000 B/frame). Measured on
  the only lane where both paths are alive, the tree runs at **1024×768**
  (the VBE mode Stage 2 selects), so the row above is the honest one:
  **6 094 848 B copied without the GPU, 12 189 696 B with it, ratio exactly
  2.000**. The 1280×800 figure survives as the *scanout* geometry, not the
  frame geometry.
- H1 was expected to find the 2D path a pessimisation *above some N
  pixels*. It found a flat **2.000× on every rect size measured**, because
  the cost is a second full copy of the *same* rectangle, not a fixed
  per-present overhead. There is no threshold to tune: H2 must **elide**
  the copy or resign.

Two rows in that table were **measured today, not estimated**, and both
replace a number that had been carried from prose:

- the eligibility screen has 15 `return 0;` statements of which **13** are
  state refusals (the other two are preconditions) — the plan's 13 was right,
  and the checker now pins the 15/13 split so a later phase cannot make the
  two agree by accident;
- `canned_vs_tgsi[21] + canned_fs_tgsi[13]` = **34 dwords**, and no
  `glsl_tgsi.c` exists anywhere in `libgl` — H4's target is an absence the
  checker can see.

Residue expected to open or close:

| Item | Class | Notes |
|---|---|---|
| RES-54 (GLSL AST → TGSI) | S → closed | H4/H5 |
| Cursor queue (queue 1) | N or dropped | H2, only on a measured need |
| TGSI dialect pin (virglrenderer version) | N | H4's golden file is the opener |
| 2D-path pessimisation above N pixels | N | H1's decision table; stays opt-in if measured so |

---

## 6. Mandatory engineering principles

The project's existing culture, restated so this series does not stand out.

1. **Host-side unit tests are mandatory and link the shipping file.** The
   backend seam, the batch encodings and the TGSI emitter are compiled with
   `cc` against the same sources the kernel uses; the kernel validator
   (`gpu_cmdcheck.c`) is the arbiter for every stream.
2. **Warning-free builds** under `-Wall -Wextra -Werror`, kernel and user.
3. **No user-space-triggered GPU access.** Every new stream path is walked by
   the existing validator before it reaches a device; a negative test for each
   new packet shape is part of the phase, not an afterthought.
4. **Honest `docs/status.md` and `docs/opengl.md`.** Nothing that is a stub
   may be marked as implemented; a SKIP is printed, never silent.
5. **`CHANGELOG.md` for every phase.**
6. **A missing device is a loud SKIP**, and a declined backend never reports
   `GL_RENDERER` hardware.
7. **Specification conformance outranks speed**, and *pixels* outrank both:
   H6's tolerance is the definition of correct.
8. **Every phase ships a `.patch` file** in `patches/`, generated from the
   phase's complete diff.

---

## 7. Effort estimate

| Phase | Lines of code (estimate) | Main risk |
|---|---:|---|
| H0 | ~350 | **landed: 23 pins, 3 after the first run** (see §8) |
| H1 | ~500 | the measurement itself: a rig that measures itself is the OPT Fact 5 trap |
| H2 | ~700 | the front-fb elision changing what a readback sees |
| H3 | ~900 | dirty-level bookkeeping, `last_level` semantics |
| H4 | **~2 500** | the TGSI dialect; inlining user functions correctly |
| H5 | ~1 200 | the eligibility table growing faster than the pipeline |
| H6 | ~600 | a tolerance model that is honest rather than convenient |
| H7 | ~400 | — |
| **Total** | **~7 150** | |

For comparison: `libgl/` is ~8 000 lines and the entire `drivers/` tree is
~15 500, so this series is roughly one libgl in size, with H4 alone a third
of it — which is why H4 is stated in terms of a token emitter with a golden
file rather than "the rest of OpenGL".

---

## 8. Progress log

| Phase | Status | Patch |
|---|---|---|
| H0 | ✅ complete — 25 pins, `make test-unit` EXIT 0 (273 runs) | `tools/check_gpu_claims.py` + `tests/unit/test_gpu_claims.sh` |
| H1 | ✅ complete 2026-11-21 — **2.000× pessimisation measured**, 8 counters, `/proc/fb`, `gpubench`, host test 18/18 | `patches/GPU_H1_present.patch` |
| H2 | ⬜ not started | — |
| H3 | ⬜ not started | — |
| H4 | ⬜ not started | — |
| H5 | ⬜ not started | — |
| H6 | ⬜ not started | — |
| H7 | ⬜ not started | — |

---

## Workflow (mandatory for every phase)

The `GL_PLAN.md` / `OPT_PLAN.md` loop, unchanged:

```
1. READ    — read ALL affected files before writing anything
2. PLAN    — this file: Status → IN PROGRESS on the phase
3. DESIGN  — show struct/API changes, list callers; cite a D-number
             instead of re-arguing
4. IMPL    — backend → host test → /gltest → demo → docs
             compile after every file; fix warnings immediately
5. BUILD   — make clean && make all  (zero warnings, -Wall -Wextra -Werror)
6. TEST    — host gate; QEMU gate (with AND without a device); existing
             suites unmodified (D3 of GL2, restated here)
7. DOCS    — this file's Result + §5 ticks, CHANGELOG.md, docs/opengl.md
             Not-implemented row, docs/status.md cells, ledger rows, and
             the checker pin — all in the SAME commit (D6)
```
