/*
 * test_tex_packets.c — the GPU_ACCEL_PLAN H3 texture packets, host-side.
 *
 * WHAT THIS TESTS AND WHY IT MUST NOT BE A MIRROR
 *
 * Three new packets reach a real device in H3: SAMPLER_VIEW, SAMPLER_STATE
 * and SET_SAMPLER_VIEWS.  A wrong length in any of them does not produce a
 * rejected packet — it produces a stream that decodes into the NEXT command,
 * which is why GL_PLAN G13's three wire-format constants had to be caught by
 * a test rather than by a device.
 *
 * So this file walks every packet the builders emit through the KERNEL's own
 * validator (`gpu_validate_cmd_stream`, the real kernel/gpu/gpu_cmdcheck.c
 * linked in), and independently re-derives each packet's length from the
 * header it wrote.  A builder that lies about its length fails here even
 * though the validator, on its own, would accept the stream — because the
 * validator only checks that the walk lands on the end, and a single packet
 * with a wrong length still lands there by accident.
 *
 * The negative cases matter as much as the positive ones: a sampler view
 * with last_level < first_level, a SET_SAMPLER_VIEWS claiming more views than
 * the resource quota allows, and one claiming a count with no array behind it
 * must all be refused by the BUILDER (returning -1) rather than producing a
 * stream the device has to survive.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "drivers/gpu/virgl.h"
#include "kernel/gpu/gpu_syscalls.h"

static int tn = 0, passed = 0, failed = 0;

#define CHECK(cond, ...) do {                                  \
    tn++;                                                      \
    if (cond) { passed++; printf("  ok   %d: ", tn); }          \
    else      { failed++; printf("  FAIL %d: ", tn); }          \
    printf(__VA_ARGS__);                                       \
    printf("\n");                                              \
} while (0)

/* Walk a stream the way the kernel does and report the packet count and
 * whether the walk landed exactly on the end.  Independent of the builder's
 * own bookkeeping on purpose: this is the check that a length lie cannot
 * hide behind a green validator. */
static int walk(const uint32_t *d, uint32_t n, uint32_t *seen_cmd,
                uint32_t *seen_obj, uint32_t *seen_len, int max_rec) {
    uint32_t i = 0;
    int rec = 0;
    while (i < n) {
        uint32_t hdr = d[i];
        uint32_t len = hdr & 0xFFFFu;
        if (rec < max_rec) {
            seen_cmd[rec] = hdr >> 24;
            seen_obj[rec] = (hdr >> 16) & 0xFFu;
            seen_len[rec] = len;
        }
        rec++;
        i += 1 + len;
        if (i > n) return -1;
    }
    return (i == n) ? rec : -1;
}

int main(void) {
    printf("=== test_tex_packets: the H3 texture packets vs the kernel "
           "validator ===\n\n");

    printf("--- SAMPLER_VIEW ---\n");
    {
        virgl_cmd_buf_t cb;
        virgl_cmd_init(&cb, 1);

        int rc = virgl_cmd_create_sampler_view(
            &cb, VIRGL_TEX_SAMPLER_VIEW, VIRGL_TEX_RES_BASE,
            VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM, 0, 4);
        CHECK(rc == 0, "the builder accepts a 5-level chain (0..4)");

        uint32_t cmd[8], obj[8], len[8];
        int rec = walk(cb.dwords, cb.count, cmd, obj, len, 8);
        CHECK(rec == 1, "the stream is exactly one packet (%d)", rec);
        CHECK(cmd[0] == VIRGL_CCMD_CREATE_OBJECT,
              "it is a CREATE_OBJECT (cmd %u)", cmd[0]);
        CHECK(obj[0] == VIRGL_OBJECT_SAMPLER_VIEW,
              "of object type SAMPLER_VIEW (%u)", obj[0]);
        CHECK(len[0] == 8, "and its payload is 8 dwords, as the source says "
              "(%u)", len[0]);
        CHECK(cb.count == 1 + len[0],
              "the buffer holds exactly header + payload (%u dwords)",
              cb.count);
        CHECK(gpu_validate_cmd_stream(cb.dwords, cb.count) == 0,
              "the KERNEL validator accepts it");

        /* The chain length survives the encoding: those two dwords are what
         * makes a mip chain one resource instead of one per level. */
        CHECK(cb.dwords[1 + 2] == VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM
              && cb.dwords[1 + 4] == 0 && cb.dwords[1 + 5] == 4,
              "format, first_level and last_level are where they belong");

        /* Negative: an inverted chain is refused by the BUILDER. */
        virgl_cmd_buf_t bad;
        virgl_cmd_init(&bad, 1);
        CHECK(virgl_cmd_create_sampler_view(
                  &bad, VIRGL_TEX_SAMPLER_VIEW, VIRGL_TEX_RES_BASE,
                  VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM, 4, 0) == -1,
              "last_level < first_level is refused, not encoded");
        CHECK(bad.count == 0, "and nothing was emitted");
    }

    printf("\n--- SAMPLER_STATE ---\n");
    {
        virgl_cmd_buf_t cb;
        virgl_cmd_init(&cb, 1);

        int rc = virgl_cmd_create_sampler_state(
            &cb, VIRGL_TEX_SAMPLER_STATE,
            VIRGL_TEX_WRAP_CLAMP, VIRGL_TEX_WRAP_CLAMP, VIRGL_TEX_WRAP_CLAMP,
            VIRGL_TEX_FILTER_NEAREST, VIRGL_TEX_FILTER_NEAREST,
            VIRGL_TEX_MIPFILTER_NONE);
        CHECK(rc == 0, "the builder accepts clamp/nearest");

        uint32_t cmd[8], obj[8], len[8];
        int rec = walk(cb.dwords, cb.count, cmd, obj, len, 8);
        CHECK(rec == 1 && obj[0] == VIRGL_OBJECT_SAMPLER_STATE,
              "one SAMPLER_STATE packet (%d)", rec);
        CHECK(len[0] == 8, "payload 8 dwords (%u)", len[0]);
        CHECK(gpu_validate_cmd_stream(cb.dwords, cb.count) == 0,
              "the KERNEL validator accepts it");

        /* The three wrap axes and three filters are legible where a reader
         * expects them — a shuffled field order would still validate and
         * would still draw, wrongly. */
        /* Layout after the header: [1] handle, [2] state handle (the same
         * value twice, as virglrenderer's struct carries it), then the six
         * state fields.  Off-by-one here is the difference between a
         * clamp/nearest texture and a repeat/linear one. */
        CHECK(cb.dwords[3] == VIRGL_TEX_WRAP_CLAMP
              && cb.dwords[4] == VIRGL_TEX_WRAP_CLAMP
              && cb.dwords[5] == VIRGL_TEX_WRAP_CLAMP,
              "wrap S/T/R in order");
        CHECK(cb.dwords[6] == VIRGL_TEX_FILTER_NEAREST
              && cb.dwords[7] == VIRGL_TEX_FILTER_NEAREST
              && cb.dwords[8] == VIRGL_TEX_MIPFILTER_NONE,
              "min/mag/mip filters in order");
    }

    printf("\n--- SET_SAMPLER_VIEWS ---\n");
    {
        virgl_cmd_buf_t cb;
        virgl_cmd_init(&cb, 1);

        const uint32_t views[2] = { VIRGL_TEX_SAMPLER_VIEW,
                                    VIRGL_TEX_SAMPLER_VIEW + 1 };
        int rc = virgl_cmd_set_sampler_views(
            &cb, VIRGL_PIPE_SHADER_FRAGMENT, 0, 2, views);
        CHECK(rc == 0, "the builder binds two views");

        uint32_t cmd[8], obj[8], len[8];
        int rec = walk(cb.dwords, cb.count, cmd, obj, len, 8);
        CHECK(rec == 1 && cmd[0] == VIRGL_CCMD_SET_SAMPLER_VIEWS,
              "one SET_SAMPLER_VIEWS packet (%d)", rec);
        CHECK(len[0] == 3 + 2, "payload is 3 + count dwords (%u)", len[0]);
        CHECK(cb.dwords[1] == VIRGL_PIPE_SHADER_FRAGMENT,
              "bound to the FRAGMENT stage");
        CHECK(cb.dwords[2] == 0 && cb.dwords[3] == 2,
              "start slot 0, count 2");
        CHECK(cb.dwords[4] == views[0] && cb.dwords[5] == views[1],
              "the handles are emitted in order");
        CHECK(gpu_validate_cmd_stream(cb.dwords, cb.count) == 0,
              "the KERNEL validator accepts it");

        /* The quota is enforced by the BUILDER, so an over-long view list
         * cannot reach a device at all. */
        virgl_cmd_buf_t over;
        virgl_cmd_init(&over, 1);
        uint32_t many[VIRGL_CMD_MAX_RESOURCES + 1];
        for (uint32_t i = 0; i < VIRGL_CMD_MAX_RESOURCES + 1; i++) many[i] = i;
        CHECK(virgl_cmd_set_sampler_views(
                  &over, VIRGL_PIPE_SHADER_FRAGMENT, 0,
                  VIRGL_CMD_MAX_RESOURCES + 1, many) == -1,
              "more views than the resource quota is refused");
        CHECK(over.count == 0, "and nothing was emitted");

        /* A count with no array behind it must not emit a packet whose
         * declared length runs past what was written. */
        virgl_cmd_buf_t nullbuf;
        virgl_cmd_init(&nullbuf, 1);
        CHECK(virgl_cmd_set_sampler_views(
                  &nullbuf, VIRGL_PIPE_SHADER_FRAGMENT, 0, 1, NULL) == -1,
              "count > 0 with a NULL handle array is refused");
        CHECK(nullbuf.count == 0, "and nothing was emitted");
    }

    printf("\n--- the three packets in ONE stream ---\n");
    {
        virgl_cmd_buf_t cb;
        virgl_cmd_init(&cb, 1);
        (void)virgl_cmd_create_sampler_view(
            &cb, VIRGL_TEX_SAMPLER_VIEW, VIRGL_TEX_RES_BASE,
            VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM, 0, 0);
        (void)virgl_cmd_create_sampler_state(
            &cb, VIRGL_TEX_SAMPLER_STATE,
            VIRGL_TEX_WRAP_REPEAT, VIRGL_TEX_WRAP_REPEAT, VIRGL_TEX_WRAP_REPEAT,
            VIRGL_TEX_FILTER_LINEAR, VIRGL_TEX_FILTER_LINEAR,
            VIRGL_TEX_MIPFILTER_LINEAR);
        const uint32_t one = VIRGL_TEX_SAMPLER_VIEW;
        (void)virgl_cmd_set_sampler_views(
            &cb, VIRGL_PIPE_SHADER_FRAGMENT, 0, 1, &one);

        uint32_t cmd[8], obj[8], len[8];
        int rec = walk(cb.dwords, cb.count, cmd, obj, len, 8);
        CHECK(rec == 3, "three packets, three walks (%d)", rec);
        CHECK(gpu_validate_cmd_stream(cb.dwords, cb.count) == 0,
              "the kernel validator accepts the combined stream");
        CHECK(cmd[0] == VIRGL_CCMD_CREATE_OBJECT
              && cmd[1] == VIRGL_CCMD_CREATE_OBJECT
              && cmd[2] == VIRGL_CCMD_SET_SAMPLER_VIEWS,
              "in the order a draw needs them");
    }

    printf("\n--- a deliberately corrupted stream is REJECTED ---\n");
    {
        /* The validator must still have teeth on these packet shapes: a
         * header claiming more payload than exists is the attack the
         * validator exists for, and it must fail for the new opcodes too. */
        virgl_cmd_buf_t cb;
        virgl_cmd_init(&cb, 1);
        (void)virgl_cmd_create_sampler_view(
            &cb, VIRGL_TEX_SAMPLER_VIEW, VIRGL_TEX_RES_BASE,
            VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM, 0, 0);
        CHECK(gpu_validate_cmd_stream(cb.dwords, cb.count) == 0, "as built: ok");

        /* Lie about the length: claim one more dword than was written. */
        cb.dwords[0] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT,
                                  VIRGL_OBJECT_SAMPLER_VIEW, 10);
        CHECK(gpu_validate_cmd_stream(cb.dwords, cb.count) == -1,
              "a SAMPLER_VIEW that overruns its payload is rejected");
    }

    printf("\ntest_tex_packets: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}
