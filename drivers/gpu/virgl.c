#include <stdint.h>
#include <stddef.h>
#include "drivers/gpu/virgl.h"
#include "drivers/gpu/virtio_gpu.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

static int virgl_ready;
static virgl_resource_info_t virgl_resources[VIRGL_CMD_MAX_RESOURCES];
static uint32_t default_rt_res = 64;
static uint32_t default_rt_surface = 65;

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

int virgl_init(void) {
    if (virgl_ready) return 0;
    if (!virtio_gpu_available() || !virtio_gpu_virgl_enabled()) return -1;
    if (!virtio_gpu_get_info()->ctx3d_ready) {
        if (virtio_gpu_ctx_create(VIRGL_CTX_DEFAULT, "AuraLiteVirGL") != 0) return -1;
    }
    virgl_ready = 1;
    kprintf("[virgl] command-stream transport ready (ctx=%u)\n", VIRGL_CTX_DEFAULT);
    return 0;
}

int virgl_available(void) { return virgl_ready || virgl_init() == 0; }

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

int virgl_submit_fenced(virgl_cmd_buf_t *cb, uint64_t *fence_id_out) {
    if (!cb || cb->overflow || cb->count == 0) return -1;
    if (!virgl_available()) return -1;
    return virtio_gpu_submit_3d_fenced(cb->ctx_id, cb->dwords,
                                       cb->count * sizeof(uint32_t), fence_id_out);
}

int virgl_submit(virgl_cmd_buf_t *cb) {
    return virgl_submit_fenced(cb, NULL);
}

static virgl_resource_info_t *virgl_resource_slot(uint32_t resource_id) {
    virgl_resource_info_t *free_slot = NULL;
    for (uint32_t i = 0; i < VIRGL_CMD_MAX_RESOURCES; i++) {
        if (virgl_resources[i].in_use && virgl_resources[i].id == resource_id) return &virgl_resources[i];
        if (!virgl_resources[i].in_use && !free_slot) free_slot = &virgl_resources[i];
    }
    return free_slot;
}

const virgl_resource_info_t *virgl_resource_lookup(uint32_t resource_id) {
    for (uint32_t i = 0; i < VIRGL_CMD_MAX_RESOURCES; i++) {
        if (virgl_resources[i].in_use && virgl_resources[i].id == resource_id) return &virgl_resources[i];
    }
    return NULL;
}

int virgl_resource_create(uint32_t resource_id, uint32_t target, uint32_t format,
                          uint32_t bind, uint32_t width, uint32_t height,
                          uint32_t depth, uint32_t array_size, uint32_t last_level,
                          uint32_t nr_samples, uint32_t flags) {
    if (!virgl_available() || resource_id == 0) return -1;
    if (virtio_gpu_resource_create_3d(resource_id, VIRGL_CTX_DEFAULT, target, format, bind,
                                      width, height, depth, array_size, last_level,
                                      nr_samples, flags) != 0) return -1;
    virgl_resource_info_t *r = virgl_resource_slot(resource_id);
    if (r) {
        memset(r, 0, sizeof(*r));
        r->id = resource_id; r->width = width; r->height = height; r->depth = depth;
        r->format = format; r->bind = bind; r->target = target; r->in_use = 1;
    }
    return 0;
}

int virgl_resource_attach(uint32_t resource_id) {
    if (!virgl_available() || resource_id == 0) return -1;
    int rc = virtio_gpu_ctx_attach_resource(VIRGL_CTX_DEFAULT, resource_id);
    if (rc == 0) {
        virgl_resource_info_t *r = virgl_resource_slot(resource_id);
        if (r) { r->id = resource_id; r->attached = 1; r->in_use = 1; }
    }
    return rc;
}

int virgl_resource_detach(uint32_t resource_id) {
    if (!virgl_available() || resource_id == 0) return -1;
    int rc = virtio_gpu_ctx_detach_resource(VIRGL_CTX_DEFAULT, resource_id);
    if (rc == 0) {
        virgl_resource_info_t *r = (virgl_resource_info_t *)virgl_resource_lookup(resource_id);
        if (r) r->attached = 0;
    }
    return rc;
}

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

int virgl_create_scanout_render_target(uint32_t resource_id, uint32_t width, uint32_t height) {
    if (!virgl_available()) return -1;
    if (virgl_resource_create(resource_id, VIRGL_PIPE_TEXTURE_2D,
                              VIRGL_PIPE_FORMAT_B8G8R8X8_UNORM,
                              VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                              width, height, 1, 1, 0, 0, 0) != 0) {
        return -1;
    }
    if (virgl_resource_attach(resource_id) != 0) return -1;

    virgl_cmd_buf_t cb;
    virgl_cmd_init(&cb, VIRGL_CTX_DEFAULT);
    virgl_cmd_create_surface(&cb, default_rt_surface, resource_id,
                             VIRGL_PIPE_FORMAT_B8G8R8X8_UNORM, 0, 0, 0);
    virgl_cmd_set_framebuffer_1(&cb, width, height, 1, default_rt_surface, 0);
    return virgl_submit(&cb);
}

int virgl_present_render_target(uint32_t resource_id, uint32_t width, uint32_t height) {
    if (!virgl_available() || resource_id == 0 || width == 0 || height == 0) return -1;
    /* The default render target is a B8G8R8X8 2D texture: 4 bytes per pixel. */
    uint32_t stride = width * 4u;
    if (virtio_gpu_transfer_to_host_3d(VIRGL_CTX_DEFAULT, resource_id,
                                       0, 0, 0, width, height, 1,
                                       0, 0, stride, stride * height) != 0) {
        kprintf("[virgl] present: transfer-to-host-3d failed\n");
        return -1;
    }
    if (virtio_gpu_set_scanout_resource(0, resource_id, 0, 0, width, height) != 0) {
        kprintf("[virgl] present: set-scanout failed\n");
        return -1;
    }
    if (virtio_gpu_flush_resource(resource_id, 0, 0, width, height) != 0) {
        kprintf("[virgl] present: resource-flush failed\n");
        return -1;
    }
    kprintf("[virgl] presented render target res=%u to scanout 0 (%ux%u)\n",
            resource_id, width, height);
    return 0;
}

int virgl_clear_screen(float r, float g, float b, float a) {
    if (!virgl_available()) return -1;
    virgl_cmd_buf_t cb;
    virgl_cmd_init(&cb, VIRGL_CTX_DEFAULT);
    virgl_cmd_clear(&cb, VIRGL_PIPE_CLEAR_COLOR0, r, g, b, a, 1.0, 0);
    return virgl_submit(&cb);
}

int virgl_demo_submit_clear(void) {
    const virtio_gpu_info_t *info = virtio_gpu_get_info();
    uint32_t w = info->width ? info->width : 640;
    uint32_t h = info->height ? info->height : 480;
    if (!virgl_available()) return -1;
    if (virgl_create_scanout_render_target(default_rt_res, w, h) != 0) {
        kprintf("[virgl] render-target setup failed\n");
        return -1;
    }
    if (virgl_clear_screen(0.08f, 0.12f, 0.28f, 1.0f) != 0) {
        kprintf("[virgl] clear submit failed\n");
        return -1;
    }
    (void)virgl_present_render_target(default_rt_res, w, h);
    kprintf("[virgl] submitted experimental clear to 3D context\n");
    return 0;
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

/* Tiny placeholder TGSI token streams.  They are deliberately isolated here:
 * if a particular virglrenderer version rejects them, the transport remains
 * usable and the boot log will show which step failed. */
static const uint32_t tiny_vs_tgsi[] = {
    0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u,
};
static const uint32_t tiny_fs_tgsi[] = {
    0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

typedef struct virgl_demo_vertex {
    float px, py, pz;
    float r, g, b, a;
} virgl_demo_vertex_t;

int virgl_demo_submit_triangle(void) {
    const virtio_gpu_info_t *info = virtio_gpu_get_info();
    uint32_t w = info->width ? info->width : 640;
    uint32_t h = info->height ? info->height : 480;
    if (!virgl_available()) return -1;

    if (virgl_create_scanout_render_target(default_rt_res, w, h) != 0) {
        kprintf("[virgl] triangle: render-target setup failed\n");
        return -1;
    }
    if (virgl_resource_create(VIRGL_VERTEX_RES, 0, VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM,
                              VIRGL_BIND_VERTEX_BUFFER,
                              sizeof(virgl_demo_vertex_t) * 3, 1, 1,
                              1, 0, 0, 0) != 0) {
        kprintf("[virgl] triangle: vertex resource create failed\n");
        return -1;
    }
    (void)virgl_resource_attach(VIRGL_VERTEX_RES);

    virgl_demo_vertex_t verts[3] = {
        { -0.65f, -0.55f, 0.0f, 1.0f, 0.1f, 0.1f, 1.0f },
        {  0.65f, -0.55f, 0.0f, 0.1f, 1.0f, 0.1f, 1.0f },
        {  0.00f,  0.65f, 0.0f, 0.1f, 0.3f, 1.0f, 1.0f },
    };

    virgl_cmd_buf_t cb;
    virgl_cmd_init(&cb, VIRGL_CTX_DEFAULT);
    virgl_cmd_inline_write(&cb, VIRGL_VERTEX_RES, 0, VIRGL_BIND_VERTEX_BUFFER,
                           sizeof(virgl_demo_vertex_t), 0, 0, 0, 0,
                           sizeof(verts), 1, 1, verts, sizeof(verts));
    virgl_cmd_create_blend_basic(&cb, VIRGL_BLEND_HANDLE);
    virgl_cmd_create_rasterizer_basic(&cb, VIRGL_RASTER_HANDLE);
    virgl_cmd_create_dsa_basic(&cb, VIRGL_DSA_HANDLE);
    virgl_cmd_create_vertex_elements_pos_color(&cb, VIRGL_VELEM_HANDLE);
    virgl_cmd_create_shader_tgsi(&cb, VIRGL_SHADER_HANDLE_VS, VIRGL_PIPE_SHADER_VERTEX,
                                 tiny_vs_tgsi, sizeof(tiny_vs_tgsi)/sizeof(tiny_vs_tgsi[0]));
    virgl_cmd_create_shader_tgsi(&cb, VIRGL_SHADER_HANDLE_FS, VIRGL_PIPE_SHADER_FRAGMENT,
                                 tiny_fs_tgsi, sizeof(tiny_fs_tgsi)/sizeof(tiny_fs_tgsi[0]));
    virgl_cmd_bind_object(&cb, VIRGL_OBJECT_BLEND, VIRGL_BLEND_HANDLE);
    virgl_cmd_bind_object(&cb, VIRGL_OBJECT_RASTERIZER, VIRGL_RASTER_HANDLE);
    virgl_cmd_bind_object(&cb, VIRGL_OBJECT_DSA, VIRGL_DSA_HANDLE);
    virgl_cmd_bind_object(&cb, VIRGL_OBJECT_VERTEX_ELEMENTS, VIRGL_VELEM_HANDLE);
    virgl_cmd_bind_object(&cb, VIRGL_OBJECT_SHADER, VIRGL_SHADER_HANDLE_VS);
    virgl_cmd_bind_object(&cb, VIRGL_OBJECT_SHADER, VIRGL_SHADER_HANDLE_FS);
    virgl_cmd_set_vertex_buffer_1(&cb, VIRGL_VERTEX_RES, sizeof(virgl_demo_vertex_t), 0);
    virgl_cmd_set_viewport(&cb, (float)w * 0.5f, (float)h * 0.5f, 1.0f,
                           (float)w * 0.5f, (float)h * 0.5f, 0.0f);
    virgl_cmd_clear(&cb, VIRGL_PIPE_CLEAR_COLOR0, 0.02f, 0.02f, 0.05f, 1.0f, 1.0, 0);
    virgl_cmd_draw_vbo(&cb, VIRGL_PIPE_PRIM_TRIANGLES, 0, 3, 0);
    uint64_t fence = 0;
    if (virgl_submit_fenced(&cb, &fence) != 0) {
        kprintf("[virgl] triangle submit failed\n");
        return -1;
    }
    (void)virgl_present_render_target(default_rt_res, w, h);
    kprintf("[virgl] submitted experimental triangle command stream fence=%llu\n",
            (unsigned long long)fence);
    return 0;
}
