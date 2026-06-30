#include "drivers/virtio_blk/virtio_blk.h"
#include "drivers/virtio/virtio_common.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/limine_requests.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define VIRTIO_VENDOR_ID         0x1AF4
#define VIRTIO_BLK_MODERN_DEVICE 0x1042
#define VIRTIO_BLK_TRANS_DEVICE  0x1001

/* PCI capability IDs */
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAP_VENDOR      0x09

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

struct vblk_queue {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint64_t            desc_phys, avail_phys, used_phys;
    uint16_t            qsize;
    uint16_t            last_used_idx;
    uint16_t            notify_off;
} __attribute__((packed));

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
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + length + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000) {
        paging_map(hhdm + p, p, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
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
    void *virt = (void *)(uintptr_t)(limine_get_hhdm_offset() + phys);
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

    q.desc_phys = alloc_zero_page((void **)&q.desc);
    q.avail_phys = alloc_zero_page((void **)&q.avail);
    q.used_phys = alloc_zero_page((void **)&q.used);
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

static uint64_t alloc_temp_page(void **virt) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    *virt = (void *)(uintptr_t)(limine_get_hhdm_offset() + phys);
    return phys;
}

int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    if (!virtio_blk_available()) return -1;
    
    uint64_t h_phys = alloc_temp_page(NULL);
    uint64_t d_phys = alloc_temp_page(NULL);
    uint64_t s_phys = alloc_temp_page(NULL);
    if (!h_phys || !d_phys || !s_phys) return -1;

    virtio_blk_req_hdr_t *hdr = (virtio_blk_req_hdr_t *)(uintptr_t)(limine_get_hhdm_offset() + h_phys);
    hdr->type = VIRTIO_BLK_T_IN;
    hdr->sector = lba;

    /* Map data buffer to physical if it's not already (simplified: we copy). */
    void *d_virt = (void *)(uintptr_t)(limine_get_hhdm_offset() + d_phys);
    
    uint16_t slot = q.last_used_idx % q.qsize;
    q.desc[slot].addr = h_phys; q.desc[slot].len = sizeof(virtio_blk_req_hdr_t); q.desc[slot].flags = 0; q.desc[slot].next = 1;
    q.desc[(slot+1)%q.qsize].addr = d_phys; q.desc[(slot+1)%q.qsize].len = count * 512; q.desc[(slot+1)%q.qsize].flags = 0; q.desc[(slot+1)%q.qsize].next = 2;
    q.desc[(slot+2)%q.qsize].addr = s_phys; q.desc[(slot+2)%q.qsize].len = 4; q.desc[(slot+2)%q.qsize].flags = VRING_DESC_F_WRITE; q.desc[(slot+2)%q.qsize].next = 0;

    q.avail->ring[q.avail->idx % q.qsize] = slot;
    q.avail->idx++;
    notify_queue();

    while (q.used->idx == q.last_used_idx) { __asm__ volatile ("pause"); }
    
    uint32_t status = *(volatile uint32_t *)(uintptr_t)(limine_get_hhdm_offset() + s_phys);
    int res = (status == VIRTIO_BLK_S_OK) ? 0 : -1;
    if (res == 0) memcpy(buf, d_virt, count * 512);

    pmm_free_frame(h_phys); pmm_free_frame(d_phys); pmm_free_frame(s_phys);
    q.last_used_idx = q.used->idx;
    return res;
}

int virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    if (!virtio_blk_available()) return -1;
    
    uint64_t h_phys = alloc_temp_page(NULL);
    uint64_t d_phys = alloc_temp_page(NULL);
    uint64_t s_phys = alloc_temp_page(NULL);
    if (!h_phys || !d_phys || !s_phys) return -1;

    virtio_blk_req_hdr_t *hdr = (virtio_blk_req_hdr_t *)(uintptr_t)(limine_get_hhdm_offset() + h_phys);
    hdr->type = VIRTIO_BLK_T_OUT;
    hdr->sector = lba;

    void *d_virt = (void *)(uintptr_t)(limine_get_hhdm_offset() + d_phys);
    memcpy(d_virt, buf, count * 512);
    
    uint16_t slot = q.last_used_idx % q.qsize;
    q.desc[slot].addr = h_phys; q.desc[slot].len = sizeof(virtio_blk_req_hdr_t); q.desc[slot].flags = 0; q.desc[slot].next = 1;
    q.desc[(slot+1)%q.qsize].addr = d_phys; q.desc[(slot+1)%q.qsize].len = count * 512; q.desc[(slot+1)%q.qsize].flags = 0; q.desc[(slot+1)%q.qsize].next = 2;
    q.desc[(slot+2)%q.qsize].addr = s_phys; q.desc[(slot+2)%q.qsize].len = 4; q.desc[(slot+2)%q.qsize].flags = VRING_DESC_F_WRITE; q.desc[(slot+2)%q.qsize].next = 0;

    q.avail->ring[q.avail->idx % q.qsize] = slot;
    q.avail->idx++;
    notify_queue();

    while (q.used->idx == q.last_used_idx) { __asm__ volatile ("pause"); }
    
    uint32_t status = *(volatile uint32_t *)(uintptr_t)(limine_get_hhdm_offset() + s_phys);
    int res = (status == VIRTIO_BLK_S_OK) ? 0 : -1;

    pmm_free_frame(h_phys); pmm_free_frame(d_phys); pmm_free_frame(s_phys);
    q.last_used_idx = q.used->idx;
    return res;
}
