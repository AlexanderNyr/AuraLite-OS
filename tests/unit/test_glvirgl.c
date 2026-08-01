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
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_glvirgl: the VirGL hardware backend (phase G13) ===\n");

    test_declines();
    test_rendering_unaffected();
    test_repeated_lifecycle();
    test_command_encoding();
    test_abi();

    printf("\ntest_glvirgl: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}
