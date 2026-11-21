#include <stdint.h>
#include <stddef.h>
#include "drivers/gpu/virgl.h"

/* The packet builders live in virgl_cmd.c: they are pure dword encoders and
 * libgl links them too, so the wire format has exactly one definition.  What
 * remains here needs the resource table or the virtio-gpu transport. */
#include "drivers/gpu/virtio_gpu.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

static int virgl_ready;
static virgl_resource_info_t virgl_resources[VIRGL_CMD_MAX_RESOURCES];
static uint32_t default_rt_res = 64;
static uint32_t default_rt_surface = 65;



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







/* ===========================================================================
 * GPU_ACCEL_PLAN H3: texture objects on the device.
 *
 * Packet shapes here are virglrenderer's, and every one is walked by the
 * kernel's own validator in tests/unit/test_glvirgl.c before it may reach a
 * device — the G13 rule ("one definition of the wire format in the tree")
 * applied to the three packets a textured draw needs.
 * ===========================================================================*/

/* VIRGL_OBJECT_SAMPLER_VIEW (object type 6).
 *
 * Payload: handle, then res_handle, format, then the five view fields.
 * `first_level`/`last_level` are what makes a mip chain ONE resource: the
 * device derives the level count from the pair, so a 5-level chain is one
 * CREATE + one VIEW rather than five of each.
 *
 * Length is 9 dwords: handle, resource, format, 5 view fields, and the
 * padding dword virglrenderer's struct carries.  A wrong length here is not
 * a rejected packet — it is a stream that decodes into the NEXT command,
 * which is why the count is pinned by a host test rather than trusted. */

/* VIRGL_OBJECT_SAMPLER_STATE (object type 7).
 *
 * Wrap modes and the three filters in one packet.  The caller validates the
 * enum values; this emits what it is given, because clamping state silently
 * is how a texture renders plausibly wrong — the outcome GL2 L6's comment
 * calls "the one thing this seam must never produce". */

/* VIRGL_CCMD_SET_SAMPLER_VIEWS (command 15).
 *
 * Binds `count` sampler-view handles to one shader stage from `start_slot`.
 * The handle array is emitted inline, so the packet length scales with the
 * count and a stream that claims more views than it carries is rejected by
 * the validator's walk rather than by a device. */


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
