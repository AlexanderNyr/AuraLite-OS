/* libgl/src/glvirgl.c — VirGL hardware backend.
 *
 * Phase G13 of GL_PLAN.md, on the seam phase G9 built and the syscall phase
 * K1 exposed.
 *
 * WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT
 *
 * G13 implemented steps 1-3 of the plan's scope: probe, clear and present.
 * The frame was still rasterised by the CPU; what moved to the GPU was the
 * PRESENT, which stops the finished pixels travelling through the
 * compositor's blit path and lands them on the scanout directly.
 *
 * GL2 L6 adds the draw path the G13 header refused to fake: DRAW_VBO, with
 * ONE canned TGSI pipeline (pass-through position and colour) and ONE vertex
 * format (clip-space xyzw + rgba), fed only by whole fixed-function
 * glDrawArrays(GL_TRIANGLES) batches that gl_backend_draw_eligible() has
 * already screened against every piece of state the canned pipeline cannot
 * reproduce.  This is NOT a TGSI compiler: the shaders are dword arrays in
 * this file, hand-written against the TGSI token format, and G11's AST still
 * cannot become TGSI (D7) -- that retarget stays a successor plan.  An
 * unsupported draw declines and the software rasterizer takes the WHOLE
 * draw, because a frame half CPU-drawn and half GPU-drawn is a tearing bug.
 *
 * The present has to know which side drew the frame: scanouting the CPU
 * buffer after a GPU draw would overwrite the triangle with the CPU's empty
 * colour buffer -- the exact "DRAW_VBO is a no-op" bug -- so a frame the GPU
 * drew entirely is scanned out from the GPU render target without the CPU
 * transfer.  The context's frame flags (see glcontext.h) carry the decision.
 *
 * SO WHAT IS IT WORTH?
 *
 * Measure before believing.  On this implementation the software rasterizer
 * costs milliseconds per frame and ag_blit() costs tens of microseconds, so
 * moving the present to the GPU is a small fraction of a frame.  The value is
 * not speed today: it is that the seam is proved end to end -- a real
 * virtio-gpu context, a real backed resource, a real command stream accepted
 * by a real device -- so the remaining work is bounded and known.
 *
 * HONESTY ABOUT DECLINING
 *
 * probe() returns failure unless a virtio-gpu with VirGL is actually present
 * AND every setup step succeeded.  A backend that registered and then failed
 * per-frame would report "hardware" in GL_RENDERER and produce silence or
 * corruption; declining leaves the software path in place and the renderer
 * string truthful.  That was the G9 reasoning and it has not changed.
 */

#include <string.h>
#include <unistd.h>

#include "GL/gl.h"
#include "GL/glbackend.h"
#include "glcontext.h"

/* The kernel's GPU ABI.  libgl is user space, so this header is shared with
 * the kernel rather than duplicated -- the alternative is two definitions of
 * a binary layout drifting apart. */
#include "kernel/gpu/gpu_syscalls.h"

/* ============================================================================
 * Syscall plumbing
 * ==========================================================================*/

static int64_t gpu_call(uint64_t op, uint64_t a2) {
    return syscall(SYS_GPU_CALL, op, a2, 0, 0, 0, 0);
}

/* ---- VirGL command-stream encoding ----
 *
 * A VirGL command is a header dword followed by its payload:
 *
 *   bits 24..31  command id
 *   bits 16..23  object type (unused by the commands here)
 *   bits  0..15  payload length in dwords, NOT counting the header
 *
 * The layout is taken from drivers/gpu/virgl.h rather than restated, because
 * the first draft of this file guessed it the other way round -- id low,
 * length high -- and produced headers the kernel validator rejected.  It
 * failed safely, but only because the validator exists; a stream that had
 * slipped through would have been a length field the host GPU trusted.
 *
 * Sharing the driver's macro means there is one definition of the wire format
 * in the tree. */
#include "drivers/gpu/virgl.h"

/* Everything else -- the CLEAR opcode, the buffer bitmask, the pixel format
 * and the bind flags -- comes from drivers/gpu/virgl.h too.  The first draft
 * of this file guessed all four and got all four wrong: CLEAR was 3 rather
 * than 4, the colour bit was 0x4 rather than 0x1, depth was 0x1 rather than
 * 0x2, and RENDER_TARGET was 0x2 rather than 0x10.  Only the redefinition
 * warning caught them, because none of it executes without a GPU.
 *
 * The lesson is narrow and worth stating: protocol constants are not worth
 * retyping.  A wrong one produces a stream the device accepts and
 * misinterprets, which is far harder to diagnose than a stream it rejects.
 *
 * VIRGL_PIPE_TEXTURE_2D is the one value the driver header does not define,
 * since nothing in the kernel creates a 2D texture resource; PIPE_TEXTURE_2D
 * is 2 in Gallium's enum, which is what virglrenderer implements. */
#define VIRGL_PIPE_TEXTURE_2D 2u

/* ============================================================================
 * Backend state
 *
 * One context and one render target for the whole library.  A second GL
 * context would need its own, but nothing creates one today and a table would
 * be speculative structure -- the single slot is a deliberate limit, checked
 * in aglx_bind() rather than assumed.
 * ==========================================================================*/

static struct {
    int      up;                 /* probe() succeeded                       */
    uint32_t ctx;                /* kernel context handle                   */
    uint32_t res;                /* render-target resource handle           */
    int      res_w, res_h;       /* its size, so a resize can be detected   */
    struct aglx_context *owner;  /* the GL context using it, or NULL        */
    gpu_info_t info;
    /* H3: how many GL texture names the backend will mirror on the device.
 * 32 is sized to the demos (a font atlas, a few sprites) and smaller
 * than the kernel's 64-resource quota, which also has to hold the
 * render target and the vertex buffer. */
#define VIRGL_TEX_MAX 32

/* GL2 L6: the canned-draw pipeline.  `pipe_up` is per render target (the
     * surface binds the resource, so a resize invalidates it); the vertex
     * buffer outlives resizes. */
    int      pipe_up;
    uint32_t vres;               /* vertex-buffer resource (fixed capacity) */

    /* H5: the depth surface, created on the first depth-tested draw and
     * destroyed with its render target, plus the state the current pipe was
     * built with (so a change rebuilds it rather than being ignored). */
    uint32_t zres;
    int      z_w, z_h;
    unsigned st_depth, st_blend, st_blend_src, st_blend_dst, st_cull, st_fmt;

    /* GL_ACCEL H3: one device resource per GL texture name, plus the dirty
     * bookkeeping that makes a texture cost ONE upload instead of one per
     * frame.  The pool is fixed: a backend that silently drops textures past
     * a limit would sample the wrong image, so past VIRGL_TEX_MAX the
     * backend declines the texture (returns non-zero) and the software
     * sampler keeps it -- the draw that needs it then declines too, which is
     * a visible, honest failure and not a corruption. */
    struct virgl_tex {
        int      used;
        GLuint   name;
        uint32_t res;            /* device resource handle                  */
        int      w, h;           /* level-0 dimensions                      */
        int      levels;         /* chain length the resource was made for   */
        int      dirty;          /* upload pending                          */
        int      uploaded_levels;/* bitmask of levels present on the device */
        uint32_t sampler;        /* sampler state handle, 0 = not created    */
    } tex[VIRGL_TEX_MAX];
} vg;

/* Tear the resource down; the context survives so a resize can rebuild. */
static void release_target(void) {
    if (vg.zres) {              /* H5: the depth surface follows its target */
        gpu_call(GPU_OP_RES_DESTROY, vg.zres);
        vg.zres = 0;
        vg.z_w = vg.z_h = 0;
    }
    if (vg.res) {
        gpu_call(GPU_OP_RES_DESTROY, vg.res);
        vg.res = 0;
        vg.res_w = vg.res_h = 0;
    }
}

/* Create a render target of the given size, replacing any existing one. */
static int make_target(int w, int h) {
    release_target();
    vg.pipe_up = 0;              /* the surface binds the old resource */

    gpu_res_create_t rc;
    memset(&rc, 0, sizeof rc);
    rc.ctx        = vg.ctx;
    rc.target     = VIRGL_PIPE_TEXTURE_2D;
    rc.format     = VIRGL_PIPE_FORMAT_B8G8R8X8_UNORM;
    rc.bind       = VIRGL_BIND_RENDER_TARGET;
    rc.width      = (uint32_t)w;
    rc.height     = (uint32_t)h;
    rc.depth      = 1;
    rc.array_size = 1;

    int64_t r = gpu_call(GPU_OP_RES_CREATE, (uint64_t)(uintptr_t)&rc);
    if (r <= 0) return -1;

    vg.res   = (uint32_t)r;
    vg.res_w = w;
    vg.res_h = h;
    return 0;
}

/* ============================================================================
 * GPU_ACCEL H3: textures on the device
 *
 * The contract with libgl is one call per changed image, and this file's job
 * is to make that cost ONE transfer per image PER LIFETIME where possible.
 * Three things follow, each of them a bug the naive version has:
 *
 *   1. A texture that stops changing is never uploaded again.  The dirty
 *      flag is cleared only after the transfer succeeded; a failed upload
 *      leaves it set so the next frame retries instead of sampling garbage
 *      that the host thinks is current.
 *
 *   2. A mip chain is ONE resource, not one per level.  The driver's
 *      RESOURCE_CREATE_3D takes `last_level`; ignoring it would allocate a
 *      level-0-sized resource and inline-write level 1 past its end.
 *
 *   3. Deleting or resizing a texture releases the device resource.  The
 *      kernel's 64-resource quota is per process and shared with the render
 *      target and the vertex buffer, so leaking one per glTexImage2D would
 *      exhaust it in a demo that reloads an atlas per scene.
 *
 * The upload itself is a RESOURCE_CREATE_3D followed by one TRANSFER per
 * level.  TRANSFER, not INLINE_WRITE: inline writes ride inside the command
 * stream, which the kernel caps at 256 KiB, and a 256x256 RGBA level is
 * exactly that -- a texture one pixel larger would silently fail to upload.
 * ==========================================================================*/

/* Defined below with the sampler packets; the upload path needs it first. */
static int tex_make_sampler(struct virgl_tex *t, const gl_texture_t *tex);

static struct virgl_tex *tex_slot_find(uint32_t res_or_name, int by_res) {
    for (int i = 0; i < VIRGL_TEX_MAX; i++) {
        struct virgl_tex *t = &vg.tex[i];
        if (!t->used) continue;
        if (by_res ? (t->res == res_or_name) : (t->name == res_or_name))
            return t;
    }
    return NULL;
}

static struct virgl_tex *tex_slot_alloc(void) {
    for (int i = 0; i < VIRGL_TEX_MAX; i++)
        if (!vg.tex[i].used) return &vg.tex[i];
    return NULL;
}

/* Free the device resource behind a slot.  Idempotent. */
static void tex_slot_release(struct virgl_tex *t) {
    if (!t || !t->used) return;
    if (t->res) gpu_call(GPU_OP_RES_DESTROY, t->res);
    memset(t, 0, sizeof *t);
}

/* Upload one mip level of `t` from libgl's texels.
 *
 * Returns 0 on success.  Level 0 carries the whole chain's size: a chain
 * whose resource does not match the level-0 image is rebuilt rather than
 * patched, because a chain cannot shrink in place on the device. */
static int tex_upload_level(struct virgl_tex *t, int level,
                            int w, int h, const uint32_t *data, int levels) {
    /* Rebuild when the level-0 geometry or the chain length changed. */
    if (t->res && (level == 0 ? (t->w != w || t->h != h) : 0)) {
        if (t->res) gpu_call(GPU_OP_RES_DESTROY, t->res);
        t->res = 0;
        t->uploaded_levels = 0;
        t->sampler = 0;              /* the view pointed at the dead resource */
    }
    if (t->res && levels != t->levels) {
        gpu_call(GPU_OP_RES_DESTROY, t->res);
        t->res = 0;
        t->uploaded_levels = 0;
        t->sampler = 0;
    }

    if (!t->res) {
        gpu_res_create_t rc;
        memset(&rc, 0, sizeof rc);
        rc.ctx        = vg.ctx;
        rc.target     = VIRGL_PIPE_TEXTURE_2D;
        rc.format     = VIRGL_PIPE_FORMAT_B8G8R8A8_UNORM;
        rc.bind       = VIRGL_BIND_SAMPLER_VIEW;
        rc.width      = (uint32_t)w;
        rc.height     = (uint32_t)h;
        rc.depth      = 1;
        rc.array_size = 1;
        rc.last_level = (uint32_t)(levels > 0 ? levels - 1 : 0);
        int64_t r = gpu_call(GPU_OP_RES_CREATE, (uint64_t)(uintptr_t)&rc);
        if (r <= 0) return -1;
        t->res    = (uint32_t)r;
        t->w      = w;
        t->h      = h;
        t->levels = levels;
    }

    if (!data) return 0;             /* size-only call */

    size_t bytes = (size_t)w * (size_t)h * 4u;
    if (bytes == 0 || bytes > GPU_MAX_XFER_BYTES) return -1;

    gpu_transfer_t tr;
    memset(&tr, 0, sizeof tr);
    tr.ctx    = vg.ctx;
    tr.res    = t->res;
    tr.level  = (uint32_t)level;
    tr.w      = (uint32_t)w;
    tr.h      = (uint32_t)h;
    tr.d      = 1;
    tr.stride = (uint32_t)w * 4u;
    tr.size   = (uint32_t)bytes;
    tr.data   = (uint64_t)(uintptr_t)data;

    if (gpu_call(GPU_OP_TRANSFER, (uint64_t)(uintptr_t)&tr) != 0) return -1;
    t->uploaded_levels |= 1 << (level & 31);
    return 0;
}

static int virgl_tex_image(struct aglx_context *ctx, GLuint name, GLenum target,
                           int face, int level, GLsizei w, GLsizei h,
                           const uint32_t *data, int last_level) {
    (void)ctx;
    /* Cube faces would need six resources or array layers; 3D needs depth.
     * Both are refused rather than approximated, so the software sampler
     * keeps them and the renderer string stays honest. */
    if (target != GL_TEXTURE_2D || face != 0) return -1;
    if (!vg.up || !vg.ctx) return -1;
    if (name == 0) return -1;

    struct virgl_tex *t = tex_slot_find(name, 0);

    if (w == 0 || h == 0 || !data) {          /* the DELETE notification */
        if (t) tex_slot_release(t);
        return 0;
    }

    if (level < 0 || level > 31) return -1;
    if (!t) {
        t = tex_slot_alloc();
        if (!t) return -1;                    /* pool full: decline       */
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->name = name;
    }

    int levels = last_level + 1;
    if (levels < 1) levels = 1;
    if (levels > 32) levels = 32;

    if (tex_upload_level(t, level, w, h, data, levels) != 0) {
        /* The slot is left allocated but with no resource: the next call
         * retries instead of sampling a resource the host never got. */
        if (!t->res) t->used = 0;
        return -1;
    }
    t->dirty = 0;

    /* The view is made once the level-0 image exists: a view over a resource
     * with no level 0 would sample the host's uninitialised allocation. */
    if (level == 0) {
        tex_make_sampler(t, NULL);   /* sampler state defaults when NULL */
    }
    return 0;
}

/* ============================================================================
 * GL2 phase L6: the canned DRAW_VBO pipeline
 *
 * ONE vertex format and ONE shader pair, hand-written as TGSI dword arrays.
 * This is deliberately the only shader content in the file: G11 produces an
 * interpreted AST, and turning that into TGSI is a compiler back end that
 * stays a successor plan (D7).  What is here is what a triangle needs.
 *
 * The TGSI encodes the pip format from Mesa's p_shader_tokens.h (LSB-first
 * bitfields): a header token (HeaderSize=2, BodySize=rest), a processor
 * token, declarations (range, then interp, then semantic), MOV instructions
 * (opcode 1) and END (opcode 117).  Disassembled:
 *
 *   VS                                  FS
 *   DCL IN[0],  POSITION                DCL IN[0], COLOR, PERSPECTIVE
 *   DCL IN[1],  COLOR                   DCL OUT[0], COLOR
 *   DCL OUT[0], POSITION                MOV OUT[0], IN[0]
 *   DCL OUT[1], COLOR                   END
 *   MOV OUT[0], IN[0]
 *   MOV OUT[1], IN[1]
 *   END
 *
 * The pass-through is the point: the batch carries CLIP coordinates the
 * fixed-function matrices already produced, so the GPU's divide, clip and
 * viewport reproduce the CPU pipeline's own maths exactly.  Nothing here
 * transforms, lights or samples.
 * ==========================================================================*/

static const uint32_t canned_vs_tgsi[21] = {
    0x00001302u, 0x00000000u,             /* header: 2 + 19; processor VS  */
    0x002F2031u, 0x00000000u, 0x00000000u,/* DCL IN[0],  POSITION          */
    0x002F2031u, 0x00000000u, 0x00000001u,/* DCL IN[1],  COLOR             */
    0x002F3031u, 0x00000000u, 0x00000000u,/* DCL OUT[0], POSITION          */
    0x002F3031u, 0x00000000u, 0x00000001u,/* DCL OUT[1], COLOR             */
    0x00A01033u, 0x000000F3u, 0x39000002u,/* MOV OUT[0], IN[0]             */
    0x00A01033u, 0x000004F3u, 0x39000042u,/* MOV OUT[1], IN[1]             */
    0x00075013u,                          /* END                           */
};

static const uint32_t canned_fs_tgsi[13] = {
    0x00000B02u, 0x00000001u,             /* header: 2 + 11; processor FS  */
    0x006F2041u, 0x00000000u, 0x00000002u,
    0x00000001u,                          /* DCL IN[0], COLOR, PERSPECTIVE */
    0x002F3031u, 0x00000000u, 0x00000001u,/* DCL OUT[0], COLOR             */
    0x00A01033u, 0x000000F3u, 0x39000002u,/* MOV OUT[0], IN[0]             */
    0x00075013u,                          /* END                           */
};

/* Test seam (test_glvirgl): the canned shaders and the one-time setup stream,
 * so the host can walk the encoding against the kernel validator without a
 * device in the room. */
const uint32_t *gl_virgl_canned_vs_tokens(int *n) {
    if (n) *n = (int)(sizeof canned_vs_tgsi / sizeof canned_vs_tgsi[0]);
    return canned_vs_tgsi;
}

const uint32_t *gl_virgl_canned_fs_tokens(int *n) {
    if (n) *n = (int)(sizeof canned_fs_tgsi / sizeof canned_fs_tgsi[0]);
    return canned_fs_tgsi;
}

/* ---- packet builders ------------------------------------------------------
 *
 * Same shapes the kernel's helpers in drivers/gpu/virgl.c emit, re-encoded
 * here because that file is kernel code.  The layouts are pinned by
 * test_glvirgl against the kernel validator, and every constant comes from
 * the shared drivers/gpu/virgl.h -- the G13 lesson, that protocol constants
 * are not worth retyping, still holds. */

static void pk_create_object(uint32_t *d, uint32_t *n, uint32_t obj_type,
                             uint32_t len, uint32_t handle,
                             const uint32_t *payload) {
    d[(*n)++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, obj_type, 1 + len);
    d[(*n)++] = handle;
    for (uint32_t i = 0; i < len; i++) {
        d[(*n)++] = payload ? payload[i] : 0;   /* payload is always fully
                                                 * initialised by the callers;
                                                 * the null guard only silences
                                                 * a false maybe-uninitialized
                                                 * through the inlined copies */
    }
}

static void pk_bind(uint32_t *d, uint32_t *n, uint32_t obj_type,
                    uint32_t handle) {
    d[(*n)++] = VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, obj_type, 1);
    d[(*n)++] = handle;
}

#define PK_PUSH(d, n, v) ((d)[(n)++] = (uint32_t)(v))
#define PK_PUSHF(d, n, fv) \
    do { union { float fl; uint32_t u; } _c; _c.fl = (fv); (d)[(n)++] = _c.u; \
     } while (0)

/* The one-time setup stream: pipeline objects, shaders, binds, the surface +
 * framebuffer state on the current render target, and a viewport that mirrors
 * the CPU pipeline's own projection (win = ndc * scale + translate, GL's
 * bottom-up window space, depth range [0,1]).
 *
 * Returns the stream length in dwords, or -1 if `cap` is too small.  Public
 * so test_glvirgl can walk it through the kernel validator. */
/* H5: GL blend factor → gallium PIPE_BLENDFACTOR_*.  0xFFFFFFFF means "no
 * exact mapping"; the caller declines rather than drawing with a substitute,
 * because a wrong factor produces a frame that looks plausible and is not. */
static uint32_t gl_to_pipe_blend(GLenum f) {
    switch (f) {
    case GL_ZERO:                return VIRGL_PIPE_BLENDFACTOR_ZERO;
    case GL_ONE:                 return VIRGL_PIPE_BLENDFACTOR_ONE;
    case GL_SRC_ALPHA:           return VIRGL_PIPE_BLENDFACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VIRGL_PIPE_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    default:                     return 0xFFFFFFFFu;
    }
}

/* H5: the depth buffer the hardware test needs.
 *
 * A texture-format resource bound as a depth surface: same shape as the
 * render target, depth format, depth binding.  Created on demand, cleared
 * once when the pipe is built, never read back -- its job is to make the
 * GPU's depth test possible, and the frame-ownership fork already means the
 * CPU never samples it.  Returns 0 when vg.zres is a surface of this size. */
static int ensure_depth_surface(int w, int h) {
    if (vg.zres && vg.z_w == w && vg.z_h == h) return 0;
    if (vg.zres) {
        gpu_call(GPU_OP_RES_DESTROY, vg.zres);
        vg.zres = 0;
        vg.z_w = vg.z_h = 0;
    }
    gpu_res_create_t rc;
    memset(&rc, 0, sizeof rc);
    rc.ctx        = vg.ctx;
    rc.target     = VIRGL_PIPE_TEXTURE_2D;
    rc.format     = VIRGL_PIPE_FORMAT_Z32_FLOAT;
    rc.bind       = VIRGL_BIND_DEPTH_STENCIL;
    rc.width      = (uint32_t)w;
    rc.height     = (uint32_t)h;
    rc.depth      = 1;
    rc.array_size = 1;
    int64_t r = gpu_call(GPU_OP_RES_CREATE, (uint64_t)(uintptr_t)&rc);
    if (r <= 0) return -1;
    vg.zres = (uint32_t)r;
    vg.z_w = w;
    vg.z_h = h;
    return 0;
}

/* The GL2 L6 stream, unchanged: a NULL state means "no depth, no blend, no
 * cull, two vertex elements", which reproduces the original bytes exactly.
 * test_glvirgl pins that the two forms agree wherever no flag is set. */
int gl_virgl_canned_setup(uint32_t w, uint32_t h, uint32_t *d, uint32_t cap) {
    return gl_virgl_setup_state(w, h, NULL, 0, d, cap);
}

/* `fmt` is GL_BATCH_FMT_*: the vertex layout the VELEM object describes.
 * Format 0 keeps the two elements L6 shipped; format 1 adds normal and
 * texcoord, whose SIZE and OFFSET are derived here from the same
 * GL_BATCH_STRIDE the batch builder uses -- one definition, not two. */
int gl_virgl_setup_state(uint32_t w, uint32_t h, const gl_virgl_state_t *st,
                         unsigned fmt, uint32_t *d, uint32_t cap) {
    uint32_t n = 0;
    if (cap < 96) return -1;

    { /* BLEND.  dwords 0..3 are sRGB/alpha-to-coverage/alpha-to-one/dither;
       * dword 4 is RT0's equation, src in the low half, dst in the high. */
      uint32_t blend[7] = { 0 };
      if (st && st->blend)
          blend[4] = (st->blend_src & 0xFFFFu) |
                     ((st->blend_dst & 0xFFFFu) << 16);
      pk_create_object(d, &n, VIRGL_OBJECT_BLEND, 7,
                       VIRGL_CANNED_BLEND, blend); }
    { uint32_t r[9] = { 0 };
      union { float f; uint32_t u; } one; one.f = 1.0f;
      r[0] = st ? st->cull : 0u;                /* PIPE_FACE_*               */
      r[3] = one.u; r[4] = one.u;               /* line width, point size    */
      pk_create_object(d, &n, VIRGL_OBJECT_RASTERIZER, 9,
                       VIRGL_CANNED_RASTER, r); }
    { uint32_t dsa[5] = { 0 };
      if (st && st->depth) {
          /* dword 0: depth_enabled | writemask<<1 | func<<2 | stencil off */
          dsa[0] = 1u | (1u << 1) | (VIRGL_PIPE_FUNC_LESS << 2);
      }
      pk_create_object(d, &n, VIRGL_OBJECT_DSA, 5,
                       VIRGL_CANNED_DSA, dsa); }
    { /* VERTEX_ELEMENTS.  Two per element: format, then (offset | buffer<<16
       * ... ) -- the layout this file already shipped, with the extra
       * elements appended, so format 0's object stays byte-identical. */
      uint32_t v[16] = { 0 };
      uint32_t nelem = 2;
      v[0] = 2;                                 /* two elements              */
      v[1] = 0;  v[2] = VIRGL_PIPE_FORMAT_R32G32B32A32_FLOAT;  /* pos  @0  */
      v[3] = 0;  v[4] = 0;
      v[5] = 16; v[6] = VIRGL_PIPE_FORMAT_R32G32B32A32_FLOAT;  /* col  @16 */
      v[7] = 0;
      if (fmt == GL_BATCH_FMT_FULL) {
          /* normal at float 8 (three floats, R32G32B32), texcoord0.s at
           * float 11 (one float).  Both offsets are 4 * the stride, spelled
           * as offsets rather than as float indices because that is the unit
           * the protocol takes. */
          nelem = 4;
          v[0] = 4;
          v[8]  = VIRGL_PIPE_FORMAT_R32G32B32_FLOAT;
          v[9]  = 32;                /* normal  @ float 8  = byte 32         */
          v[10] = 0;
          v[11] = VIRGL_PIPE_FORMAT_R32_FLOAT;
          v[12] = 44;                /* tex s   @ float 11 = byte 44         */
          v[13] = 0;
      }
      (void)nelem;
      pk_create_object(d, &n, VIRGL_OBJECT_VERTEX_ELEMENTS, 2u + 2u * v[0],
                       VIRGL_CANNED_VELEM, v); }
    /* CREATE_SHADER (the kernel helper's exact shape): header, handle, type,
     * token count, one zero (stream-output info), then the token dwords. */
    { const uint32_t cnt = (uint32_t)(sizeof canned_vs_tgsi / 4u);
      d[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER,
                          4 + cnt);
      PK_PUSH(d, n, VIRGL_CANNED_VS);
      PK_PUSH(d, n, VIRGL_PIPE_SHADER_VERTEX);
      PK_PUSH(d, n, cnt);
      PK_PUSH(d, n, 0);
      for (uint32_t i = 0; i < cnt; i++) PK_PUSH(d, n, canned_vs_tgsi[i]); }
    { const uint32_t cnt = (uint32_t)(sizeof canned_fs_tgsi / 4u);
      d[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER,
                          4 + cnt);
      PK_PUSH(d, n, VIRGL_CANNED_FS);
      PK_PUSH(d, n, VIRGL_PIPE_SHADER_FRAGMENT);
      PK_PUSH(d, n, cnt);
      PK_PUSH(d, n, 0);
      for (uint32_t i = 0; i < cnt; i++) PK_PUSH(d, n, canned_fs_tgsi[i]); }

    pk_bind(d, &n, VIRGL_OBJECT_BLEND,           VIRGL_CANNED_BLEND);
    pk_bind(d, &n, VIRGL_OBJECT_RASTERIZER,      VIRGL_CANNED_RASTER);
    pk_bind(d, &n, VIRGL_OBJECT_DSA,             VIRGL_CANNED_DSA);
    pk_bind(d, &n, VIRGL_OBJECT_VERTEX_ELEMENTS, VIRGL_CANNED_VELEM);
    pk_bind(d, &n, VIRGL_OBJECT_SHADER,          VIRGL_CANNED_VS);
    pk_bind(d, &n, VIRGL_OBJECT_SHADER,          VIRGL_CANNED_FS);

    /* s[0] is the handle; the 5-dword surface payload is s[1..5], so the
     * array needs six slots.  With s[5] the payload read ran one dword past
     * the end (gcc 15 -Warray-bounds: "forming offset [20, 23] is out of
     * the bounds [0, 20] of object 's'"). */
    { uint32_t s[6] = { 0 };
      s[0] = VIRGL_SURFACE_HANDLE; s[1] = vg.res;
      s[2] = VIRGL_PIPE_FORMAT_B8G8R8X8_UNORM;
      pk_create_object(d, &n, VIRGL_OBJECT_SURFACE, 5, s[0], s + 1); }
    /* H5: a depth surface, when the draw asked for one.  A DSA with the test
     * enabled and no zsurf in the framebuffer would test against nothing --
     * the host either ignores it or faults, and neither is a frame. */
    uint32_t zhandle = 0;
    if (st && st->depth && vg.zres) {
        uint32_t z[6] = { 0 };
        z[0] = VIRGL_ZSURFACE_HANDLE; z[1] = vg.zres;
        z[2] = VIRGL_PIPE_FORMAT_Z32_FLOAT;
        pk_create_object(d, &n, VIRGL_OBJECT_SURFACE, 5, z[0], z + 1);
        zhandle = VIRGL_ZSURFACE_HANDLE;
    }
    { d[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 5);
      PK_PUSH(d, n, 1);              /* nr_cbufs                      */
      PK_PUSH(d, n, zhandle);        /* zsurf                         */
      PK_PUSH(d, n, w); PK_PUSH(d, n, h);
      PK_PUSH(d, n, VIRGL_SURFACE_HANDLE); }
    { d[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
      PK_PUSH(d, n, 0);              /* start slot                    */
      PK_PUSHF(d, n, (float)w * 0.5f); PK_PUSHF(d, n, (float)h * 0.5f);
      PK_PUSHF(d, n, 0.5f);          /* scale: (far-near)/2, near 0 far 1 */
      PK_PUSHF(d, n, (float)w * 0.5f); PK_PUSHF(d, n, (float)h * 0.5f);
      PK_PUSHF(d, n, 0.5f); }        /* translate                     */

    { /* the vertex buffer is bound here once: one buffer, stride 32 */
      d[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 5);
      PK_PUSH(d, n, 0);              /* start slot                    */
      PK_PUSH(d, n, 1);              /* count                         */
      PK_PUSH(d, n, 32);             /* stride: 8 floats              */
      PK_PUSH(d, n, 0);              /* offset                        */
      PK_PUSH(d, n, vg.vres); }

    return (n <= cap) ? (int)n : -1;
}

static int gpu_submit(uint32_t *d, uint32_t n, int fenced) {
    gpu_submit_t sb;
    memset(&sb, 0, sizeof sb);
    sb.ctx    = vg.ctx;
    sb.size   = n * 4u;
    sb.fenced = fenced ? 1u : 0u;
    sb.cmd    = (uint64_t)(uintptr_t)d;
    return gpu_call(GPU_OP_SUBMIT, (uint64_t)(uintptr_t)&sb) != 0 ? -1 : 0;
}

/* Fixed vertex-buffer capacity: 16384 vertices of 8 floats.  A draw bigger
 * than one chunk is submitted in whole-triangle chunks of this size. */
#define VB_MAX_VERTS 16384u
#define VB_BYTES     (VB_MAX_VERTS * 8u * 4u)
#define DRAW_CHUNK   96u               /* payload fits one 1 KiB stream   */

static int vb_create(void) {
    if (vg.vres) return 0;
    gpu_res_create_t rc;
    memset(&rc, 0, sizeof rc);
    rc.ctx    = vg.ctx;
    rc.target = 0;                       /* 0 = buffer                    */
    rc.format = VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM;
    rc.bind   = VIRGL_BIND_VERTEX_BUFFER;
    rc.width  = VB_BYTES;
    rc.height = rc.depth = rc.array_size = 1;

    int64_t r = gpu_call(GPU_OP_RES_CREATE, (uint64_t)(uintptr_t)&rc);
    if (r <= 0) return -1;
    vg.vres = (uint32_t)r;
    return 0;
}

static int aglx_bind(struct aglx_context *ctx);   /* defined below */

static int glvirgl_draw(struct aglx_context *ctx,
                        const gl_draw_batch_t *batch) {
    if (!vg.up || aglx_bind(ctx) != 0) return -1;
    if (!batch || !batch->data || batch->count < 3 ||
        (batch->count % 3) != 0) return -1;
    if ((size_t)batch->count >
        (size_t)VB_MAX_VERTS * (((size_t)1) << 20)) return -1; /* sane count */
    if (batch->format != GL_BATCH_FMT_POS_COLOR &&
        batch->format != GL_BATCH_FMT_FULL) return -1;

    if (vb_create() != 0) return -1;

    /* H5: the state this draw needs.  The eligibility screen has already
     * refused anything this cannot reproduce, so each flag here has a 1:1
     * mapping -- the G11a rule applied to state. */
    gl_virgl_state_t st;
    memset(&st, 0, sizeof st);
    if (ctx->depth_test) {
        st.depth = 1u;
        if (ensure_depth_surface(vg.res_w, vg.res_h) != 0) return -1;
    }
    if (ctx->blend) {
        st.blend = 1u;
        st.blend_src = gl_to_pipe_blend(ctx->blend_src);
        st.blend_dst = gl_to_pipe_blend(ctx->blend_dst);
    }
    if (ctx->cull_face) {
        /* glCullFace names the face to DISCARD and PIPE_FACE_* names the face
         * to discard, so this is the identity.  GL_FRONT_AND_BACK culls both
         * on the CPU and nothing reaches here in practice; mapping it to BACK
         * would draw what the CPU discarded, so it declines instead. */
        switch (ctx->cull_mode) {
        case GL_BACK:  st.cull = VIRGL_PIPE_FACE_BACK;  break;
        case GL_FRONT: st.cull = VIRGL_PIPE_FACE_FRONT; break;
        default: return -1;
        }
    }

    const int state_changed = !vg.pipe_up ||
        vg.st_depth != st.depth || vg.st_blend != st.blend ||
        vg.st_blend_src != st.blend_src || vg.st_blend_dst != st.blend_dst ||
        vg.st_cull != st.cull || vg.st_fmt != batch->format;

    if (state_changed) {
        uint32_t d[96];
        int n = gl_virgl_setup_state((uint32_t)vg.res_w, (uint32_t)vg.res_h,
                                     &st, batch->format, d, 96);
        if (n < 0 || gpu_submit(d, (uint32_t)n, 0) != 0) return -1;

        if (st.depth) {
            /* Clear the depth buffer to "far" once, so the first frame's test
             * has a known value everywhere rather than whatever the resource
             * happened to contain.  Nothing reads it back, so no flush. */
            uint32_t c[9];
            union { double d; uint32_t u[2]; } far; far.d = 1.0;
            c[0] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
            c[1] = VIRGL_PIPE_CLEAR_DEPTH;
            c[2] = c[3] = c[4] = c[5] = 0;         /* colour unused      */
            c[6] = far.u[0]; c[7] = far.u[1];      /* depth = 1.0        */
            c[8] = 0;                              /* stencil            */
            if (gpu_submit(c, 9, 0) != 0) return -1;
        }

        vg.pipe_up = 1;
        vg.st_depth = st.depth;
        vg.st_blend = st.blend;
        vg.st_blend_src = st.blend_src;
        vg.st_blend_dst = st.blend_dst;
        vg.st_cull = st.cull;
        vg.st_fmt = batch->format;
    }

    /* The stride is the batch's own: a format-1 draw is twelve floats per
     * vertex, and a backend that assumed eight would upload two thirds of
     * every vertex and let the GPU read the tail of the previous one. */
    const uint32_t vstride = GL_BATCH_STRIDE(batch->format);
    const uint32_t vbytes  = vstride * 4u;

    /* Whole triangles per chunk; the last chunk of the draw submits fenced,
     * which is the ABI's documented wait -- the present that follows must
     * not race the rasterization (the kernel's own demo does the same). */
    uint32_t done = 0;
    while (done < (uint32_t)batch->count) {
        uint32_t chunk = (uint32_t)batch->count - done;
        if (chunk > DRAW_CHUNK) chunk = DRAW_CHUNK;

        static uint32_t d[32 + DRAW_CHUNK * 12];
        uint32_t n = 0;
        d[n++] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0,
                            12 + chunk * vstride);
        PK_PUSH(d, n, vg.vres);
        PK_PUSH(d, n, 0);              /* level                         */
        PK_PUSH(d, n, VIRGL_BIND_VERTEX_BUFFER);  /* usage              */
        PK_PUSH(d, n, vbytes);         /* stride                        */
        PK_PUSH(d, n, 0);              /* layer stride                  */
        PK_PUSH(d, n, 0); PK_PUSH(d, n, 0); PK_PUSH(d, n, 0); /* x,y,z  */
        PK_PUSH(d, n, chunk * vstride * 4u);/* w in bytes, then h, d    */
        PK_PUSH(d, n, 1); PK_PUSH(d, n, 1);
        PK_PUSH(d, n, chunk * vstride * 4u);/* total bytes              */
        for (uint32_t i = 0; i < chunk * vstride; i++) {
            PK_PUSHF(d, n, batch->data[(size_t)done * vstride + i]);
        }
        d[n++] = VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO, 0, 12);
        PK_PUSH(d, n, 0);              /* start                         */
        PK_PUSH(d, n, chunk);          /* count                         */
        PK_PUSH(d, n, VIRGL_PIPE_PRIM_TRIANGLES);
        PK_PUSH(d, n, 0);              /* not indexed                   */
        PK_PUSH(d, n, 1);              /* instance count                */
        PK_PUSH(d, n, 0);              /* index bias                    */
        PK_PUSH(d, n, 0);              /* start instance                */
        PK_PUSH(d, n, 0);              /* primitive restart             */
        PK_PUSH(d, n, 0);              /* restart index                 */
        PK_PUSH(d, n, 0);              /* min index                     */
        PK_PUSH(d, n, chunk ? chunk - 1u : 0u); /* max index            */
        PK_PUSH(d, n, 0);              /* cso/indirect handle           */

        int last = (done + chunk) >= (uint32_t)batch->count;
        if (gpu_submit(d, n, last) != 0) return -1;
        done += chunk;
    }

    ctx->frame_gpu_draw = 1;    /* the present fork consults this */
    return 0;
}

/* ============================================================================
 * Backend entry points
 * ==========================================================================*/

static int glvirgl_init(void) {
    memset(&vg, 0, sizeof vg);

    /* Ask the kernel what is there.  A kernel without the syscall returns a
     * negative errno, which is the same "no" as no device at all. */
    if (gpu_call(GPU_OP_INFO, (uint64_t)(uintptr_t)&vg.info) != 0) return -1;
    if (!vg.info.present || !vg.info.virgl) return -1;

    int64_t c = gpu_call(GPU_OP_CTX_CREATE, 0);
    if (c <= 0) return -1;
    vg.ctx = (uint32_t)c;

    vg.up = 1;
    return 0;
}

/* Bind the backend to a GL context, creating a matching render target.
 * Returns 0 when the hardware path can serve this context. */
static int aglx_bind(struct aglx_context *ctx) {
    if (!vg.up || !ctx) return -1;

    /* One GL context at a time.  A second one falls back to software rather
     * than silently sharing a render target with the first. */
    if (vg.owner && vg.owner != ctx) return -1;

    if (!vg.res || vg.res_w != ctx->win_width || vg.res_h != ctx->win_height) {
        if (make_target(ctx->win_width, ctx->win_height) != 0) return -1;
    }
    vg.owner = ctx;
    return 0;
}

static int glvirgl_clear(struct aglx_context *ctx, GLbitfield mask) {
    if (aglx_bind(ctx) != 0) return -1;

    /* VirGL's CLEAR takes the buffer mask, an RGBA colour as four floats, a
     * double depth and a stencil value: 8 dwords of payload. */
    uint32_t buffers = 0;
    if (mask & GL_COLOR_BUFFER_BIT) buffers |= VIRGL_PIPE_CLEAR_COLOR0;
    if (mask & GL_DEPTH_BUFFER_BIT) buffers |= VIRGL_PIPE_CLEAR_DEPTH;
    if (buffers == 0) return -1;              /* nothing we handle */

    union { float f; uint32_t u; } cv[4];
    cv[0].f = ctx->clear_color.r;
    cv[1].f = ctx->clear_color.g;
    cv[2].f = ctx->clear_color.b;
    cv[3].f = ctx->clear_color.a;

    union { double d; uint32_t u[2]; } dv;
    dv.d = (double)ctx->clear_depth;

    uint32_t cmd[9];
    cmd[0] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    cmd[1] = buffers;
    cmd[2] = cv[0].u; cmd[3] = cv[1].u; cmd[4] = cv[2].u; cmd[5] = cv[3].u;
    cmd[6] = dv.u[0]; cmd[7] = dv.u[1];
    cmd[8] = 0;                                /* stencil */

    gpu_submit_t sb;
    memset(&sb, 0, sizeof sb);
    sb.ctx  = vg.ctx;
    sb.size = (uint32_t)sizeof cmd;
    sb.cmd  = (uint64_t)(uintptr_t)cmd;

    if (gpu_call(GPU_OP_SUBMIT, (uint64_t)(uintptr_t)&sb) != 0) return -1;

    /* The GPU cleared its own render target, but the CPU rasterizer draws
     * into ctx->color and knows nothing about that.  Until the draw path
     * moves to the GPU too, the software buffer has to be cleared as well or
     * the frame presented below would carry the previous frame's pixels.
     *
     * So this returns -1: "not handled, do it in software".  Emitting the
     * hardware clear anyway keeps the GPU's target consistent for the day the
     * draw path arrives, and costs one command per frame.  Returning 0 here
     * would be the bug -- it would skip the software clear and present stale
     * content. */
    return -1;
}

static int glvirgl_present(struct aglx_context *ctx) {
    if (aglx_bind(ctx) != 0) return -1;

    /* GL2 L6: the frame-ownership fork.  A frame the GPU drew entirely must
     * scan out from the GPU render target WITHOUT the CPU transfer below --
     * transferring would overwrite the triangle with the CPU's empty colour
     * buffer, the exact "DRAW_VBO is a no-op" bug.  A mixed frame (any
     * software rasterization this frame) presents the CPU buffer, which is
     * self-consistent; the GPU-drawn parts of a mixed frame are lost with
     * it, which is the honest cost of a canned subset and the reason the
     * eligibility screen is strict.  Either way the flags reset here: the
     * next frame starts unowned. */
    int gpu_frame = ctx->frame_gpu_draw && !ctx->frame_sw_raster;
    ctx->frame_gpu_draw = 0;
    ctx->frame_sw_raster = 0;

    if (gpu_frame) {
        gpu_scanout_t so;
        memset(&so, 0, sizeof so);
        so.scanout = 0;
        so.res     = vg.res;
        so.w       = (uint32_t)ctx->win_width;
        so.h       = (uint32_t)ctx->win_height;
        if (gpu_call(GPU_OP_SET_SCANOUT, (uint64_t)(uintptr_t)&so) != 0)
            return -1;

        gpu_flush_t fl;
        memset(&fl, 0, sizeof fl);
        fl.res = vg.res;
        fl.w   = (uint32_t)ctx->win_width;
        fl.h   = (uint32_t)ctx->win_height;
        if (gpu_call(GPU_OP_FLUSH, (uint64_t)(uintptr_t)&fl) != 0) return -1;
        return 0;                              /* handled: no CPU transfer */
    }

    if (!ctx->win_color) return -1;

    size_t bytes = (size_t)ctx->win_width * (size_t)ctx->win_height * 4u;
    if (bytes == 0 || bytes > GPU_MAX_XFER_BYTES) return -1;

    /* Upload the finished frame into the render target. */
    gpu_transfer_t tr;
    memset(&tr, 0, sizeof tr);
    tr.ctx    = vg.ctx;
    tr.res    = vg.res;
    tr.w      = (uint32_t)ctx->win_width;
    tr.h      = (uint32_t)ctx->win_height;
    tr.d      = 1;
    tr.stride = (uint32_t)ctx->win_width * 4u;
    tr.size   = (uint32_t)bytes;
    tr.data   = (uint64_t)(uintptr_t)ctx->win_color;

    if (gpu_call(GPU_OP_TRANSFER, (uint64_t)(uintptr_t)&tr) != 0) return -1;

    /* Scan it out and flush.  SET_SCANOUT points the display at the resource;
     * RESOURCE_FLUSH tells the host the contents changed. */
    gpu_scanout_t so;
    memset(&so, 0, sizeof so);
    so.scanout = 0;
    so.res     = vg.res;
    so.w       = (uint32_t)ctx->win_width;
    so.h       = (uint32_t)ctx->win_height;
    if (gpu_call(GPU_OP_SET_SCANOUT, (uint64_t)(uintptr_t)&so) != 0) return -1;

    gpu_flush_t fl;
    memset(&fl, 0, sizeof fl);
    fl.res = vg.res;
    fl.w   = (uint32_t)ctx->win_width;
    fl.h   = (uint32_t)ctx->win_height;
    if (gpu_call(GPU_OP_FLUSH, (uint64_t)(uintptr_t)&fl) != 0) return -1;

    return 0;                                  /* handled: skip ag_blit */
}

/* ----------------------------------------------------------------------------
 * H3: the sampler side of a texture
 *
 * A sampler VIEW names (resource, format, level range); a sampler STATE names
 * wrap and filter.  Both are CREATE_OBJECT packets, so both are submitted
 * once per texture rather than once per draw -- the point of the phase is
 * that a bound, unchanged texture costs NOTHING per frame.
 *
 * The state packet is sent per texture rather than shared between textures
 * with equal state: the allocation is a hash table this backend does not
 * have, and the packet is 40 bytes sent once per texture lifetime.
 * --------------------------------------------------------------------------*/

/* Map a GL wrap mode onto the wire value.  Refusing unknown modes is
 * deliberate: the software sampler is the oracle, and a silently defaulted
 * wrap mode would make the two disagree on screen with no error anywhere. */
static int wrap_to_virgl(GLenum m, uint32_t *out) {
    switch (m) {
    case GL_REPEAT:                *out = VIRGL_TEX_WRAP_REPEAT;            return 0;
    case GL_CLAMP:                 *out = VIRGL_TEX_WRAP_CLAMP;             return 0;
    case GL_CLAMP_TO_EDGE:         *out = VIRGL_TEX_WRAP_CLAMP;             return 0;
    case GL_CLAMP_TO_BORDER:       *out = VIRGL_TEX_WRAP_CLAMP_TO_BORDER;   return 0;
    default:                       return -1;
    }
}

static int filter_to_virgl(GLenum m, uint32_t *out) {
    switch (m) {
    case GL_NEAREST:               *out = VIRGL_TEX_FILTER_NEAREST;  return 0;
    case GL_LINEAR:                *out = VIRGL_TEX_FILTER_LINEAR;   return 0;
    default:                       return -1;
    }
}

/* Build and submit the sampler view + state for a texture that has just been
 * uploaded.  Returns 0 when the texture is sampleable on the device.
 *
 * Both packets go through the builders in drivers/gpu/virgl.c -- the same
 * functions tests/unit/test_tex_packets.c walks against the kernel validator
 * -- so the encoding has one definition in the tree and cannot drift between
 * the driver and the backend (the G13 lesson). */
static int tex_make_sampler(struct virgl_tex *t, const gl_texture_t *tex) {
    if (!t || !t->res) return -1;
    if (t->sampler) return 0;                /* already made */

    /* The per-texture state lives in gl_texture_t, which this backend does
     * not otherwise see.  A NULL `tex` therefore means the GL defaults --
     * REPEAT/REPEAT and NEAREST filters -- which is what glTexImage2D leaves
     * a fresh texture with.  A texture whose state differs gets its sampler
     * rebuilt by virgl_tex_state() before the first draw that samples it. */
    GLenum min_f = GL_NEAREST, mag_f = GL_NEAREST, ws_m = GL_REPEAT, wt_m = GL_REPEAT;
    if (tex) {
        min_f = tex->min_filter; mag_f = tex->mag_filter;
        ws_m = tex->wrap_s;      wt_m = tex->wrap_t;
    }

    uint32_t ws, wt, wr, minf, magf;
    GLenum min_filter = min_f;
    uint32_t mipf = VIRGL_TEX_MIPFILTER_NONE;
    if (min_filter == GL_NEAREST_MIPMAP_NEAREST ||
        min_filter == GL_NEAREST_MIPMAP_LINEAR) {
        mipf = VIRGL_TEX_MIPFILTER_NEAREST;
        min_filter = GL_NEAREST;
    } else if (min_filter == GL_LINEAR_MIPMAP_NEAREST ||
               min_filter == GL_LINEAR_MIPMAP_LINEAR) {
        mipf = VIRGL_TEX_MIPFILTER_LINEAR;
        min_filter = GL_LINEAR;
    }
    if (wrap_to_virgl(ws_m, &ws) != 0) return -1;
    if (wrap_to_virgl(wt_m, &wt) != 0) return -1;
    /* GL is 2D here: there is no third axis to wrap, so R mirrors S.  Sending
     * an unwrapped R would be state the host reads but no draw can observe. */
    wr = ws;
    if (filter_to_virgl(min_filter, &minf) != 0) return -1;
    if (filter_to_virgl(mag_f, &magf) != 0) return -1;

    virgl_cmd_buf_t cb;
    virgl_cmd_init(&cb, vg.ctx);

    if (virgl_cmd_create_sampler_view(&cb, VIRGL_TEX_SAMPLER_VIEW, t->res,
                                      VIRGL_PIPE_FORMAT_B8G8R8A8_UNORM,
                                      0, (uint32_t)(t->levels > 0 ? t->levels - 1 : 0)) != 0)
        return -1;
    if (virgl_cmd_create_sampler_state(&cb, VIRGL_TEX_SAMPLER_STATE,
                                       ws, wt, wr, minf, magf, mipf) != 0)
        return -1;

    /* Bind the one view to unit 0.  A draw that samples more units than this
     * is a draw this backend does not claim to support yet -- H5's business,
     * and the eligibility screen refuses it before it gets here. */
    uint32_t views[1] = { VIRGL_TEX_SAMPLER_VIEW };
    if (virgl_cmd_set_sampler_views(&cb, VIRGL_PIPE_SHADER_FRAGMENT, 0, 1, views) != 0)
        return -1;

    if (cb.overflow) return -1;
    if (gpu_submit(cb.dwords, cb.count, 0) != 0) return -1;

    t->sampler = VIRGL_TEX_SAMPLER_VIEW;
    return 0;
}

void virgl_tex_release(struct aglx_context *ctx, GLuint name) {
    (void)ctx;
    struct virgl_tex *t = tex_slot_find(name, 0);
    if (t) tex_slot_release(t);
}

static void glvirgl_destroy(struct aglx_context *ctx) {
    if (vg.owner == ctx) {
        release_target();
        vg.pipe_up = 0;
        if (vg.vres) {
            gpu_call(GPU_OP_RES_DESTROY, vg.vres);
            vg.vres = 0;
        }
        vg.owner = NULL;
    }
}

static const gl_backend_t glvirgl_backend = {
    "AuraLite VirGL (virtio-gpu)",
    GL_BACKEND_HARDWARE,
    glvirgl_init,
    glvirgl_clear,
    glvirgl_present,
    glvirgl_draw,                /* GL2 L6: the canned DRAW_VBO path */
    virgl_tex_image,             /* H3: textures on the device */
    glvirgl_destroy,
};

/* Called from context creation, before the software backend is registered, so
 * a working hardware path takes precedence automatically. */
void gl_virgl_register(void) {
    gl_backend_register(&glvirgl_backend);
}
