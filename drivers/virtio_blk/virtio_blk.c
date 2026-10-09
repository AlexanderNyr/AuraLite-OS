#include "drivers/virtio_blk/virtio_blk.h"
#include "drivers/virtio/virtio_common.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/boot_info.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define VIRTIO_VENDOR_ID         0x1AF4
#define VIRTIO_BLK_MODERN_DEVICE 0x1042
#define VIRTIO_BLK_TRANS_DEVICE  0x1001

/* PCI capability IDs */
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAP_VENDOR      0x09

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
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
#if defined(__TINYC__)
#pragma pack(pop)
#endif

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct vblk_queue {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint64_t            desc_phys, avail_phys, used_phys;
    uint16_t            qsize;
    uint16_t            last_used_idx;
    uint16_t            notify_off;
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

static int init_attempted = 0;
static int init_result = -1;
static int present = 0;

static uint8_t pci_bus, pci_dev, pci_func;
static volatile struct virtio_pci_common_cfg *common_cfg;
static volatile uint8_t *notify_base;
static uint32_t notify_multiplier;

static struct vblk_queue q;
static uint64_t device_capacity_sectors;

static uint8_t pci_read8_at(uint8_t off) {
    uint32_t v = pci_config_read(pci_bus, pci_dev, pci_func, off & 0xFC);
    return (uint8_t)(v >> ((off & 3) * 8));
}

static uint64_t map_bar_region(uint8_t bar, uint32_t offset, uint32_t length) {
    uint32_t raw = pci_get_bar(pci_bus, pci_dev, pci_func, bar);
    if (raw == 0 || raw == 0xFFFFFFFF || (raw & 1)) return 0;
    uint64_t phys = (uint64_t)(raw & ~0xFULL) + offset;
    uint64_t hhdm = boot_get_hhdm_offset();
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + length + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000) {
        paging_map(hhdm + p, p, PAGE_FLAGS_MMIO);
    }
    return hhdm + phys;
}

static int parse_virtio_caps(void) {
    uint16_t status = pci_read8_at(0x06) | (pci_read8_at(0x07) << 8);
    if (!(status & PCI_STATUS_CAP_LIST)) return -1;
    uint8_t cap = pci_read8_at(0x34) & 0xFC;
    while (cap) {
        if (pci_read8_at(cap) == PCI_CAP_VENDOR) {
            uint8_t cfg_type = pci_read8_at(cap + 3);
            uint8_t bar = pci_read8_at(cap + 4);
            uint32_t off = pci_config_read(pci_bus, pci_dev, pci_func, cap + 8);
            uint32_t len = pci_config_read(pci_bus, pci_dev, pci_func, cap + 12);
            uint64_t va = map_bar_region(bar, off, len ? len : 0x1000);
            if (va) {
                if (cfg_type == 1) common_cfg = (volatile struct virtio_pci_common_cfg *)(uintptr_t)va;
                else if (cfg_type == 2) {
                    notify_base = (volatile uint8_t *)(uintptr_t)va;
                    notify_multiplier = pci_config_read(pci_bus, pci_dev, pci_func, cap + 16);
                }
            }
        }
        cap = pci_read8_at(cap + 1) & 0xFC;
    }
    return (common_cfg && notify_base) ? 0 : -1;
}

static uint64_t alloc_zero_page(void **virt_out) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    void *virt = (void *)(uintptr_t)(boot_get_hhdm_offset() + phys);
    memset(virt, 0, 4096);
    if (virt_out) *virt_out = virt;
    return phys;
}

static int setup_queue(void) {
    memset(&q, 0, sizeof(q));
    common_cfg->queue_select = 0;
    uint16_t qsz = common_cfg->queue_size;
    if (qsz == 0 || qsz > 256) qsz = 256;
    q.qsize = qsz;

    /* vblk_queue is packed (hardware register layout), so the member
     * ADDRESSES may not be taken (clang 19 -Waddress-of-packed-member,
     * surfaced by the 2026-09 toolchain bump): allocate into locals,
     * assign the values. */
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    q.desc_phys  = alloc_zero_page((void **)&desc);
    q.avail_phys = alloc_zero_page((void **)&avail);
    q.used_phys  = alloc_zero_page((void **)&used);
    q.desc = desc; q.avail = avail; q.used = used;
    if (!q.desc_phys || !q.avail_phys || !q.used_phys) return -1;

    common_cfg->queue_select = 0;
    common_cfg->queue_size = qsz;
    common_cfg->queue_desc = q.desc_phys;
    common_cfg->queue_driver = q.avail_phys;
    common_cfg->queue_device = q.used_phys;
    common_cfg->queue_enable = 1;
    q.notify_off = common_cfg->queue_notify_off;
    q.last_used_idx = q.used->idx;
    return 0;
}

static void notify_queue(void) {
    volatile uint16_t *qn = (volatile uint16_t *)(uintptr_t)
        (notify_base + (uint32_t)q.notify_off * notify_multiplier);
    *qn = 0;
}

int virtio_blk_init(void) {
    if (init_attempted) return init_result;
    init_attempted = 1;
    init_result = -1;

    if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_BLK_MODERN_DEVICE, &pci_bus, &pci_dev, &pci_func) != 0) {
        if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_BLK_TRANS_DEVICE, &pci_bus, &pci_dev, &pci_func) != 0) {
            return -1;
        }
    }
    present = 1;
    pci_enable_bus_master(pci_bus, pci_dev, pci_func);
    if (parse_virtio_caps() != 0) return -1;

    common_cfg->device_status = 0;
    common_cfg->device_status = 1 | 2; // ACK, DRIVER
    common_cfg->device_status |= 8;    // FEATURES_OK
    common_cfg->device_status |= 4;    // DRIVER_OK

    if (setup_queue() != 0) return -1;

    /* Read capacity: write 0 to queue, read back from config space or a specific request.
     * In modern virtio-blk, capacity is in the device config space. */
    uint32_t cap_low = pci_config_read(pci_bus, pci_dev, pci_func, 0x10);
    uint32_t cap_high = pci_config_read(pci_bus, pci_dev, pci_func, 0x14);
    device_capacity_sectors = ((uint64_t)cap_high << 32) | cap_low;

    kprintf("[virtio-blk] ready: capacity %llu sectors\n", device_capacity_sectors);
    init_result = 0;
    return 0;
}

int virtio_blk_available(void) { return present && init_result == 0; }
uint64_t virtio_blk_sector_count(void) { return device_capacity_sectors; }

/* One request = header + data + status, chained through three descriptors.
 * The data buffer is a single 4 KiB frame, so a request covers at most eight
 * sectors; a larger count used to let the device DMA past the frame. */
#define VIRTIO_BLK_MAX_SECTORS 8u
/* Completion polls before a request is abandoned.  A device that never
 * completes used to hang the kernel in an unbounded spin. */
#define VIRTIO_BLK_POLL_LIMIT  50000000u

static uint64_t alloc_temp_page(void **virt) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    /* Callers that only need the physical address pass NULL. */
    if (virt) *virt = (void *)(uintptr_t)(boot_get_hhdm_offset() + phys);
    return phys;
}

static int virtio_blk_transfer(uint32_t type, uint64_t lba, uint32_t count, void *buf) {
    if (!virtio_blk_available()) return -1;
    if (count == 0 || count > VIRTIO_BLK_MAX_SECTORS) return -1;

    uint64_t h_phys = alloc_temp_page(NULL);
    uint64_t d_phys = alloc_temp_page(NULL);
    uint64_t s_phys = alloc_temp_page(NULL);
    if (!h_phys || !d_phys || !s_phys) {
        if (h_phys) pmm_free_frame(h_phys);
        if (d_phys) pmm_free_frame(d_phys);
        if (s_phys) pmm_free_frame(s_phys);
        return -1;
    }

    const uint32_t bytes = count * 512u;
    virtio_blk_req_hdr_t *hdr = (virtio_blk_req_hdr_t *)(uintptr_t)(boot_get_hhdm_offset() + h_phys);
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    hdr->sector = lba;

    void *d_virt = (void *)(uintptr_t)(boot_get_hhdm_offset() + d_phys);
    if (type == VIRTIO_BLK_T_OUT) memcpy(d_virt, buf, bytes);

    /* Status byte: 0xFF until the device writes VIRTIO_BLK_S_OK. */
    volatile uint8_t *status_p = (volatile uint8_t *)(uintptr_t)(boot_get_hhdm_offset() + s_phys);
    *status_p = 0xFF;

    /* Every link needs VRING_DESC_F_NEXT; the device writes the data buffer
     * for a read (IN) and the status byte always.  The chain used to be built
     * without NEXT, so the device saw a header and nothing else. */
    uint16_t slot = q.last_used_idx % q.qsize;
    uint16_t d1 = (uint16_t)((slot + 1) % q.qsize);
    uint16_t d2 = (uint16_t)((slot + 2) % q.qsize);
    q.desc[slot].addr = h_phys;
    q.desc[slot].len = sizeof(virtio_blk_req_hdr_t);
    q.desc[slot].flags = VRING_DESC_F_NEXT;
    q.desc[slot].next = d1;
    q.desc[d1].addr = d_phys;
    q.desc[d1].len = bytes;
    q.desc[d1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    q.desc[d1].next = d2;
    q.desc[d2].addr = s_phys;
    q.desc[d2].len = 1;
    q.desc[d2].flags = VRING_DESC_F_WRITE;
    q.desc[d2].next = 0;

    q.avail->ring[q.avail->idx % q.qsize] = slot;
    q.avail->idx++;
    notify_queue();

    uint32_t spins = 0;
    while (q.used->idx == q.last_used_idx) {
        if (++spins > VIRTIO_BLK_POLL_LIMIT) {
            /* The device may still DMA into these frames, so they are leaked
             * rather than reused, and the device is taken out of service. */
            init_result = -1;
            return -1;
        }
        __asm__ volatile ("pause");
    }

    int res = (*status_p == VIRTIO_BLK_S_OK) ? 0 : -1;
    if (res == 0 && type == VIRTIO_BLK_T_IN) memcpy(buf, d_virt, bytes);

    pmm_free_frame(h_phys); pmm_free_frame(d_phys); pmm_free_frame(s_phys);
    q.last_used_idx = q.used->idx;
    return res;
}

int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    return virtio_blk_transfer(VIRTIO_BLK_T_IN, lba, count, buf);
}

int virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    return virtio_blk_transfer(VIRTIO_BLK_T_OUT, lba, count, (void *)(uintptr_t)buf);
}
