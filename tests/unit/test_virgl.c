/*
 * test_virgl.c — host unit tests for the VirGL command-stream encoding.
 *
 * The kernel virgl.c builder cannot be linked directly here (it pulls in the
 * virtio-gpu transport and kernel headers), so this test re-implements the same
 * tiny emit/packet helpers and validates that the wire encoding contract — the
 * VIRGL_CMD0 opcode/object/length packing and the dword payload layout of the
 * CLEAR and DRAW_VBO packets — matches what virgl.c emits.  If virgl.c changes
 * the layout, these expectations must change with it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/virgl.h"

static int passed = 0, failed = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } else { passed++; } } while (0)

/* Mirror of virgl.c's command buffer + emit primitives. */
typedef struct {
    uint32_t dwords[VIRGL_CMD_MAX_DWORDS];
    uint32_t count;
    int overflow;
} cb_t;

static void cb_init(cb_t *cb) { memset(cb, 0, sizeof(*cb)); }
static int emit(cb_t *cb, uint32_t dw) {
    if (cb->count >= VIRGL_CMD_MAX_DWORDS) { cb->overflow = 1; return -1; }
    cb->dwords[cb->count++] = dw;
    return 0;
}
static uint32_t f2u(float f) { union { float f; uint32_t u; } v; v.f = f; return v.u; }
static int emit_f(cb_t *cb, float f) { return emit(cb, f2u(f)); }

/* ---- VIRGL_CMD0 packing ---- */
static void t_cmd0_packing(void) {
    uint32_t h = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    CHECK(((h >> 24) & 0xff) == VIRGL_CCMD_CLEAR);
    CHECK(((h >> 16) & 0xff) == 0);
    CHECK((h & 0xffff) == 8);

    uint32_t h2 = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 6);
    CHECK(((h2 >> 24) & 0xff) == VIRGL_CCMD_CREATE_OBJECT);
    CHECK(((h2 >> 16) & 0xff) == VIRGL_OBJECT_SURFACE);
    CHECK((h2 & 0xffff) == 6);
}

/* ---- CLEAR packet ---- */
static void double_to_u32(double d, uint32_t *lo, uint32_t *hi) {
    union { double d; uint64_t u; } v; v.d = d;
    *lo = (uint32_t)v.u; *hi = (uint32_t)(v.u >> 32);
}
static void t_clear_packet(void) {
    cb_t cb; cb_init(&cb);
    uint32_t dlo, dhi; double_to_u32(1.0, &dlo, &dhi);
    emit(&cb, VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8));
    emit(&cb, VIRGL_PIPE_CLEAR_COLOR0);
    emit_f(&cb, 0.25f); emit_f(&cb, 0.5f); emit_f(&cb, 0.75f); emit_f(&cb, 1.0f);
    emit(&cb, dlo); emit(&cb, dhi);
    emit(&cb, 0);

    /* header + 8 payload dwords */
    CHECK(cb.count == 9);
    CHECK((cb.dwords[0] & 0xffff) == 8);
    CHECK(cb.dwords[1] == VIRGL_PIPE_CLEAR_COLOR0);
    CHECK(cb.dwords[2] == f2u(0.25f));
    CHECK(cb.dwords[5] == f2u(1.0f));
    CHECK(cb.dwords[8] == 0);
    CHECK(cb.overflow == 0);
}

/* ---- DRAW_VBO packet ---- */
static void t_draw_packet(void) {
    cb_t cb; cb_init(&cb);
    uint32_t count = 3;
    emit(&cb, VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO, 0, 12));
    emit(&cb, 0);            /* start */
    emit(&cb, count);        /* count */
    emit(&cb, VIRGL_PIPE_PRIM_TRIANGLES);
    emit(&cb, 0);            /* indexed */
    emit(&cb, 1);            /* instance_count */
    for (int i = 0; i < 5; i++) emit(&cb, 0);
    emit(&cb, count - 1);    /* max_index */
    emit(&cb, 0);            /* cso/indirect */

    CHECK(cb.count == 13);   /* header + 12 */
    CHECK(((cb.dwords[0] >> 24) & 0xff) == VIRGL_CCMD_DRAW_VBO);
    CHECK(cb.dwords[2] == count);
    CHECK(cb.dwords[3] == VIRGL_PIPE_PRIM_TRIANGLES);
    CHECK(cb.dwords[11] == count - 1);
}

/* ---- overflow guard ---- */
static void t_overflow_guard(void) {
    cb_t cb; cb_init(&cb);
    for (uint32_t i = 0; i < VIRGL_CMD_MAX_DWORDS + 8; i++) emit(&cb, i);
    CHECK(cb.overflow == 1);
    CHECK(cb.count == VIRGL_CMD_MAX_DWORDS);
}

int main(void) {
    printf("=== VirGL command-stream encoding tests ===\n\n");
    t_cmd0_packing();
    t_clear_packet();
    t_draw_packet();
    t_overflow_guard();
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
