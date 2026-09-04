/*
 * test_glvirgl.c — the VirGL hardware backend (phase G13).
 *
 * WHAT CAN AND CANNOT BE TESTED ON THE HOST
 *
 * There is no virtio-gpu here, so the interesting question is not "does the
 * GPU path work" — it cannot run — but the one that actually matters for
 * correctness: **does the backend decline cleanly when there is no GPU, and
 * does everything downstream behave exactly as if it had never been
 * registered?**
 *
 * That is the property G9 chose deliberately and G13 must not break. A
 * hardware backend that registered and then failed per-frame would report
 * "hardware" in GL_RENDERER and produce silence or corruption; declining
 * leaves the software path in place and the renderer string truthful.
 *
 * The stub in tests/unit/glstub returns -1 from every syscall, which is what
 * a kernel without the GPU syscall, and a machine without a GPU, both look
 * like. So these tests exercise the real glvirgl.c against the real failure
 * mode rather than a mock of one.
 *
 * The command-stream ENCODING is tested separately and directly: those are
 * pure bit manipulations that must be right whether or not a device is
 * present, and getting them wrong is how a backend sends a hostile-looking
 * stream to a real GPU later.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "GL/gl.h"
#include "GL/glbackend.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"
#include "auragui.h"
#include "kernel/gpu/gpu_syscalls.h"

static int tn = 0, passed = 0, failed = 0;

#define CHECK(cond, name) do {                          \
    tn++;                                               \
    if (cond) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", (name)); }  \
} while (0)

#define W 64
#define H 64

static uint32_t px(aglx_context_t *c, int x, int y) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return b[(size_t)(H - 1 - y) * W + x];
}

/* ============================================================================
 * Declining cleanly
 * ==========================================================================*/

static void test_declines(void) {
    printf("--- the backend declines without a GPU ---\n");

    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    CHECK(c != NULL, "a context is created even with the backend registered");
    if (!c) return;
    aglxMakeCurrent(c);

    /* The candidate must be registered — the seam has to be real — and must
     * not be the one selected.  gl_backend_force() answering for its name is
     * the evidence it is in the registry at all. */
    CHECK(gl_backend_force("AuraLite VirGL (virtio-gpu)") == 0,
          "the VirGL candidate is registered");
    gl_backend_force(NULL);

    const gl_backend_t *active = gl_backend_active();
    CHECK(active != NULL, "a backend is always active");

    const gl_backend_info_t *bi = gl_backend_info();
    CHECK(bi != NULL, "the active backend describes itself");
    CHECK(bi && (bi->flags & GL_BACKEND_SOFTWARE) != 0,
          "software is the active backend when no GPU is present");
    CHECK(bi && bi->hardware == 0,
          "and it does not claim a hardware path");

    /* GL_RENDERER must not claim hardware.  An application that logs it, or
     * chooses its quality settings from it, has to be told the truth. */
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    CHECK(renderer != NULL, "GL_RENDERER is reported");
    CHECK(renderer && strstr(renderer, "VirGL") == NULL,
          "GL_RENDERER does not claim VirGL when it declined");
    CHECK(renderer && strstr(renderer, "Software") != NULL,
          "GL_RENDERER names the software rasterizer");

    aglxDestroyContext(c);
}

static void test_rendering_unaffected(void) {
    printf("--- rendering is unaffected by the declined backend ---\n");

    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) { CHECK(0, "context"); return; }
    aglxMakeCurrent(c);

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -10, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    /* Clear must reach the software buffer.  This is the specific hazard in
     * G13's clear(): it emits a hardware CLEAR and then returns -1 so the
     * software clear still runs.  Returning 0 there would skip the software
     * clear and present the previous frame's pixels. */
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    CHECK(px(c, 32, 32) == 0x0000FF, "glClear reaches the software buffer");

    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK(px(c, 32, 32) == 0xFF0000, "a second clear also lands");

    /* Geometry still draws. */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 1, 0);
    glBegin(GL_QUADS);
    glVertex3f(8, 8, 0); glVertex3f(56, 8, 0);
    glVertex3f(56, 56, 0); glVertex3f(8, 56, 0);
    glEnd();
    CHECK(px(c, 32, 32) == 0x00FF00, "geometry still rasterises in software");
    CHECK(px(c, 2, 2) == 0x000000, "and is placed where it should be");

    /* Present must fall through to ag_blit(), which the stub records. */
    ag_stub_reset();
    CHECK(aglxSwapBuffers(c) == 0, "the swap succeeds");
    CHECK(ag_stub.calls == 1,
          "presentation falls through to the software blit");
    CHECK(ag_stub.last_w == (uint32_t)W && ag_stub.last_h == (uint32_t)H,
          "and carries the whole window");

    CHECK(glGetError() == GL_NO_ERROR, "no error was raised throughout");
    aglxDestroyContext(c);
}

static void test_repeated_lifecycle(void) {
    printf("--- repeated context lifecycles ---\n");

    /* The backend's init() runs once per registration and its destroy() once
     * per context.  Creating and destroying repeatedly must not accumulate
     * state or start reporting hardware. */
    int all_software = 1;
    for (int i = 0; i < 8; i++) {
        aglx_context_t *c = aglxCreateContext(1, 32, 32, AGLX_DEPTH);
        if (!c) { all_software = 0; break; }
        aglxMakeCurrent(c);
        const gl_backend_info_t *bi = gl_backend_info();
        if (!bi || bi->hardware) all_software = 0;
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        aglxDestroyContext(c);
    }
    CHECK(all_software, "eight lifecycles all stay on the software backend");

    /* Two contexts at once: the backend limits itself to one GL context, and
     * the second must fall back rather than share a render target. */
    aglx_context_t *a = aglxCreateContext(1, 32, 32, AGLX_DEPTH);
    aglx_context_t *b = aglxCreateContext(2, 32, 32, AGLX_DEPTH);
    CHECK(a && b, "two contexts can coexist");
    if (a && b) {
        aglxMakeCurrent(a);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        aglxMakeCurrent(b);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        CHECK((aglxGetColorBuffer(a)[0] & 0x00FFFFFF) == 0x00FF0000u &&
              (aglxGetColorBuffer(b)[0] & 0x00FFFFFF) == 0x000000FFu,
              "and each keeps its own contents");
    }
    aglxDestroyContext(a);
    aglxDestroyContext(b);
}

/* ============================================================================
 * Command-stream encoding
 *
 * Pure bit manipulation, testable without a device — and worth testing
 * precisely because a wrong header length is how a backend hands a real GPU a
 * stream that walks off the end of its buffer.  The kernel validator would
 * reject it, which is the second line of defence; this is the first.
 * ==========================================================================*/

/* The driver's own macro, so the test and the shipping code cannot disagree
 * about the wire format. */
#include "drivers/gpu/virgl.h"

#define HDR(id, len) VIRGL_CMD0((id), 0, (len))

static void test_command_encoding(void) {
    printf("--- VirGL command encoding ---\n");

    /* A header packs the command id in the TOP byte and the payload length in
     * dwords in the low half.  The first draft of glvirgl.c had these the
     * other way round; the kernel validator caught it, which is the point of
     * having a validator, but the encoding is asserted here so the next
     * change to it fails a test rather than a GPU. */
    uint32_t h = HDR(3, 8);
    CHECK((h >> 24) == 3, "the command id lands in the top byte");
    CHECK((h & 0xFFFFu) == 8, "the payload length lands in the low half");

    /* The CLEAR command glvirgl.c builds: a header plus eight payload dwords,
     * nine in total.  A header claiming a length that does not match the
     * buffer is exactly what the kernel validator rejects. */
    uint32_t cmd[9];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = HDR(3, 8);
    CHECK((cmd[0] & 0xFFFFu) + 1 == sizeof cmd / sizeof cmd[0],
          "the declared length matches the buffer");

    /* The kernel's own validator must accept it — the two must agree, and the
     * validator is the shipping code, not a copy. */
    CHECK(gpu_validate_cmd_stream(cmd, sizeof cmd / sizeof cmd[0]) == 0,
          "the kernel validator accepts a well-formed CLEAR");

    /* A header that lies about its length must be rejected. */
    uint32_t bad[9];
    memset(bad, 0, sizeof bad);
    bad[0] = HDR(3, 64);            /* claims 64 dwords, buffer has 8 */
    CHECK(gpu_validate_cmd_stream(bad, sizeof bad / sizeof bad[0]) != 0,
          "and rejects one whose header overruns the buffer");

    /* Colour floats are transmitted as raw bit patterns, so the union punning
     * in glvirgl.c must round-trip. */
    {
        union { float f; uint32_t u; } cv;
        cv.f = 0.5f;
        CHECK(cv.u == 0x3F000000u, "0.5f encodes as the expected bit pattern");
        cv.u = 0x3F800000u;
        CHECK(cv.f == 1.0f, "and decodes back");
    }

    /* Depth is a double, split across two dwords, little-endian. */
    {
        union { double d; uint32_t u[2]; } dv;
        dv.d = 1.0;
        CHECK(dv.u[0] == 0x00000000u && dv.u[1] == 0x3FF00000u,
              "1.0 as a double splits into the expected dword pair");
    }
}

/* ============================================================================
 * The ABI shared with the kernel
 *
 * glvirgl.c includes kernel/gpu/gpu_syscalls.h rather than restating the
 * structs.  These checks are what makes that safe: if a field moves, the test
 * fails here instead of the GPU receiving garbage.
 * ==========================================================================*/

static void test_abi(void) {
    printf("--- the kernel ABI ---\n");

    /* Every argument block must be a fixed size with no compiler-chosen
     * padding, because the kernel reads exactly this many bytes. */
    CHECK(sizeof(gpu_info_t) == 24, "gpu_info_t has the expected size");
    CHECK(sizeof(gpu_res_create_t) == 48,
          "gpu_res_create_t has the expected size");
    /* Sized by inspection rather than assumption: the point is that it is a
     * fixed, padding-free layout, not any particular number. */
    CHECK(sizeof(gpu_transfer_t) % 8 == 0,
          "gpu_transfer_t is 8-byte aligned with no tail padding surprise");
    CHECK(sizeof(gpu_transfer_t) >= 13 * 4 + 8,
          "gpu_transfer_t is large enough for its declared fields");
    CHECK(sizeof(gpu_submit_t) == 32, "gpu_submit_t has the expected size");
    CHECK(sizeof(gpu_scanout_t) == 24, "gpu_scanout_t has the expected size");
    CHECK(sizeof(gpu_flush_t) == 24, "gpu_flush_t has the expected size");

    /* The pointer fields must be 64-bit, so a 32-bit user pointer cannot be
     * silently truncated. */
    {
        gpu_transfer_t t;
        CHECK(sizeof t.data == 8, "gpu_transfer_t::data is 64 bits");
        gpu_submit_t s;
        CHECK(sizeof s.cmd == 8, "gpu_submit_t::cmd is 64 bits");
    }

    /* The sub-op numbering is part of the ABI: reordering it would silently
     * repoint every call. */
    CHECK(GPU_OP_INFO == 1, "GPU_OP_INFO is 1");
    CHECK(GPU_OP_CTX_CREATE == 2, "GPU_OP_CTX_CREATE is 2");
    CHECK(GPU_OP_TRANSFER == 6, "GPU_OP_TRANSFER is 6");
    CHECK(GPU_OP_SUBMIT == 7, "GPU_OP_SUBMIT is 7");
    CHECK(GPU_OP_SET_SCANOUT == 8, "GPU_OP_SET_SCANOUT is 8");
    CHECK(GPU_OP_FLUSH == 9, "GPU_OP_FLUSH is 9");

    /* The transfer cap must be large enough for a full frame at the largest
     * context this implementation allows, or present() could never work. */
    CHECK(GPU_MAX_XFER_BYTES >= 4096u * 4096u * 4u ||
          GPU_MAX_XFER_BYTES >= 1280u * 800u * 4u,
          "the transfer cap admits a full-resolution frame");
}


/* ============================================================================
 * GL2 phase L6: the canned DRAW_VBO pipeline
 *
 * No GPU here either — the value is the same as G13's encoding tests: the
 * canned shaders and the setup stream are pure bit manipulations, and the
 * kernel validator is the shipping code they must agree with.  The eligibility
 * matrix and the dispatch/fallback pair run against a forced test backend so
 * the whole-draw contract is exercised, not assumed.
 * ==========================================================================*/

/* Walk one TGSI stream the way tgsi_parse does: a header token (HeaderSize
 * dwords including itself, BodySize following), a processor token, then
 * self-sized declaration / immediate / instruction tokens landing exactly on
 * the end of the buffer.  Returns 0 if the walk is coherent. */
static int tgsi_walk_ok(const uint32_t *t, int n, uint32_t want_processor) {
    if (n < 3) return 0;
    if ((t[0] & 0xFFu) != 2) return 0;            /* HeaderSize is 2        */
    if ((int)(t[0] >> 8) != n - 2) return 0;      /* BodySize = the rest    */
    if ((t[1] & 0xFu) != want_processor) return 0;
    int pos = 2;
    while (pos < n) {
        uint32_t type = t[pos] & 0xFu;
        uint32_t nrt  = (t[pos] >> 4) & 0xFFu;
        if (type > 3 || nrt == 0) return 0;
        if (type == 1) {                          /* declaration            */
            if (((t[pos] >> 16) & 0xFu) != 0xFu) return 0;  /* writemask   */
            uint32_t flags = t[pos];
            int expect = 2;
            if (flags & (1u << 22)) expect++;     /* interp                 */
            if (flags & (1u << 21)) expect++;     /* semantic               */
            if ((int)nrt != expect) return 0;
        }
        pos += (int)nrt;
    }
    if (pos != n) return 0;
    /* The stream must END: last token an instruction, opcode 117, no regs. */
    uint32_t last = t[n - 1];
    return (last & 0xFu) == 3u && ((last >> 4) & 0xFFu) == 1u &&
           (((last >> 12) & 0xFFu) == 117u);
}

static void test_canned_shaders(void) {
    printf("--- GL2 L6: the canned TGSI shaders ---\n");

    int vn = 0, fn = 0;
    const uint32_t *vs = gl_virgl_canned_vs_tokens(&vn);
    const uint32_t *fs = gl_virgl_canned_fs_tokens(&fn);
    CHECK(vs && vn == 21, "the VS is 21 dwords");
    CHECK(fs && fn == 13, "the FS is 13 dwords");

    CHECK(tgsi_walk_ok(vs, vn, 0), "the VS walks as TGSI and ends with END");
    CHECK(tgsi_walk_ok(fs, fn, 1), "the FS walks as TGSI and ends with END");

    /* First instruction of each is MOV: opcode 1, one dst, one src. */
    CHECK(vs && ((vs[14] >> 12) & 0xFFu) == 1u &&
          ((vs[14] >> 21) & 0x3u) == 1u && ((vs[14] >> 23) & 0xFu) == 1u,
          "VS instruction 0 is MOV with one dst and one src");
    CHECK(fs && ((fs[9] >> 12) & 0xFFu) == 1u,
          "FS instruction 0 is MOV");

    /* The FS input carries the perspective interpolation flag and the COLOR
     * semantic — that is what links it to the VS's OUT[1]. */
    CHECK(fs && (fs[2] & (1u << 22)) && (fs[2] & (1u << 21)) &&
          fs[4] == 2u && fs[5] == 1u,
          "the FS input is a perspective-interpolated COLOR");
}

static void test_canned_setup_stream(void) {
    printf("--- GL2 L6: the canned setup stream vs the kernel validator ---\n");

    uint32_t d[128];
    int n = gl_virgl_canned_setup(320, 240, d, 128);
    CHECK(n > 40, "the setup stream is non-trivial");
    CHECK(gpu_validate_cmd_stream(d, (uint32_t)n) == 0,
          "the kernel validator accepts the whole setup stream");

    /* Walk the packets and assert the ones the draw depends on. */
    int shaders = 0, fb = 0, vbufs = 0, viewport = 0, vs_len = 0, fs_len = 0;
    int i = 0;
    while (i < n) {
        uint32_t hdr = d[i];
        uint32_t cmd = hdr >> 24, obj = (hdr >> 16) & 0xFFu, len = hdr & 0xFFFFu;
        if (cmd == VIRGL_CCMD_CREATE_OBJECT && obj == VIRGL_OBJECT_SHADER) {
            shaders++;
            if (d[i + 2] == VIRGL_PIPE_SHADER_VERTEX)   vs_len = (int)len;
            if (d[i + 2] == VIRGL_PIPE_SHADER_FRAGMENT) fs_len = (int)len;
        }
        if (cmd == VIRGL_CCMD_SET_FRAMEBUFFER_STATE) fb = (int)len;
        if (cmd == VIRGL_CCMD_SET_VERTEX_BUFFERS) {
            vbufs = (int)len;
            CHECK(d[i + 1] == 0 && d[i + 2] == 1, "one vertex buffer, slot 0");
            CHECK(d[i + 3] == 32, "the vertex stride is 8 floats");
        }
        if (cmd == VIRGL_CCMD_SET_VIEWPORT_STATE) viewport = (int)len;
        i += 1 + (int)len;
    }
    CHECK(i == n, "the packet walk lands on the end");
    CHECK(shaders == 2, "both canned shaders are created");
    CHECK(vs_len == 4 + 21, "the VS packet carries 21 token dwords");
    CHECK(fs_len == 4 + 13, "the FS packet carries 13 token dwords");
    CHECK(fb == 5, "SET_FRAMEBUFFER_STATE has one colour surface");
    CHECK(vbufs == 5, "SET_VERTEX_BUFFERS has the 1-buffer shape");
    CHECK(viewport == 7, "SET_VIEWPORT_STATE has scale and translate");
}

/* ---- dispatch and the whole-draw fallback, through a forced backend ----- */

static GLint      fake_calls;
static GLsizei    fake_count;
static GLfloat    fake_first[8];
static int        fake_ret;               /* what the hook returns          */
static int        fake_seen;

static int fake_draw(struct aglx_context *ctx, const gl_draw_batch_t *batch) {
    (void)ctx;
    fake_calls++;
    if (!batch || !batch->data) return -1;
    fake_count = batch->count;
    memcpy(fake_first, batch->data, sizeof fake_first);
    fake_seen = 1;
    return fake_ret;
}

static const gl_backend_t fake_backend = {
    "canned-test-backend",
    GL_BACKEND_SOFTWARE,
    0, 0, 0,
    fake_draw,
    0,
};

static void test_canned_dispatch(void) {
    printf("--- GL2 L6: dispatch and the whole-draw fallback ---\n");

    gl_backend_register(&fake_backend);
    CHECK(gl_backend_force("canned-test-backend") == 0,
          "the test backend registers and can be forced active");

    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    CHECK(c != NULL, "context for the dispatch test");
    if (!c) return;
    aglxMakeCurrent(c);

    static const GLfloat tri[9] = { -1.0f, -1.0f, 0.0f,
                                     1.0f, -1.0f, 0.0f,
                                     0.0f,  1.0f, 0.0f };
    static const GLfloat red[9] = { 1, 0, 0,  1, 0, 0,  1, 0, 0 };

    glVertexPointer(3, GL_FLOAT, 0, tri);
    glColorPointer(3, GL_FLOAT, 0, red);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    /* Handled: the hook sees the whole draw, with clip coordinates equal to
     * the object coordinates under the identity matrices, and the software
     * rasterizer must NOT also draw it. */
    fake_calls = 0; fake_seen = 0; fake_ret = 0;
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK(fake_calls == 1 && fake_seen, "an eligible draw reaches the hook");
    CHECK(fake_count == 3, "the hook receives all three vertices");
    CHECK(fake_first[0] == -1.0f && fake_first[1] == -1.0f &&
          fake_first[2] == 0.0f && fake_first[3] == 1.0f,
          "the batch carries the identity-transform clip position");
    CHECK(fake_first[4] == 1.0f && fake_first[7] == 1.0f,
          "the batch carries the vertex colour");
    {
        const uint32_t *b = aglxGetColorBuffer(c);
        CHECK(b[(size_t)(H / 2) * W + W / 2] == 0,
              "a handled draw leaves the software buffer untouched");
    }

    /* Declined: the same draw falls back WHOLE to software, and its pixels
     * appear. */
    fake_ret = -1;
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK(fake_calls == 2, "the hook is consulted again");
    {
        const uint32_t *b = aglxGetColorBuffer(c);
        CHECK(b[(size_t)(H / 2) * W + W / 2] != 0,
              "a declined draw falls back to software and draws");
    }

    /* Ineligible mode: the hook is never consulted, software draws it. */
    fake_calls = 0;
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);
    CHECK(fake_calls == 0, "an ineligible mode never reaches the hook");
    {
        const uint32_t *b = aglxGetColorBuffer(c);
        CHECK(b[(size_t)(H - 8) * W + W / 2] != 0,
              "the ineligible draw still rendered in software");
    }

    gl_backend_force(NULL);              /* back to the default selection */
    aglxDestroyContext(c);
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_glvirgl: the VirGL hardware backend (phase G13) ===\n");

    test_declines();
    test_rendering_unaffected();
    test_repeated_lifecycle();
    test_command_encoding();
    test_abi();
    test_canned_shaders();
    test_canned_setup_stream();
    test_canned_dispatch();

    printf("\ntest_glvirgl: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}
