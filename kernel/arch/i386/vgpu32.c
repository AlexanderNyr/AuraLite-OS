/* kernel/arch/i386/vgpu32.c -- the virtio-gpu backend, honestly absent
 * (RESIDUE3 C3 / RES-11).
 *
 * drivers/framebuffer/graphics.c flips its back buffer to whichever surface
 * is visible: the linear framebuffer, or a virtio-gpu scanout when one is
 * present.  The 32-bit kernel has the framebuffer half (fbmap32.c maps the
 * VBE LFB into a device-memory window) and NOT the virtio-gpu half, because
 * drivers/gpu/virtio_gpu.c speaks the modern virtio-pci transport through
 * pci_config_read()/paging_map() with 64-bit BARs and queue addresses, none
 * of which this port has been taught yet.
 *
 * Rather than teach graphics.c to know which architectures have a GPU (a
 * portable file learning about hardware that is missing), the arch supplies
 * the three symbols and answers 0.  The compositor's flip therefore always
 * takes the framebuffer path here -- which is exactly what a machine with a
 * VBE mode and no passed-through GPU should do.
 *
 * This is a FINDING for the C3 report, not a claim: on i386 the pixels go to
 * the linear framebuffer and to nothing else.
 */

#include <stdint.h>

int virtio_gpu_available(void)
{
    return 0;
}

int virtio_gpu_present(const void *pixels, uint32_t width, uint32_t height,
                       uint32_t pitch_bytes)
{
    (void)pixels; (void)width; (void)height; (void)pitch_bytes;
    return -1;
}

int virtio_gpu_present_rect(const void *pixels, uint32_t width, uint32_t height,
                            uint32_t pitch_bytes, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h)
{
    (void)pixels; (void)width; (void)height; (void)pitch_bytes;
    (void)x; (void)y; (void)w; (void)h;
    return -1;
}
