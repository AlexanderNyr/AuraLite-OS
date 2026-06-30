#ifndef AURALITE_DRIVERS_GPU_VIRTIO_GPU_H
#define AURALITE_DRIVERS_GPU_VIRTIO_GPU_H

#include <stdint.h>

/*
 * virtio-gpu PCI driver.
 *
 * Provides modern virtio PCI discovery, control queue setup, optional 2D
 * scanout mirroring for the AuraLite framebuffer and a low-level VirGL/3D
 * command transport.  This is still a kernel driver/transport layer, not a full
 * OpenGL/Vulkan/Gallium userspace stack.
 */

typedef struct virtio_gpu_info {
    int      present;
    int      modern;
    int      controlq_ready;
    int      virgl_supported;
    int      virgl_enabled;
    int      ctx3d_ready;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint16_t width;
    uint16_t height;
    const char *backend_name;
} virtio_gpu_info_t;

int virtio_gpu_init(void);
const virtio_gpu_info_t *virtio_gpu_get_info(void);
int virtio_gpu_available(void);

/* Accelerated 2D scanout path.  The source buffer must contain packed 32-bit
 * pixels in the same format AuraLite's graphics back buffer uses. */
int virtio_gpu_create_2d(uint32_t width, uint32_t height);
int virtio_gpu_present(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch_bytes);
int virtio_gpu_present_rect(const void *pixels, uint32_t width, uint32_t height,
                            uint32_t pitch_bytes, int32_t x, int32_t y,
                            uint32_t w, uint32_t h);

/* VirGL/3D command path.  This is a low-level kernel transport API: callers
 * must provide valid VirGL command-stream bytes. */
int virtio_gpu_virgl_enabled(void);
int virtio_gpu_ctx_create(uint32_t ctx_id, const char *name);
int virtio_gpu_ctx_destroy(uint32_t ctx_id);
int virtio_gpu_resource_create_3d(uint32_t resource_id, uint32_t ctx_id,
                                  uint32_t target, uint32_t format, uint32_t bind,
                                  uint32_t width, uint32_t height, uint32_t depth,
                                  uint32_t array_size, uint32_t last_level,
                                  uint32_t nr_samples, uint32_t flags);
int virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id);
int virtio_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id);
int virtio_gpu_submit_3d(uint32_t ctx_id, const void *cmd, uint32_t cmd_size);
int virtio_gpu_submit_3d_fenced(uint32_t ctx_id, const void *cmd, uint32_t cmd_size,
                                uint64_t *fence_id_out);
uint64_t virtio_gpu_last_fence_id(void);
int virtio_gpu_transfer_to_host_3d(uint32_t ctx_id, uint32_t resource_id,
                                   uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, uint32_t h, uint32_t d,
                                   uint64_t offset, uint32_t level,
                                   uint32_t stride, uint32_t layer_stride);
/* Bind an arbitrary resource id to a display scanout, then flush a sub-rect of
 * it to the host display.  Used to present a VirGL/3D render target. */
int virtio_gpu_set_scanout_resource(uint32_t scanout_id, uint32_t resource_id,
                                    uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int virtio_gpu_flush_resource(uint32_t resource_id,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int virtio_gpu_virgl_supported(void);

#endif /* AURALITE_DRIVERS_GPU_VIRTIO_GPU_H */
