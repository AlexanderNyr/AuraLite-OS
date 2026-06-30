/* virtio_gpu.c — modern virtio-gpu PCI control-queue driver.
 *
 * The driver brings up virtio-gpu on modern PCI, configures queue 0, supports
 * 2D scanout mirroring and provides a low-level VirGL/3D submit path.  It is a
 * transport/acceleration backend for AuraLite's renderer, not a complete
 * Gallium/OpenGL state tracker.
 */

#include <stdint.h>
#include <stddef.h>
#include "drivers/gpu/virtio_gpu.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/limine_requests.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_GPU_MODERN_DEVICE 0x1050
#define VIRTIO_GPU_TRANS_DEVICE  0x1000 /* transitional range starts here; GPU is often 0x1050 modern */

/* PCI capability IDs. */
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAP_VENDOR     0x09

/* virtio PCI capability cfg_type values. */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

/* Device status bits. */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

#define VIRTIO_GPU_FLAG_FENCE     1u

/* Feature bits. */
#define VIRTIO_F_VERSION_1 32
#define VIRTIO_GPU_F_VIRGL 0

/* Virtqueue descriptor flags. */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VGPU_Q_SIZE 8

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO      0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF        0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT           0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH        0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_CTX_CREATE            0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY           0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE   0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE   0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D    0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D   0x0205
#define VIRTIO_GPU_CMD_SUBMIT_3D             0x0206
#define VIRTIO_GPU_RESP_OK_NODATA            0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO      0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM     2
#define VGPU_RESOURCE_ID                      1

struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VGPU_Q_SIZE];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VGPU_Q_SIZE];
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing_1 {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct virtio_gpu_mem_entry entry;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;
    char debug_name[64];
} __attribute__((packed));

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
} __attribute__((packed));

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed));

static virtio_gpu_info_t info = {
    .backend_name = "none",
};

static int init_attempted;
static int init_result = -1;
static uint64_t next_fence_id = 1;
static uint64_t last_fence_id;

static uint8_t pci_bus, pci_dev, pci_func;
static volatile struct virtio_pci_common_cfg *common_cfg;
static volatile uint8_t *notify_base;
static uint32_t notify_multiplier;
static volatile uint8_t *isr_cfg;
static volatile uint8_t *device_cfg;

static struct vring_desc *vq_desc;
static struct vring_avail *vq_avail;
static struct vring_used *vq_used;
static uint64_t vq_desc_phys, vq_avail_phys, vq_used_phys;
static uint16_t vq_last_used_idx;

static void *cmd_req;
static void *cmd_resp;
static uint64_t cmd_req_phys, cmd_resp_phys;

static uint8_t *gpu_fb;
static uint64_t gpu_fb_phys;
static uint64_t gpu_fb_pages;
static uint32_t gpu_fb_w, gpu_fb_h, gpu_fb_pitch;
static int gpu_resource_ready;

static uint16_t pci_read16_at(uint8_t off) {
    uint32_t v = pci_config_read(pci_bus, pci_dev, pci_func, off & 0xFC);
    return (uint16_t)(v >> ((off & 2) * 8));
}

static uint8_t pci_read8_at(uint8_t off) {
    uint32_t v = pci_config_read(pci_bus, pci_dev, pci_func, off & 0xFC);
    return (uint8_t)(v >> ((off & 3) * 8));
}

static uint64_t map_bar_region(uint8_t bar, uint32_t offset, uint32_t length) {
    uint32_t raw = pci_get_bar(pci_bus, pci_dev, pci_func, bar);
    if (raw == 0 || raw == 0xFFFFFFFF || (raw & 1)) return 0;
    uint64_t phys = (uint64_t)(raw & ~0xFULL) + offset;
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + length + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000) {
        paging_map(hhdm + p, p, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    }
    return hhdm + phys;
}

static int parse_virtio_caps(void) {
    uint16_t status = pci_read16_at(0x06);
    if (!(status & PCI_STATUS_CAP_LIST)) return -1;
    uint8_t cap = pci_read8_at(0x34) & 0xFC;
    int guard = 0;
    while (cap && guard++ < 64) {
        uint8_t cap_vndr = pci_read8_at(cap + 0);
        uint8_t cap_next = pci_read8_at(cap + 1) & 0xFC;
        if (cap_vndr == PCI_CAP_VENDOR) {
            uint8_t cfg_type = pci_read8_at(cap + 3);
            uint8_t bar = pci_read8_at(cap + 4);
            uint32_t off = pci_config_read(pci_bus, pci_dev, pci_func, cap + 8);
            uint32_t len = pci_config_read(pci_bus, pci_dev, pci_func, cap + 12);
            uint64_t va = map_bar_region(bar, off, len ? len : 0x1000);
            if (va) {
                if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                    common_cfg = (volatile struct virtio_pci_common_cfg *)(uintptr_t)va;
                } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    notify_base = (volatile uint8_t *)(uintptr_t)va;
                    notify_multiplier = pci_config_read(pci_bus, pci_dev, pci_func, cap + 16);
                } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
                    isr_cfg = (volatile uint8_t *)(uintptr_t)va;
                } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                    device_cfg = (volatile uint8_t *)(uintptr_t)va;
                }
            }
        }
        cap = cap_next;
    }
    return (common_cfg && notify_base) ? 0 : -1;
}

static uint64_t alloc_zero_page(void **virt_out) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    void *virt = (void *)(uintptr_t)(limine_get_hhdm_offset() + phys);
    memset(virt, 0, 4096);
    if (virt_out) *virt_out = virt;
    return phys;
}

static uint64_t read_device_features(void) {
    common_cfg->device_feature_select = 0;
    uint32_t lo = common_cfg->device_feature;
    common_cfg->device_feature_select = 1;
    uint32_t hi = common_cfg->device_feature;
    return ((uint64_t)hi << 32) | lo;
}

static void write_driver_features(uint64_t f) {
    common_cfg->driver_feature_select = 0;
    common_cfg->driver_feature = (uint32_t)f;
    common_cfg->driver_feature_select = 1;
    common_cfg->driver_feature = (uint32_t)(f >> 32);
}

static int setup_control_queue(void) {
    common_cfg->queue_select = 0;
    uint16_t qsz = common_cfg->queue_size;
    if (qsz == 0) return -1;
    if (qsz > VGPU_Q_SIZE) qsz = VGPU_Q_SIZE;

    vq_desc_phys = alloc_zero_page((void **)&vq_desc);
    vq_avail_phys = alloc_zero_page((void **)&vq_avail);
    vq_used_phys = alloc_zero_page((void **)&vq_used);
    if (!vq_desc_phys || !vq_avail_phys || !vq_used_phys) return -1;

    common_cfg->queue_size = qsz;
    common_cfg->queue_desc = vq_desc_phys;
    common_cfg->queue_driver = vq_avail_phys;
    common_cfg->queue_device = vq_used_phys;
    common_cfg->queue_enable = 1;
    vq_last_used_idx = vq_used->idx;
    info.controlq_ready = 1;
    return 0;
}

static void notify_queue0(void) {
    common_cfg->queue_select = 0;
    uint16_t off = common_cfg->queue_notify_off;
    volatile uint16_t *qnotify = (volatile uint16_t *)(uintptr_t)(notify_base + (uint32_t)off * notify_multiplier);
    *qnotify = 0;
}

static int submit_2desc(uint64_t req_phys, uint32_t req_len,
                        uint64_t resp_phys, uint32_t resp_len) {
    vq_desc[0].addr = req_phys;
    vq_desc[0].len = req_len;
    vq_desc[0].flags = VRING_DESC_F_NEXT;
    vq_desc[0].next = 1;
    vq_desc[1].addr = resp_phys;
    vq_desc[1].len = resp_len;
    vq_desc[1].flags = VRING_DESC_F_WRITE;
    vq_desc[1].next = 0;

    uint16_t aidx = vq_avail->idx;
    vq_avail->ring[aidx % VGPU_Q_SIZE] = 0;
    __asm__ volatile ("mfence" ::: "memory");
    vq_avail->idx = aidx + 1;
    __asm__ volatile ("mfence" ::: "memory");
    notify_queue0();

    int timeout = 10000000;
    while (vq_used->idx == vq_last_used_idx && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
    if (timeout <= 0) return -1;
    __asm__ volatile ("mfence" ::: "memory");
    vq_last_used_idx = vq_used->idx;
    if (isr_cfg) (void)*isr_cfg;
    return 0;
}

static int submit_desc_chain(struct vring_desc *descs, uint16_t head) {
    for (uint16_t i = 0; i < VGPU_Q_SIZE; i++) vq_desc[i] = descs[i];
    uint16_t aidx = vq_avail->idx;
    vq_avail->ring[aidx % VGPU_Q_SIZE] = head;
    __asm__ volatile ("mfence" ::: "memory");
    vq_avail->idx = aidx + 1;
    __asm__ volatile ("mfence" ::: "memory");
    notify_queue0();
    int timeout = 10000000;
    while (vq_used->idx == vq_last_used_idx && timeout-- > 0) __asm__ volatile ("pause");
    if (timeout <= 0) return -1;
    __asm__ volatile ("mfence" ::: "memory");
    vq_last_used_idx = vq_used->idx;
    if (isr_cfg) (void)*isr_cfg;
    return 0;
}

static int ensure_cmd_pages(void) {
    if (cmd_req && cmd_resp) return 0;
    cmd_req_phys = alloc_zero_page(&cmd_req);
    cmd_resp_phys = alloc_zero_page(&cmd_resp);
    return (cmd_req_phys && cmd_resp_phys) ? 0 : -1;
}

static int gpu_cmd(const void *req, uint32_t req_len, void *resp_out,
                   uint32_t resp_len, uint32_t expect_type) {
    if (!info.controlq_ready || ensure_cmd_pages() != 0) return -1;
    if (req_len > 4096 || resp_len > 4096) return -1;
    memset(cmd_req, 0, 4096);
    memset(cmd_resp, 0, 4096);
    memcpy(cmd_req, req, req_len);
    if (submit_2desc(cmd_req_phys, req_len, cmd_resp_phys, resp_len) != 0) return -1;
    struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)cmd_resp;
    if (hdr->type != expect_type) {
        kprintf("[virtio-gpu] cmd 0x%x unexpected resp=0x%x\n",
                ((const struct virtio_gpu_ctrl_hdr *)req)->type, hdr->type);
        return -1;
    }
    if (resp_out && resp_len) memcpy(resp_out, cmd_resp, resp_len);
    return 0;
}

static int get_display_info(void) {
    struct virtio_gpu_ctrl_hdr req;
    struct virtio_gpu_resp_display_info resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (gpu_cmd(&req, sizeof(req), &resp, sizeof(resp),
                VIRTIO_GPU_RESP_OK_DISPLAY_INFO) != 0) return -1;

    for (int i = 0; i < 16; i++) {
        if (resp.pmodes[i].enabled && resp.pmodes[i].r.width && resp.pmodes[i].r.height) {
            info.width = (uint16_t)resp.pmodes[i].r.width;
            info.height = (uint16_t)resp.pmodes[i].r.height;
            kprintf("[virtio-gpu] scanout %d: %ux%u flags=0x%x\n", i,
                    resp.pmodes[i].r.width, resp.pmodes[i].r.height, resp.pmodes[i].flags);
            return 0;
        }
    }
    kprintf("[virtio-gpu] display info OK, no enabled scanout\n");
    return 0;
}

static int gpu_cmd_nodata(const void *req, uint32_t req_len) {
    struct virtio_gpu_ctrl_hdr resp;
    return gpu_cmd(req, req_len, &resp, sizeof(resp), VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_unref_resource(uint32_t resource_id) {
    /* RESOURCE_UNREF has resource_id immediately after the generic header in
     * the virtio-gpu protocol; reuse a small local packed shape. */
    struct { struct virtio_gpu_ctrl_hdr hdr; uint32_t rid; uint32_t pad; } pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    pkt.rid = resource_id;
    return gpu_cmd_nodata(&pkt, sizeof(pkt));
}

static int gpu_cmd_with_payload(const void *req, uint32_t req_len,
                                uint64_t payload_phys, uint32_t payload_len) {
    if (!info.controlq_ready || ensure_cmd_pages() != 0) return -1;
    memset(cmd_req, 0, 4096);
    memset(cmd_resp, 0, 4096);
    memcpy(cmd_req, req, req_len);
    struct vring_desc d[VGPU_Q_SIZE];
    memset(d, 0, sizeof(d));
    d[0].addr = cmd_req_phys;
    d[0].len = req_len;
    d[0].flags = VRING_DESC_F_NEXT;
    d[0].next = 1;
    d[1].addr = payload_phys;
    d[1].len = payload_len;
    d[1].flags = VRING_DESC_F_NEXT;
    d[1].next = 2;
    d[2].addr = cmd_resp_phys;
    d[2].len = sizeof(struct virtio_gpu_ctrl_hdr);
    d[2].flags = VRING_DESC_F_WRITE;
    if (submit_desc_chain(d, 0) != 0) return -1;
    struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)cmd_resp;
    if (hdr->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[virtio-gpu] payload cmd 0x%x unexpected resp=0x%x\n",
                ((const struct virtio_gpu_ctrl_hdr *)req)->type, hdr->type);
        return -1;
    }
    return 0;
}

int virtio_gpu_create_2d(uint32_t width, uint32_t height) {
    if (!virtio_gpu_available() || width == 0 || height == 0) return -1;
    if (gpu_resource_ready && gpu_fb_w == width && gpu_fb_h == height) return 0;
    if (gpu_resource_ready) {
        (void)virtio_gpu_unref_resource(VGPU_RESOURCE_ID);
        if (gpu_fb_phys && gpu_fb_pages) {
            for (uint64_t i = 0; i < gpu_fb_pages; i++) pmm_free_frame(gpu_fb_phys + i * 4096ULL);
        }
        gpu_resource_ready = 0;
        gpu_fb = 0; gpu_fb_phys = 0; gpu_fb_pages = 0;
        gpu_fb_w = gpu_fb_h = gpu_fb_pitch = 0;
    }

    uint64_t bytes = (uint64_t)width * (uint64_t)height * 4ULL;
    uint64_t pages = (bytes + 4095ULL) / 4096ULL;
    gpu_fb_phys = pmm_alloc_contiguous(pages);
    if (!gpu_fb_phys) {
        kprintf("[virtio-gpu] failed to allocate %llu contiguous pages for scanout\n",
                (unsigned long long)pages);
        return -1;
    }
    gpu_fb_pages = pages;
    gpu_fb = (uint8_t *)(uintptr_t)(limine_get_hhdm_offset() + gpu_fb_phys);
    memset(gpu_fb, 0, (size_t)(pages * 4096ULL));
    gpu_fb_w = width;
    gpu_fb_h = height;
    gpu_fb_pitch = width * 4;

    struct virtio_gpu_resource_create_2d create;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = VGPU_RESOURCE_ID;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = width;
    create.height = height;
    if (gpu_cmd_nodata(&create, sizeof(create)) != 0) return -1;

    struct virtio_gpu_resource_attach_backing_1 attach;
    memset(&attach, 0, sizeof(attach));
    attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.resource_id = VGPU_RESOURCE_ID;
    attach.nr_entries = 1;
    attach.entry.addr = gpu_fb_phys;
    attach.entry.length = (uint32_t)bytes;
    if (gpu_cmd_nodata(&attach, sizeof(attach)) != 0) return -1;

    struct virtio_gpu_set_scanout scanout;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.width = width;
    scanout.r.height = height;
    scanout.scanout_id = 0;
    scanout.resource_id = VGPU_RESOURCE_ID;
    if (gpu_cmd_nodata(&scanout, sizeof(scanout)) != 0) return -1;

    gpu_resource_ready = 1;
    if (!info.width) info.width = (uint16_t)width;
    if (!info.height) info.height = (uint16_t)height;
    kprintf("[virtio-gpu] 2D resource ready: %ux%u backing=%llu KiB\n",
            width, height, (unsigned long long)(bytes / 1024ULL));
    return 0;
}

int virtio_gpu_present_rect(const void *pixels, uint32_t width, uint32_t height,
                            uint32_t pitch_bytes, int32_t x, int32_t y,
                            uint32_t w, uint32_t h) {
    if (!pixels || !virtio_gpu_available()) return -1;
    if (virtio_gpu_create_2d(width, height) != 0) return -1;

    int32_t x1 = x + (int32_t)w;
    int32_t y1 = y + (int32_t)h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > (int32_t)width) x1 = (int32_t)width;
    if (y1 > (int32_t)height) y1 = (int32_t)height;
    if (x >= x1 || y >= y1) return 0;
    w = (uint32_t)(x1 - x);
    h = (uint32_t)(y1 - y);

    const uint8_t *src = (const uint8_t *)pixels;
    for (uint32_t row = 0; row < h; row++) {
        memcpy(gpu_fb + ((uint32_t)y + row) * gpu_fb_pitch + (uint32_t)x * 4,
               src + ((uint32_t)y + row) * pitch_bytes + (uint32_t)x * 4,
               w * 4);
    }
    __asm__ volatile ("mfence" ::: "memory");

    struct virtio_gpu_transfer_to_host_2d tx;
    memset(&tx, 0, sizeof(tx));
    tx.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    tx.r.x = (uint32_t)x; tx.r.y = (uint32_t)y; tx.r.width = w; tx.r.height = h;
    tx.offset = (uint64_t)(uint32_t)y * gpu_fb_pitch + (uint64_t)(uint32_t)x * 4ULL;
    tx.resource_id = VGPU_RESOURCE_ID;
    if (gpu_cmd_nodata(&tx, sizeof(tx)) != 0) return -1;

    struct virtio_gpu_resource_flush fl;
    memset(&fl, 0, sizeof(fl));
    fl.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    fl.r.x = (uint32_t)x; fl.r.y = (uint32_t)y; fl.r.width = w; fl.r.height = h;
    fl.resource_id = VGPU_RESOURCE_ID;
    return gpu_cmd_nodata(&fl, sizeof(fl));
}

int virtio_gpu_present(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch_bytes) {
    return virtio_gpu_present_rect(pixels, width, height, pitch_bytes, 0, 0, width, height);
}

int virtio_gpu_init(void) {
    if (init_attempted) return init_result;
    init_attempted = 1;
    init_result = -1;

    memset(&info, 0, sizeof(info));
    info.backend_name = "none";

    if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_GPU_MODERN_DEVICE,
                        &pci_bus, &pci_dev, &pci_func) != 0) {
        return -1;
    }

    info.present = 1;
    info.modern = 1;
    info.pci_vendor = VIRTIO_VENDOR_ID;
    info.pci_device = VIRTIO_GPU_MODERN_DEVICE;
    info.backend_name = "virtio-gpu-modern";
    kprintf("[virtio-gpu] found modern GPU at PCI %u:%u.%u\n", pci_bus, pci_dev, pci_func);

    pci_enable_bus_master(pci_bus, pci_dev, pci_func);
    if (parse_virtio_caps() != 0) {
        kprintf("[virtio-gpu] modern PCI capabilities not found\n");
        info.backend_name = "virtio-gpu-probe-only";
        return -1;
    }

    common_cfg->device_status = 0;
    __asm__ volatile ("mfence" ::: "memory");
    common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    common_cfg->device_status |= VIRTIO_STATUS_DRIVER;

    uint64_t dev_features = read_device_features();
    uint64_t wanted = (1ULL << VIRTIO_F_VERSION_1);
    if (dev_features & (1ULL << VIRTIO_GPU_F_VIRGL)) {
        info.virgl_supported = 1;
        wanted |= (1ULL << VIRTIO_GPU_F_VIRGL);
    }
    write_driver_features(wanted);
    common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio-gpu] FEATURES_OK rejected\n");
        common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    info.virgl_enabled = info.virgl_supported;

    if (setup_control_queue() != 0) {
        kprintf("[virtio-gpu] failed to setup control queue\n");
        common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }

    common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    if (get_display_info() != 0) {
        kprintf("[virtio-gpu] control queue ready, display-info command failed\n");
    }

    kprintf("[virtio-gpu] ready: virgl=%d enabled=%d size=%ux%u\n",
            info.virgl_supported, info.virgl_enabled, info.width, info.height);
    if (info.virgl_enabled) {
        if (virtio_gpu_ctx_create(1, "AuraLite3D") == 0)
            kprintf("[virtio-gpu] default VirGL context 1 ready\n");
        else
            kprintf("[virtio-gpu] default VirGL context create failed\n");
    }
    init_result = 0;
    return init_result;
}

int virtio_gpu_virgl_enabled(void) { return info.virgl_enabled; }

int virtio_gpu_ctx_create(uint32_t ctx_id, const char *name) {
    if (!virtio_gpu_available() || !info.virgl_enabled) return -1;
    struct virtio_gpu_ctx_create req;
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    req.hdr.ctx_id = ctx_id;
    if (name) {
        uint32_t n = 0;
        while (name[n] && n < sizeof(req.debug_name) - 1) { req.debug_name[n] = name[n]; n++; }
        req.nlen = n;
    }
    int rc = gpu_cmd_nodata(&req, sizeof(req));
    if (rc == 0) info.ctx3d_ready = 1;
    return rc;
}

int virtio_gpu_ctx_destroy(uint32_t ctx_id) {
    if (!virtio_gpu_available() || !info.virgl_enabled) return -1;
    struct virtio_gpu_ctrl_hdr req;
    memset(&req, 0, sizeof(req));
    req.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    req.ctx_id = ctx_id;
    return gpu_cmd_nodata(&req, sizeof(req));
}

int virtio_gpu_resource_create_3d(uint32_t resource_id, uint32_t ctx_id,
                                  uint32_t target, uint32_t format, uint32_t bind,
                                  uint32_t width, uint32_t height, uint32_t depth,
                                  uint32_t array_size, uint32_t last_level,
                                  uint32_t nr_samples, uint32_t flags) {
    if (!virtio_gpu_available() || !info.virgl_enabled) return -1;
    struct virtio_gpu_resource_create_3d req;
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    req.hdr.ctx_id = ctx_id;
    req.resource_id = resource_id;
    req.target = target; req.format = format; req.bind = bind;
    req.width = width; req.height = height; req.depth = depth;
    req.array_size = array_size; req.last_level = last_level;
    req.nr_samples = nr_samples; req.flags = flags;
    return gpu_cmd_nodata(&req, sizeof(req));
}

static int ctx_res_cmd(uint32_t type, uint32_t ctx_id, uint32_t resource_id) {
    if (!virtio_gpu_available() || !info.virgl_enabled) return -1;
    struct virtio_gpu_ctx_resource req;
    memset(&req, 0, sizeof(req));
    req.hdr.type = type;
    req.hdr.ctx_id = ctx_id;
    req.resource_id = resource_id;
    return gpu_cmd_nodata(&req, sizeof(req));
}

int virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id) {
    return ctx_res_cmd(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, resource_id);
}
int virtio_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id) {
    return ctx_res_cmd(VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, resource_id);
}

int virtio_gpu_submit_3d_fenced(uint32_t ctx_id, const void *cmd, uint32_t cmd_size,
                                uint64_t *fence_id_out) {
    if (!virtio_gpu_available() || !info.virgl_enabled || !cmd || cmd_size == 0) return -1;
    uint64_t pages = ((uint64_t)cmd_size + 4095ULL) / 4096ULL;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) return -1;
    void *virt = (void *)(uintptr_t)(limine_get_hhdm_offset() + phys);
    memset(virt, 0, (size_t)(pages * 4096ULL));
    memcpy(virt, cmd, cmd_size);

    uint64_t fence = next_fence_id++;
    struct virtio_gpu_cmd_submit req;
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    req.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    req.hdr.fence_id = fence;
    req.hdr.ctx_id = ctx_id;
    req.size = cmd_size;
    int rc = gpu_cmd_with_payload(&req, sizeof(req), phys, cmd_size);
    for (uint64_t i = 0; i < pages; i++) pmm_free_frame(phys + i * 4096ULL);
    if (rc == 0) {
        last_fence_id = fence;
        if (fence_id_out) *fence_id_out = fence;
    }
    return rc;
}

int virtio_gpu_submit_3d(uint32_t ctx_id, const void *cmd, uint32_t cmd_size) {
    return virtio_gpu_submit_3d_fenced(ctx_id, cmd, cmd_size, NULL);
}

uint64_t virtio_gpu_last_fence_id(void) { return last_fence_id; }

int virtio_gpu_transfer_to_host_3d(uint32_t ctx_id, uint32_t resource_id,
                                   uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, uint32_t h, uint32_t d,
                                   uint64_t offset, uint32_t level,
                                   uint32_t stride, uint32_t layer_stride) {
    if (!virtio_gpu_available() || !info.virgl_enabled) return -1;
    struct virtio_gpu_transfer_host_3d req;
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    req.hdr.ctx_id = ctx_id;
    req.box.x = x; req.box.y = y; req.box.z = z;
    req.box.w = w; req.box.h = h; req.box.d = d;
    req.offset = offset; req.resource_id = resource_id; req.level = level;
    req.stride = stride; req.layer_stride = layer_stride;
    return gpu_cmd_nodata(&req, sizeof(req));
}

int virtio_gpu_set_scanout_resource(uint32_t scanout_id, uint32_t resource_id,
                                    uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!virtio_gpu_available()) return -1;
    struct virtio_gpu_set_scanout scanout;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.x = x; scanout.r.y = y; scanout.r.width = w; scanout.r.height = h;
    scanout.scanout_id = scanout_id;
    scanout.resource_id = resource_id;
    return gpu_cmd_nodata(&scanout, sizeof(scanout));
}

int virtio_gpu_flush_resource(uint32_t resource_id,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!virtio_gpu_available()) return -1;
    struct virtio_gpu_resource_flush fl;
    memset(&fl, 0, sizeof(fl));
    fl.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    fl.r.x = x; fl.r.y = y; fl.r.width = w; fl.r.height = h;
    fl.resource_id = resource_id;
    return gpu_cmd_nodata(&fl, sizeof(fl));
}

const virtio_gpu_info_t *virtio_gpu_get_info(void) { return &info; }
int virtio_gpu_available(void) { return info.present && info.controlq_ready; }
int virtio_gpu_virgl_supported(void) { return info.virgl_supported; }
