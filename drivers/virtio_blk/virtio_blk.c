#include "drivers/virtio_blk/virtio_blk.h"
#include "drivers/virtio/virtio_common.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/boot_info.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/arch/arch.h"   /* RESIDUE3 C4: MSI, via the width seam */

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
/* C4: the ISR-status region (virtio-pci cfg_type 3).  Reading it is
 * what retires the interrupt -- it is READ-TO-CLEAR, which is why the
 * handler must touch this and nothing else. */
static volatile uint8_t *isr_base;
static volatile uint8_t *devcfg_base;
/* C4: the device-config region (cfg_type 4), where virtio-blk's capacity
 * lives.  The old code read BAR0 out of PCI config space and called it a
 * sector count (measured: 18356408198371524609 sectors on an 8 MiB disk) --
 * harmless only because the bound check it fed was always satisfied. */
static uint32_t notify_multiplier;

static struct vblk_queue q;
static uint64_t device_capacity_sectors;
/* C4: the MSI-X vector this device was attached to (0 = INTx/polling), and
 * the number of interrupts it has taken.  The counter is the receipt that
 * proves the vector is not merely programmed but DELIVERED. */
static uint32_t msi_vector;
/* Did the device offer VIRTIO_PCI_F_MSI_X (bit 27)?  Probing MSI-X on a
 * device that did not is legal but the answer means nothing. */
static int      msi_offered;
static volatile uint32_t msi_irqs;
static int      msi_report_done;

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

/* ---- RESIDUE3 C4 (RES-36): the MSI/MSI-X transport shims ---------------
 *
 * kernel/drivers/pci/msi_caps.c does not know how this driver reaches its
 * config space (port I/O through pci_config_read) or its BARs
 * (map_bar_region), so those are handed to it as an accessor table.  Nothing
 * here makes a decision: it is the same three reads and two writes the
 * capability walk above already does, in the shape the walker expects. */
static uint8_t  msi_cfg8 (void *ctx, uint16_t off) { (void)ctx; return pci_read8_at((uint8_t)off); }
static uint16_t msi_cfg16(void *ctx, uint16_t off) {
    (void)ctx;
    return (uint16_t)(pci_read8_at((uint8_t)off) | (pci_read8_at((uint8_t)(off + 1)) << 8));
}
static uint32_t msi_cfg32(void *ctx, uint16_t off) {
    (void)ctx;
    return pci_config_read(pci_bus, pci_dev, pci_func, (uint8_t)off);
}
static void msi_w16(void *ctx, uint16_t off, uint16_t v) {
    (void)ctx;
    /* The config mechanism exposes dword writes only: read, patch the half,
     * write back.  A torn read-modify-write is not possible here -- this runs
     * with interrupts disabled during probe, and nothing else touches this
     * device's config space. */
    uint32_t cur = pci_config_read(pci_bus, pci_dev, pci_func, (uint8_t)(off & 0xFC));
    unsigned sh = (unsigned)(off & 2u) * 8u;
    uint32_t mask = 0xFFFFu << sh;
    pci_config_write(pci_bus, pci_dev, pci_func, (uint8_t)(off & 0xFC),
                     (cur & ~mask) | ((uint32_t)v << sh));
}
static void msi_w32(void *ctx, uint16_t off, uint32_t v) {
    (void)ctx;
    pci_config_write(pci_bus, pci_dev, pci_func, (uint8_t)off, v);
}
static volatile uint8_t *msi_bar(void *ctx, uint8_t bar, uint32_t off, uint32_t len) {
    (void)ctx;
    uint64_t va = map_bar_region(bar, off, len ? len : 0x1000);
    return (volatile uint8_t *)(uintptr_t)va;
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
                else if (cfg_type == 4) {
                    devcfg_base = (volatile uint8_t *)(uintptr_t)va;
                }
                else if (cfg_type == 3) {
                    isr_base = (volatile uint8_t *)(uintptr_t)va;
                }
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

/* Interrupt handler: the virtio ISR read (read-to-clear) is the part the
 * device cares about -- it deasserts the line / retires the message.  The
 * request path still POLLS the used ring, which is deliberate: making I/O
 * blocking-on-interrupt is a scheduler change (wait queues, IRQ-safe sleep),
 * not a transport change, and this phase is scoped to the transport.  What
 * the counter proves is exactly what the plan asks: the vector is programmed
 * AND the device signals it. */
static void virtio_blk_msi_handler(void *cookie)
{
    (void)cookie;
    if (isr_base) (void)*isr_base;      /* read-to-clear: retires the message */
    msi_irqs++;
}

static void virtio_blk_msi_attach(void) {
    msi_pci_io_t io;
    memset(&io, 0, sizeof(io));
    io.ctx = 0;
    io.cfg8 = msi_cfg8; io.cfg16 = msi_cfg16; io.cfg32 = msi_cfg32;
    io.w16 = msi_w16; io.w32 = msi_w32; io.bar_map = msi_bar;

    uint32_t vec = 0;
    int rc = msi_attach_pci(&io, virtio_blk_msi_handler, 0, "virtio-blk", &vec);
    if (rc != 0) {
        /* Three distinguishable states, because a test lane asserts on each:
         * off by knob (the fw_cfg line already said so -- say nothing),
         * -1 no capability (the honest INTx case), and anything else refused
         * (msi.c printed the reason).  The old code called a disabled build
         * "attach refused", which is the sort of line that makes a log lie. */
        if (rc == -1)
            kprintf("[msi] virtio-blk: no MSI/MSI-X capability -- "
                    "INTx/polling path\n");
        else if (msi_enabled())
            kprintf("[msi] virtio-blk: attach refused (see above) -- "
                    "INTx/polling path\n");
        return;
    }
    msi_vector = vec;

    /* Tell the DEVICE which vector to use for queue 0.  Per the spec the
     * device answers with VIRTIO_MSI_NO_VECTOR (0xFFFF) if it cannot honour
     * the request, and reading back is the only way to know. */
    /* The register takes the MSI-X TABLE INDEX (msi.h: MSI_TABLE_ENTRY),
     * not the x86 vector: the vector is already inside the table entry this
     * driver had programmed.  See msi.h for the measured NO_VECTOR that
     * writing the vector here produces. */
    common_cfg->queue_select = 0;
    common_cfg->queue_msix_vector = (uint16_t)MSI_TABLE_ENTRY;
    uint16_t got = common_cfg->queue_msix_vector;
    if (got == 0xFFFF) {
        kprintf("[msi] virtio-blk: device refused queue 0 vector %u "
                "(NO_VECTOR); keeping INTx\n", (unsigned)vec);
        msi_vector = 0;
        return;
    }
    /* Configuration-change interrupts name the same vector: this driver never
     * relies on them, but leaving the field at NO_VECTOR would mean the one
     * device event we could also see was silently unrouted. */
    common_cfg->msix_config = (uint16_t)MSI_TABLE_ENTRY;
    kprintf("[msi] virtio-blk: queue 0 -> table entry %u (readback %u), "
            "x86 vector %u\n", (unsigned)MSI_TABLE_ENTRY, (unsigned)got,
            (unsigned)vec);
}

/* Forward declarations: init's self-test issues a request, and both the
 * request path and the self-test share the once-only receipt. */
static int  virtio_blk_transfer(uint32_t type, uint64_t lba, uint32_t count, void *buf);
static void msi_receipt(void);

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

    /* Feature negotiation, which this driver used to skip entirely.  A
     * modern (capability-based) transport is only honoured by the device
     * once VIRTIO_F_VERSION_1 (feature bit 32) is acked: without it a
     * transitional virtio-blk stays in legacy I/O-BAR mode, so the common
     * config window accepts writes that mean nothing and the first request
     * never completes.  Measured before this fix: the C4 self-test read
     * failed (status never written) on a device whose probes all looked
     * healthy.  Accept VERSION_1 and, when offered, MSI-X (bit 27) -- the
     * latter is what makes queue_msix_vector meaningful. */
    common_cfg->device_feature_select = 1;
    uint32_t feat_hi = common_cfg->device_feature;
    common_cfg->device_feature_select = 0;
    uint32_t feat_lo = common_cfg->device_feature;

    common_cfg->driver_feature_select = 1;
    common_cfg->driver_feature = feat_hi & 1u;              /* VERSION_1 */
    common_cfg->driver_feature_select = 0;
    common_cfg->driver_feature = feat_lo & (1u << 27);      /* MSI-X */
    msi_offered = (feat_lo >> 27) & 1u;

    common_cfg->device_status |= 8;    // FEATURES_OK
    /* A device that refuses the feature set clears FEATURES_OK here; going
     * on would build rings the device will never look at. */
    if (!(common_cfg->device_status & 8)) {
        kprintf("[virtio-blk] device refused the feature set "
                "(features %08x:%08x); driver will not use it\n",
                feat_hi, feat_lo);
        return -1;
    }
    common_cfg->device_status |= 4;    // DRIVER_OK

    if (setup_queue() != 0) return -1;

    /* Capacity: the modern virtio-blk device-config region (cfg_type 4)
     * holds a le64 sector count at offset 0.  It is read as two dwords on
     * purpose -- config space is not required to tear-free a 64-bit read. */
    if (devcfg_base) {
        uint32_t lo = *(volatile uint32_t *)(devcfg_base + 0);
        uint32_t hi = *(volatile uint32_t *)(devcfg_base + 4);
        device_capacity_sectors = ((uint64_t)hi << 32) | lo;
    } else {
        device_capacity_sectors = 0;      /* unknown: the bound check refuses */
    }

    /* C4: after the queue exists (queue 0 is selected and enabled by
     * setup_queue), attach the interrupt.  Failures leave the device on the
     * polling path it had before this phase. */
    virtio_blk_msi_attach();

    kprintf("[virtio-blk] ready: capacity %llu sectors\n", device_capacity_sectors);

    /* C4 self-test: the request path had no in-tree caller on x86_64, so a
     * phase that claims "interrupts arrive" cannot be proven without issuing
     * a request.  One read-only sector, into a static buffer, so neither the
     * disk nor the arena allocator is disturbed. */
    /* The self-test goes through the public request path, so the device has
     * to be declared usable first -- otherwise virtio_blk_available() is
     * still false and the transfer refuses before it touches the ring. */
    init_result = 0;
    static uint8_t selftest_buf[512];
    if (device_capacity_sectors > 0 &&
        virtio_blk_transfer(VIRTIO_BLK_T_IN, 0, 1, selftest_buf) == 0) {
        for (uint32_t i = 0; i < 200000 && msi_irqs == 0; i++)
            __asm__ volatile ("pause");
        msi_receipt();
        kprintf("[virtio-blk] self-test: LBA 0 read OK (%s)\n",
                msi_vector ? (msi_irqs ? "interrupt delivered" : "no message")
                           : "INTx/polling");
    } else {
        kprintf("[virtio-blk] self-test: LBA 0 read FAILED\n");
    }

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

/* Prints the delivery receipt exactly once.  The message can trail the
 * used-ring write by a few thousand cycles, so the self-test below gives it
 * a grace window before deciding it never arrived. */
static void msi_receipt(void) {
    if (!msi_vector || msi_report_done || msi_irqs == 0) return;
    msi_report_done = 1;
    kprintf("[msi] virtio-blk: %u interrupt(s) delivered on vector %u\n",
            (unsigned)msi_irqs, (unsigned)msi_vector);
}

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

    /* C4 receipt: prove DELIVERY, once, on the first request after the
     * interrupt has actually fired.  Printed from the request path because
     * that is where the device's completion (and therefore its message)
     * happens; printing at probe time would only prove a register was
     * written. */
    msi_receipt();
    return res;
}

int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    return virtio_blk_transfer(VIRTIO_BLK_T_IN, lba, count, buf);
}

int virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    return virtio_blk_transfer(VIRTIO_BLK_T_OUT, lba, count, (void *)(uintptr_t)buf);
}
