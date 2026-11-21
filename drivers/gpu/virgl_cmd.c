/* drivers/gpu/virgl_cmd.c — the VirGL wire format, and NOTHING else.
 *
 * Split out of virgl.c during GPU_ACCEL_PLAN H3, for one structural reason:
 * the command BUILDERS are pure functions over a dword buffer, but they used
 * to live in the same translation unit as the resource table and the
 * virtio-gpu transport.  That made them linkable only into the kernel, so
 * libgl's backend had to restate the encoding -- and H3 is exactly the phase
 * where two copies of "how many dwords follows this header" start to drift.
 *
 * The G13 lesson (one definition of the wire format in the tree) is what this
 * file is: libgl links it, the kernel links it, and tests/unit walks its
 * output through the kernel's own validator.  Anything with a transport
 * dependency stays in virgl.c.
 */

#include <stdint.h>
#include <stddef.h>

#include "drivers/gpu/virgl.h"
#include "kernel/lib/string.h"

/* Bit-exact float/double encoding: the wire wants the IEEE pattern, and a
 * float->int cast would be a value conversion with rounding. */
static uint32_t f2u(float f) {
    union { float f; uint32_t u; } v;
    v.f = f;
    return v.u;
}

static void double_to_u32(double d, uint32_t *lo, uint32_t *hi) {
    union { double d; uint64_t u; } v;
    v.d = d;
    *lo = (uint32_t)v.u;
    *hi = (uint32_t)(v.u >> 32);
}


void virgl_cmd_init(virgl_cmd_buf_t *cb, uint32_t ctx_id) {
    if (!cb) return;
    memset(cb, 0, sizeof(*cb));
    cb->ctx_id = ctx_id ? ctx_id : VIRGL_CTX_DEFAULT;
}

int virgl_emit(virgl_cmd_buf_t *cb, uint32_t dw) {
    if (!cb || cb->count >= VIRGL_CMD_MAX_DWORDS) {
        if (cb) cb->overflow = 1;
        return -1;
    }
    cb->dwords[cb->count++] = dw;
    return 0;
}

int virgl_emit_float(virgl_cmd_buf_t *cb, float f) { return virgl_emit(cb, f2u(f)); }

int virgl_cmd_clear(virgl_cmd_buf_t *cb, uint32_t buffers,
                    float r, float g, float b, float a,
                    double depth, uint32_t stencil) {
    uint32_t dlo, dhi;
    double_to_u32(depth, &dlo, &dhi);
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8));
    virgl_emit(cb, buffers);
    virgl_emit_float(cb, r);
    virgl_emit_float(cb, g);
    virgl_emit_float(cb, b);
    virgl_emit_float(cb, a);
    virgl_emit(cb, dlo);
    virgl_emit(cb, dhi);
    return virgl_emit(cb, stencil);
}

int virgl_cmd_create_surface(virgl_cmd_buf_t *cb, uint32_t handle,
                             uint32_t resource_id, uint32_t format,
                             uint32_t first_layer, uint32_t last_layer,
                             uint32_t level) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 6));
    virgl_emit(cb, handle);
    virgl_emit(cb, resource_id);
    virgl_emit(cb, format);
    virgl_emit(cb, first_layer);
    virgl_emit(cb, last_layer);
    return virgl_emit(cb, level);
}

int virgl_cmd_set_framebuffer_1(virgl_cmd_buf_t *cb, uint32_t width, uint32_t height,
                                uint32_t nr_cbufs, uint32_t cbuf_handle,
                                uint32_t zsurf_handle) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 4 + nr_cbufs));
    virgl_emit(cb, nr_cbufs);
    virgl_emit(cb, zsurf_handle);
    virgl_emit(cb, width);
    virgl_emit(cb, height);
    if (nr_cbufs) return virgl_emit(cb, cbuf_handle);
    return 0;
}

int virgl_cmd_set_viewport(virgl_cmd_buf_t *cb, float sx, float sy, float sz,
                           float tx, float ty, float tz) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    virgl_emit(cb, 0); /* start slot */
    virgl_emit_float(cb, sx);
    virgl_emit_float(cb, sy);
    virgl_emit_float(cb, sz);
    virgl_emit_float(cb, tx);
    virgl_emit_float(cb, ty);
    return virgl_emit_float(cb, tz);
}

int virgl_cmd_bind_object(virgl_cmd_buf_t *cb, uint32_t object_type, uint32_t handle) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, object_type, 1));
    return virgl_emit(cb, handle);
}

int virgl_cmd_draw_vbo(virgl_cmd_buf_t *cb, uint32_t mode, uint32_t start,
                       uint32_t count, uint32_t indexed) {
    /* Common virgl DRAW_VBO packet subset.  Extra fields are zeroed/default. */
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO, 0, 12));
    virgl_emit(cb, start);
    virgl_emit(cb, count);
    virgl_emit(cb, mode);
    virgl_emit(cb, indexed);
    virgl_emit(cb, 1);      /* instance_count */
    virgl_emit(cb, 0);      /* index_bias */
    virgl_emit(cb, 0);      /* start_instance */
    virgl_emit(cb, 0);      /* primitive_restart */
    virgl_emit(cb, 0);      /* restart_index */
    virgl_emit(cb, 0);      /* min_index */
    virgl_emit(cb, count ? count - 1 : 0); /* max_index */
    return virgl_emit(cb, 0);      /* cso/indirect handle */
}

int virgl_cmd_create_blend_basic(virgl_cmd_buf_t *cb, uint32_t handle) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 8));
    virgl_emit(cb, handle);
    for (int i = 0; i < 7; i++) virgl_emit(cb, 0);
    return 0;
}

int virgl_cmd_create_rasterizer_basic(virgl_cmd_buf_t *cb, uint32_t handle) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 10));
    virgl_emit(cb, handle);
    virgl_emit(cb, 0); /* flatshade/cull/fill defaults */
    virgl_emit(cb, 0);
    virgl_emit(cb, 0);
    virgl_emit_float(cb, 1.0f); /* line width */
    virgl_emit_float(cb, 1.0f); /* point size */
    virgl_emit(cb, 0);
    virgl_emit(cb, 0);
    virgl_emit(cb, 0);
    return virgl_emit(cb, 0);
}

int virgl_cmd_create_dsa_basic(virgl_cmd_buf_t *cb, uint32_t handle) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 6));
    virgl_emit(cb, handle);
    for (int i = 0; i < 5; i++) virgl_emit(cb, 0);
    return 0;
}

int virgl_cmd_create_vertex_elements_pos_color(virgl_cmd_buf_t *cb, uint32_t handle) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 9));
    virgl_emit(cb, handle);
    virgl_emit(cb, 2); /* elements */
    /* element 0: vec3 position at offset 0 */
    virgl_emit(cb, 0); /* src_offset */
    virgl_emit(cb, VIRGL_PIPE_FORMAT_R32G32B32_FLOAT);
    virgl_emit(cb, 0); /* vertex_buffer_index */
    virgl_emit(cb, 0); /* instance divisor */
    /* element 1: vec4 color after position */
    virgl_emit(cb, 12);
    virgl_emit(cb, VIRGL_PIPE_FORMAT_R32G32B32A32_FLOAT);
    return virgl_emit(cb, 0);
}

int virgl_cmd_create_shader_tgsi(virgl_cmd_buf_t *cb, uint32_t handle,
                                uint32_t shader_type, const uint32_t *tokens,
                                uint32_t token_count) {
    if (!tokens || token_count == 0) return -1;
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 4 + token_count));
    virgl_emit(cb, handle);
    virgl_emit(cb, shader_type);
    virgl_emit(cb, token_count);
    virgl_emit(cb, 0); /* stream output info dwords */
    for (uint32_t i = 0; i < token_count; i++) virgl_emit(cb, tokens[i]);
    return cb->overflow ? -1 : 0;
}

int virgl_cmd_set_vertex_buffer_1(virgl_cmd_buf_t *cb, uint32_t resource_id,
                                  uint32_t stride, uint32_t offset) {
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 5));
    virgl_emit(cb, 0); /* start slot */
    virgl_emit(cb, 1); /* count */
    virgl_emit(cb, stride);
    virgl_emit(cb, offset);
    return virgl_emit(cb, resource_id);
}

int virgl_cmd_create_sampler_view(virgl_cmd_buf_t *cb, uint32_t handle,
                                  uint32_t resource_id, uint32_t format,
                                  uint32_t first_level, uint32_t last_level) {
    if (!cb) return -1;
    if (last_level < first_level) return -1;   /* a negative chain length */

    /* Length is derived from the emits below, not typed twice: 8 payload
     * dwords after the header.  The first draft said 9 and this test caught
     * it -- exactly the length-lie class the file's header warns about. */
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT,
                              VIRGL_OBJECT_SAMPLER_VIEW, 8));
    virgl_emit(cb, handle);
    virgl_emit(cb, resource_id);
    virgl_emit(cb, format);
    virgl_emit(cb, 0);              /* texture_2d: no texel offset        */
    virgl_emit(cb, first_level);
    virgl_emit(cb, last_level);
    virgl_emit(cb, 0);              /* swizzle r/g/b/a packed = identity  */
    virgl_emit(cb, 0);              /* padding: struct alignment          */
    return cb->overflow ? -1 : 0;
}

int virgl_cmd_create_sampler_state(virgl_cmd_buf_t *cb, uint32_t handle,
                                   uint32_t wrap_s, uint32_t wrap_t,
                                   uint32_t wrap_r, uint32_t min_filter,
                                   uint32_t mag_filter, uint32_t mip_filter) {
    if (!cb) return -1;

    /* 8 payload dwords after the header (see the SAMPLER_VIEW note above:
     * the count is counted, not typed twice). */
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT,
                              VIRGL_OBJECT_SAMPLER_STATE, 8));
    virgl_emit(cb, handle);
    virgl_emit(cb, handle);         /* state = handle, as virglrenderer expects */
    virgl_emit(cb, wrap_s);
    virgl_emit(cb, wrap_t);
    virgl_emit(cb, wrap_r);
    virgl_emit(cb, min_filter);
    virgl_emit(cb, mag_filter);
    virgl_emit(cb, mip_filter);
    return cb->overflow ? -1 : 0;
}

int virgl_cmd_set_sampler_views(virgl_cmd_buf_t *cb, uint32_t shader_type,
                                uint32_t start_slot, uint32_t count,
                                const uint32_t *view_handles) {
    if (!cb) return -1;
    if (count > VIRGL_CMD_MAX_RESOURCES) return -1;   /* the quota, enforced */
    if (count > 0 && !view_handles) return -1;

    /* Payload: shader type, start slot, count, then one handle per view. */
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3 + count));
    virgl_emit(cb, shader_type);
    virgl_emit(cb, start_slot);
    virgl_emit(cb, count);
    for (uint32_t i = 0; i < count; i++) virgl_emit(cb, view_handles[i]);
    return cb->overflow ? -1 : 0;
}

int virgl_cmd_inline_write(virgl_cmd_buf_t *cb, uint32_t resource_id,
                           uint32_t level, uint32_t usage, uint32_t stride,
                           uint32_t layer_stride, uint32_t x, uint32_t y,
                           uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                           const void *data, uint32_t bytes) {
    if (!data || bytes == 0) return -1;
    uint32_t payload_dw = (bytes + 3u) / 4u;
    virgl_emit(cb, VIRGL_CMD0(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, 12 + payload_dw));
    virgl_emit(cb, resource_id);
    virgl_emit(cb, level);
    virgl_emit(cb, usage);
    virgl_emit(cb, stride);
    virgl_emit(cb, layer_stride);
    virgl_emit(cb, x); virgl_emit(cb, y); virgl_emit(cb, z);
    virgl_emit(cb, w); virgl_emit(cb, h); virgl_emit(cb, d);
    virgl_emit(cb, bytes);
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < payload_dw; i++) {
        uint32_t dw = 0;
        for (uint32_t b = 0; b < 4; b++) {
            uint32_t idx = i * 4 + b;
            if (idx < bytes) dw |= ((uint32_t)p[idx]) << (8 * b);
        }
        virgl_emit(cb, dw);
    }
    return cb->overflow ? -1 : 0;
}
