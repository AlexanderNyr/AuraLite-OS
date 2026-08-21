/* kernel/drivers/virtio_mmio.h -- the virtio-mmio transport
 * (RISCV_PLAN V7, decision D7; PROMOTED from kernel/arch/riscv64/ in
 * ARM64_PLAN A7 -- the fdt.c treatment: one source file, every
 * MMIO-transport kernel compiles it, the claim checkers assert the
 * single-source linkage).
 *
 * The virt machines' virtio windows (riscv64: 8, aarch64: 32 -- both
 * recorded from the DTB by the shared walker) speak virtio over plain
 * MMIO registers -- no PCI, no capabilities, just a fixed layout at
 * each window (virtio spec 4.2, the "legacy interface" version=1
 * flavour QEMU virt exposes by default, plus version=2 handled where
 * it differs).
 *
 * The virtqueue STRUCTURES (vring_desc/avail/used) come from
 * drivers/virtio/virtio_common.h -- the same header the PCI drivers
 * use; only the transport differs (D7's one-implementation rule).
 *
 * What the promotion changed: the file no longer calls any arch's
 * allocator, console or clock by name.  Each tenant hands over a
 * vmmio_arch_ops table -- and the aarch64 table's mmio_is_device hook
 * turns Fact 5.2's bug class (device registers behind a Normal
 * mapping: reordered, combined, speculated) into a REFUSAL at attach
 * time instead of a convention in a comment.
 */

#ifndef AURALITE_DRIVERS_VIRTIO_MMIO_H
#define AURALITE_DRIVERS_VIRTIO_MMIO_H

#include <stdint.h>

#include "drivers/virtio/virtio_common.h"

/* Register offsets (virtio spec 4.2.2). */
#define VM_MAGIC          0x000   /* 0x74726976 "virt" */
#define VM_VERSION        0x004   /* 1 = legacy, 2 = modern */
#define VM_DEVICE_ID      0x008   /* 1 = net, 2 = blk, 0 = empty */
#define VM_VENDOR_ID      0x00C
#define VM_DEV_FEATURES   0x010
#define VM_DRV_FEATURES   0x020
#define VM_GUEST_PAGE_SZ  0x028   /* legacy only */
#define VM_QUEUE_SEL      0x030
#define VM_QUEUE_NUM_MAX  0x034
#define VM_QUEUE_NUM      0x038
#define VM_QUEUE_ALIGN    0x03C   /* legacy only */
#define VM_QUEUE_PFN      0x040   /* legacy only */
#define VM_QUEUE_READY    0x044   /* modern only */
#define VM_QUEUE_NOTIFY   0x050
#define VM_INT_STATUS     0x060
#define VM_INT_ACK        0x064
#define VM_STATUS         0x070
#define VM_QUEUE_DESC_LO  0x080   /* modern only */
#define VM_QUEUE_DESC_HI  0x084
#define VM_QUEUE_AVAIL_LO 0x090
#define VM_QUEUE_AVAIL_HI 0x094
#define VM_QUEUE_USED_LO  0x0A0
#define VM_QUEUE_USED_HI  0x0A4
#define VM_CONFIG         0x100   /* device-specific config space */

#define VM_MAGIC_VALUE    0x74726976u

#define VM_DEV_NET 1
#define VM_DEV_BLK 2

/* Status bits (identical to the PCI transport's). */
#define VM_S_ACK        1
#define VM_S_DRIVER     2
#define VM_S_DRIVER_OK  4
#define VM_S_FEATURES_OK 8

/* The legacy layout's page granule.  Both tenants run 4 KiB frames
 * (PAGE_SIZE_RV and PAGE_SIZE_A64 agree); this constant is what goes
 * into VM_GUEST_PAGE_SZ and what the vring pad rounds to, so it must
 * equal the granularity of alloc_frame below -- a tenant with a
 * different frame size would need a field here, and none exists. */
#define VMMIO_PAGE 4096u

/* The arch services the transport consumes -- the promotion seam.
 * Everything the rv64 original reached for by name (pmm_rv_alloc_
 * frame, p2v_rv, sbi_puts, rv_rdtime) arrives through this table
 * instead. */
struct vmmio_arch_ops {
    uint64_t (*alloc_frame)(void);   /* one physical frame, 0 = OOM */
    void    *(*p2v)(uint64_t phys);  /* HHDM view of that frame */
    void     (*puts)(const char *s); /* the boot console */
    uint64_t (*ticks)(void);         /* monotonic counter ... */
    uint64_t  ticks_per_sec;         /* ... at this rate (deadlines) */
    /* 1 = this VA is mapped with device attributes (or the arch has
     * no per-PTE attributes to check -- riscv64's Sv39 without
     * Svpbmt leaves memory types to the PMAs, and its hook says so
     * honestly).  0 = Normal mapping: the transport REFUSES the
     * window (aarch64, Fact 5.2 -- prevented by refusal, not
     * convention). */
    int      (*mmio_is_device)(const volatile void *va);
};

/* One probed device: window VA + a single legacy-layout virtqueue.
 * The legacy vring is ONE contiguous allocation (desc, avail, pad to
 * page, used) whose PFN goes in VM_QUEUE_PFN. */
struct vmmio_dev {
    const struct vmmio_arch_ops *ops;
    volatile uint8_t   *base;      /* HHDM VA of the window */
    uint32_t            version;   /* 1 or 2 */
    uint32_t            device_id;
    uint16_t            qsize;
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t            last_used_idx;
    uint16_t            free_head;  /* simple bump allocator, V7 scope */
};

/* Scan `count` windows at bases[] (HHDM VAs) for device_id; on hit,
 * negotiate features (accepting none -- bring-up scope), set up
 * queue 0, and mark DRIVER_OK.  Returns 0 and fills *dev, or -1. */
int vmmio_probe(struct vmmio_dev *dev, const struct vmmio_arch_ops *ops,
                const uint64_t *bases, uint32_t count, uint32_t device_id);

/* Post a descriptor chain (already written into dev->desc[0..n-1],
 * head at index 0) and busy-wait for the used ring to advance.
 * Returns the used length, or -1 on timeout. */
int vmmio_submit_wait(struct vmmio_dev *dev, int ndesc);

/* Read a device-config byte (VM_CONFIG + off). */
uint8_t vmmio_cfg8(struct vmmio_dev *dev, uint32_t off);

#endif /* AURALITE_DRIVERS_VIRTIO_MMIO_H */
