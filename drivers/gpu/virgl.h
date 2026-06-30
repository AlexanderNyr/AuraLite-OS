#ifndef AURALITE_DRIVERS_GPU_VIRGL_H
#define AURALITE_DRIVERS_GPU_VIRGL_H

#include <stdint.h>
#include <stddef.h>

/*
 * Tiny VirGL command-stream helper layer.
 *
 * This is intentionally low-level: it builds virglrenderer/Gallium command
 * streams and submits them through drivers/gpu/virtio_gpu.c.  It is enough to
 * create the first higher-level 3D API experiments without putting raw dword
 * packets all over the renderer.
 */

#define VIRGL_CTX_DEFAULT 1u
#define VIRGL_CMD_MAX_DWORDS 1024u

/* VirGL command opcodes (virglrenderer protocol). */
#define VIRGL_CCMD_CREATE_OBJECT       1u
#define VIRGL_CCMD_BIND_OBJECT         2u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 3u
#define VIRGL_CCMD_CLEAR               4u
#define VIRGL_CCMD_DRAW_VBO            5u
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE 6u
#define VIRGL_CCMD_SET_VERTEX_BUFFERS  7u
#define VIRGL_CCMD_SET_INDEX_BUFFER    8u
#define VIRGL_CCMD_SET_CONSTANT_BUFFER 9u
#define VIRGL_CCMD_SET_STENCIL_REF     10u
#define VIRGL_CCMD_SET_BLEND_COLOR     11u
#define VIRGL_CCMD_SET_SCISSOR_STATE   12u
#define VIRGL_CCMD_BLIT                13u
#define VIRGL_CCMD_SET_VIEWPORT_STATE  14u
#define VIRGL_CCMD_SET_SAMPLER_VIEWS   15u
#define VIRGL_CCMD_SET_SAMPLER_STATES  16u
#define VIRGL_CCMD_SET_VERTEX_ELEMENTS 17u
#define VIRGL_CCMD_SET_ALPHA_REF       18u
#define VIRGL_CCMD_SET_CLIP_STATE      19u
#define VIRGL_CCMD_SET_SHADER_BUFFERS  20u
#define VIRGL_CCMD_SET_SHADER_IMAGES   21u
#define VIRGL_CCMD_MEMORY_BARRIER      22u
#define VIRGL_CCMD_LAUNCH_GRID         23u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE_NO_ATTACH 24u

/* Object types. */
#define VIRGL_OBJECT_BLEND             1u
#define VIRGL_OBJECT_RASTERIZER        2u
#define VIRGL_OBJECT_DSA               3u
#define VIRGL_OBJECT_SHADER            4u
#define VIRGL_OBJECT_VERTEX_ELEMENTS   5u
#define VIRGL_OBJECT_SAMPLER_VIEW      6u
#define VIRGL_OBJECT_SAMPLER_STATE     7u
#define VIRGL_OBJECT_SURFACE           8u
#define VIRGL_OBJECT_QUERY             9u
#define VIRGL_OBJECT_STREAMOUT_TARGET  10u
#define VIRGL_OBJECT_GRID              11u

/* Gallium-ish constants used by the helpers. */
#define VIRGL_PIPE_CLEAR_COLOR0        0x00000001u
#define VIRGL_PIPE_CLEAR_DEPTH         0x00000002u
#define VIRGL_PIPE_CLEAR_STENCIL       0x00000004u
#define VIRGL_PIPE_SHADER_VERTEX       0u
#define VIRGL_PIPE_SHADER_FRAGMENT     1u
#define VIRGL_PIPE_SHADER_COMPUTE      5u
#define VIRGL_PIPE_PRIM_TRIANGLES      4u
#define VIRGL_PIPE_FORMAT_B8G8R8X8_UNORM 2u
#define VIRGL_PIPE_TEXTURE_2D          2u
#define VIRGL_BIND_RENDER_TARGET       0x00000010u
#define VIRGL_BIND_SAMPLER_VIEW        0x00000020u
#define VIRGL_BIND_VERTEX_BUFFER       0x00000004u
#define VIRGL_BIND_INDEX_BUFFER        0x00000008u
#define VIRGL_BIND_CUSTOM              0x00000040u

#define VIRGL_PIPE_FORMAT_R32G32B32_FLOAT 31u
#define VIRGL_PIPE_FORMAT_R32G32B32A32_FLOAT 32u
#define VIRGL_PIPE_FORMAT_R8G8B8A8_UNORM 67u

#define VIRGL_SHADER_HANDLE_VS 101u
#define VIRGL_SHADER_HANDLE_FS 102u
#define VIRGL_BLEND_HANDLE     103u
#define VIRGL_RASTER_HANDLE    104u
#define VIRGL_DSA_HANDLE       105u
#define VIRGL_VELEM_HANDLE     106u
#define VIRGL_VERTEX_RES       107u

#define VIRGL_CMD0(cmd, obj, len) ((((cmd) & 0xffu) << 24) | (((obj) & 0xffu) << 16) | ((len) & 0xffffu))

#define VIRGL_CMD_MAX_RESOURCES 16u

typedef struct virgl_resource_info {
    uint32_t id;
    uint32_t width, height, depth;
    uint32_t format, bind, target;
    int attached;
    int in_use;
} virgl_resource_info_t;

typedef struct virgl_cmd_buf {
    uint32_t dwords[VIRGL_CMD_MAX_DWORDS];
    uint32_t count;
    uint32_t ctx_id;
    int overflow;
} virgl_cmd_buf_t;

int  virgl_init(void);
int  virgl_available(void);
void virgl_cmd_init(virgl_cmd_buf_t *cb, uint32_t ctx_id);
int  virgl_emit(virgl_cmd_buf_t *cb, uint32_t dw);
int  virgl_emit_float(virgl_cmd_buf_t *cb, float f);
int  virgl_submit(virgl_cmd_buf_t *cb);
int  virgl_submit_fenced(virgl_cmd_buf_t *cb, uint64_t *fence_id_out);

int virgl_resource_create(uint32_t resource_id, uint32_t target, uint32_t format,
                          uint32_t bind, uint32_t width, uint32_t height,
                          uint32_t depth, uint32_t array_size, uint32_t last_level,
                          uint32_t nr_samples, uint32_t flags);
int virgl_resource_attach(uint32_t resource_id);
int virgl_resource_detach(uint32_t resource_id);
const virgl_resource_info_t *virgl_resource_lookup(uint32_t resource_id);

/* Convenience packet builders. */
int virgl_cmd_clear(virgl_cmd_buf_t *cb, uint32_t buffers,
                    float r, float g, float b, float a,
                    double depth, uint32_t stencil);
int virgl_cmd_create_surface(virgl_cmd_buf_t *cb, uint32_t handle,
                             uint32_t resource_id, uint32_t format,
                             uint32_t first_layer, uint32_t last_layer,
                             uint32_t level);
int virgl_cmd_set_framebuffer_1(virgl_cmd_buf_t *cb, uint32_t width, uint32_t height,
                                uint32_t nr_cbufs, uint32_t cbuf_handle,
                                uint32_t zsurf_handle);
int virgl_cmd_set_viewport(virgl_cmd_buf_t *cb, float sx, float sy, float sz,
                           float tx, float ty, float tz);
int virgl_cmd_bind_object(virgl_cmd_buf_t *cb, uint32_t object_type, uint32_t handle);
int virgl_cmd_draw_vbo(virgl_cmd_buf_t *cb, uint32_t mode, uint32_t start,
                       uint32_t count, uint32_t indexed);
int virgl_cmd_create_blend_basic(virgl_cmd_buf_t *cb, uint32_t handle);
int virgl_cmd_create_rasterizer_basic(virgl_cmd_buf_t *cb, uint32_t handle);
int virgl_cmd_create_dsa_basic(virgl_cmd_buf_t *cb, uint32_t handle);
int virgl_cmd_create_vertex_elements_pos_color(virgl_cmd_buf_t *cb, uint32_t handle);
int virgl_cmd_create_shader_tgsi(virgl_cmd_buf_t *cb, uint32_t handle,
                                uint32_t shader_type, const uint32_t *tokens,
                                uint32_t token_count);
int virgl_cmd_set_vertex_buffer_1(virgl_cmd_buf_t *cb, uint32_t resource_id,
                                  uint32_t stride, uint32_t offset);
int virgl_cmd_inline_write(virgl_cmd_buf_t *cb, uint32_t resource_id,
                           uint32_t level, uint32_t usage, uint32_t stride,
                           uint32_t layer_stride, uint32_t x, uint32_t y,
                           uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                           const void *data, uint32_t bytes);

/* Present the default render-target resource to display scanout 0 by issuing
 * TRANSFER_TO_HOST_3D + SET_SCANOUT + RESOURCE_FLUSH.  Returns 0 on success. */
int virgl_present_render_target(uint32_t resource_id, uint32_t width, uint32_t height);

/* Smoke-test / demo helpers. */
int virgl_clear_screen(float r, float g, float b, float a);
int virgl_create_scanout_render_target(uint32_t resource_id, uint32_t width, uint32_t height);
int virgl_demo_submit_clear(void);
int virgl_demo_submit_triangle(void);

#endif /* AURALITE_DRIVERS_GPU_VIRGL_H */
