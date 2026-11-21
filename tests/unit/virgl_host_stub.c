/* tests/unit/virgl_host_stub.c — inert stand-ins for virgl.c's kernel
 * dependencies, so the REAL packet builders in drivers/gpu/virgl.c run in a
 * host test (GPU_ACCEL_PLAN H3).
 *
 * Generated from drivers/gpu/virtio_gpu.h's own declarations: the compiler
 * checks every signature against the header, so a stub that drifts from the
 * transport is a build error rather than a silently wrong test.
 *
 * The builders under test never reach any of these — the transport is only
 * touched by submit/attach paths, which the packet tests do not call.  A
 * return of -1 from every function is a statement that a test which DID
 * reach the transport would fail loudly rather than pass by accident.
 */
#include <stdint.h>
#include <stddef.h>
#include "drivers/gpu/virtio_gpu.h"

int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int virtio_gpu_init(void) { return -1; }
int virtio_gpu_available(void) { return 0; }
int virtio_gpu_create_2d(uint32_t width, uint32_t height) { (void)width; (void)height; return -1; }
int virtio_gpu_present(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch_bytes) { (void)pixels; (void)width; (void)height; (void)pitch_bytes; return -1; }
int virtio_gpu_present_rect(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch_bytes, int32_t x, int32_t y, uint32_t w, uint32_t h) { (void)pixels; (void)width; (void)height; (void)pitch_bytes; (void)x; (void)y; (void)w; (void)h; return -1; }
int virtio_gpu_virgl_enabled(void) { return -1; }
int virtio_gpu_ctx_create(uint32_t ctx_id, const char *name) { (void)ctx_id; (void)name; return -1; }
int virtio_gpu_ctx_destroy(uint32_t ctx_id) { (void)ctx_id; return -1; }
int virtio_gpu_resource_create_3d(uint32_t resource_id, uint32_t ctx_id, uint32_t target, uint32_t format, uint32_t bind, uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size, uint32_t last_level, uint32_t nr_samples, uint32_t flags) { (void)resource_id; (void)ctx_id; (void)target; (void)format; (void)bind; (void)width; (void)height; (void)depth; (void)array_size; (void)last_level; (void)nr_samples; (void)flags; return -1; }
int virtio_gpu_resource_attach_memory(uint32_t resource_id, uint32_t bytes) { (void)resource_id; (void)bytes; return -1; }
int virtio_gpu_resource_upload(uint32_t ctx_id, uint32_t resource_id, const void *src, uint32_t size, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint32_t level, uint32_t stride, uint32_t layer_stride) { (void)ctx_id; (void)resource_id; (void)src; (void)size; (void)x; (void)y; (void)z; (void)w; (void)h; (void)d; (void)level; (void)stride; (void)layer_stride; return -1; }
int virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id) { (void)ctx_id; (void)resource_id; return -1; }
int virtio_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id) { (void)ctx_id; (void)resource_id; return -1; }
int virtio_gpu_submit_3d(uint32_t ctx_id, const void *cmd, uint32_t cmd_size) { (void)ctx_id; (void)cmd; (void)cmd_size; return -1; }
int virtio_gpu_submit_3d_fenced(uint32_t ctx_id, const void *cmd, uint32_t cmd_size, uint64_t *fence_id_out) { (void)ctx_id; (void)cmd; (void)cmd_size; (void)fence_id_out; return -1; }
int virtio_gpu_transfer_to_host_3d(uint32_t ctx_id, uint32_t resource_id, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint64_t offset, uint32_t level, uint32_t stride, uint32_t layer_stride) { (void)ctx_id; (void)resource_id; (void)x; (void)y; (void)z; (void)w; (void)h; (void)d; (void)offset; (void)level; (void)stride; (void)layer_stride; return -1; }
int virtio_gpu_set_scanout_resource(uint32_t scanout_id, uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)scanout_id; (void)resource_id; (void)x; (void)y; (void)w; (void)h; return -1; }
int virtio_gpu_flush_resource(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)resource_id; (void)x; (void)y; (void)w; (void)h; return -1; }
int virtio_gpu_virgl_supported(void) { return -1; }

/* Not matched by the generator's regex (pointer return type), added by hand
 * with the header's exact signature. */
const virtio_gpu_info_t *virtio_gpu_get_info(void) {
    static virtio_gpu_info_t info;
    return &info;
}
