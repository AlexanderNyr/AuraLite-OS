/* kernel/arch/riscv64/virtio_mmio.c -- the virtio-mmio transport
 * (RISCV_PLAN V7).  See the header for the register map and the D7
 * one-implementation story.
 *
 * QEMU virt's windows are legacy (version=1) by default: the vring
 * is one contiguous physically-addressed block published through
 * QUEUE_PFN, with the guest's page size declared first.  version=2
 * (modern) publishes desc/avail/used separately -- both paths are
 * here because -global virtio-mmio.force-legacy=false is one QEMU
 * flag away, and probing is cheaper than folklore (the V0 DBCN
 * lesson generalised).
 */

#include <stdint.h>

#include "kernel/arch/riscv64/virtio_mmio.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/trap.h"

static inline uint32_t rd32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}
static inline void wr32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

uint8_t vmmio_cfg8(struct vmmio_dev *dev, uint32_t off)
{
    return *(volatile uint8_t *)(dev->base + VM_CONFIG + off);
}

/* The legacy vring layout (virtio 0.9.5 s.2.3): desc[qsize], then
 * avail (flags/idx/ring[qsize]/used_event), then pad to QUEUE_ALIGN,
 * then used.  One block, one PFN. */
static int setup_queue_legacy(struct vmmio_dev *dev)
{
    uint32_t qsize = dev->qsize;
    uint32_t desc_bytes  = 16 * qsize;
    uint32_t avail_bytes = 6 + 2 * qsize;
    uint32_t used_off    = (desc_bytes + avail_bytes + PAGE_SIZE_RV - 1)
                           & ~(PAGE_SIZE_RV - 1);
    uint32_t used_bytes  = 6 + 8 * qsize;
    uint32_t total       = used_off + used_bytes;

    /* Contiguous frames: the V7 queues fit 2 pages at qsize<=128;
     * take pages one by one and require adjacency (the bring-up PMM
     * has no multi-frame API; adjacency holds on a fresh boot and is
     * CHECKED, not assumed). */
    uint32_t npages = (total + PAGE_SIZE_RV - 1) / PAGE_SIZE_RV;
    uint64_t first = pmm_rv_alloc_frame();
    if (!first)
        return -1;
    uint64_t prev = first;
    for (uint32_t i = 1; i < npages; i++) {
        uint64_t f = pmm_rv_alloc_frame();
        if (f != prev + PAGE_SIZE_RV) {
            /* Non-adjacent: give up loudly rather than corrupt. */
            sbi_puts("[vmmio] queue alloc not contiguous -- refusing\n");
            return -1;
        }
        prev = f;
    }

    uint8_t *ring = (uint8_t *)p2v_rv(first);
    for (uint32_t i = 0; i < total; i++)
        ring[i] = 0;

    dev->desc  = (struct vring_desc *)ring;
    dev->avail = (struct vring_avail *)(ring + desc_bytes);
    dev->used  = (struct vring_used *)(ring + used_off);

    wr32((volatile uint8_t *)dev->base, VM_GUEST_PAGE_SZ, PAGE_SIZE_RV);
    wr32((volatile uint8_t *)dev->base, VM_QUEUE_SEL, 0);
    wr32((volatile uint8_t *)dev->base, VM_QUEUE_NUM, qsize);
    wr32((volatile uint8_t *)dev->base, VM_QUEUE_ALIGN, PAGE_SIZE_RV);
    wr32((volatile uint8_t *)dev->base, VM_QUEUE_PFN,
         (uint32_t)(first / PAGE_SIZE_RV));
    return 0;
}

static int setup_queue_modern(struct vmmio_dev *dev)
{
    uint32_t qsize = dev->qsize;
    uint64_t desc_phys  = pmm_rv_alloc_frame();
    uint64_t avail_phys = pmm_rv_alloc_frame();
    uint64_t used_phys  = pmm_rv_alloc_frame();
    if (!desc_phys || !avail_phys || !used_phys)
        return -1;

    dev->desc  = (struct vring_desc *)p2v_rv(desc_phys);
    dev->avail = (struct vring_avail *)p2v_rv(avail_phys);
    dev->used  = (struct vring_used *)p2v_rv(used_phys);
    for (int i = 0; i < 4096; i++) {
        ((uint8_t *)dev->desc)[i] = 0;
        ((uint8_t *)dev->avail)[i] = 0;
        ((uint8_t *)dev->used)[i] = 0;
    }

    volatile uint8_t *b = dev->base;
    wr32((volatile uint8_t *)b, VM_QUEUE_SEL, 0);
    wr32((volatile uint8_t *)b, VM_QUEUE_NUM, qsize);
    wr32((volatile uint8_t *)b, VM_QUEUE_DESC_LO, (uint32_t)desc_phys);
    wr32((volatile uint8_t *)b, VM_QUEUE_DESC_HI, (uint32_t)(desc_phys >> 32));
    wr32((volatile uint8_t *)b, VM_QUEUE_AVAIL_LO, (uint32_t)avail_phys);
    wr32((volatile uint8_t *)b, VM_QUEUE_AVAIL_HI, (uint32_t)(avail_phys >> 32));
    wr32((volatile uint8_t *)b, VM_QUEUE_USED_LO, (uint32_t)used_phys);
    wr32((volatile uint8_t *)b, VM_QUEUE_USED_HI, (uint32_t)(used_phys >> 32));
    wr32((volatile uint8_t *)b, VM_QUEUE_READY, 1);
    return 0;
}

int vmmio_probe(struct vmmio_dev *dev, uint64_t *bases, uint32_t count,
                uint32_t device_id)
{
    for (uint32_t i = 0; i < count; i++) {
        volatile uint8_t *b = (volatile uint8_t *)bases[i];
        if (rd32((volatile uint8_t *)b, VM_MAGIC) != VM_MAGIC_VALUE)
            continue;
        uint32_t ver = rd32((volatile uint8_t *)b, VM_VERSION);
        if (ver != 1 && ver != 2)
            continue;
        if (rd32((volatile uint8_t *)b, VM_DEVICE_ID) != device_id)
            continue;

        dev->base      = b;
        dev->version   = ver;
        dev->device_id = device_id;
        dev->last_used_idx = 0;
        dev->free_head = 0;

        /* Reset, then the status dance (same bits as the PCI path). */
        wr32((volatile uint8_t *)b, VM_STATUS, 0);
        wr32((volatile uint8_t *)b, VM_STATUS, VM_S_ACK);
        wr32((volatile uint8_t *)b, VM_STATUS, VM_S_ACK | VM_S_DRIVER);

        /* Feature negotiation: accept NONE (bring-up scope -- the
         * legacy layouts below assume no EVENT_IDX etc.). */
        wr32((volatile uint8_t *)b, VM_DRV_FEATURES, 0);
        if (ver == 2) {
            uint32_t st = rd32((volatile uint8_t *)b, VM_STATUS);
            wr32((volatile uint8_t *)b, VM_STATUS, st | VM_S_FEATURES_OK);
            if (!(rd32((volatile uint8_t *)b, VM_STATUS) & VM_S_FEATURES_OK)) {
                sbi_puts("[vmmio] FEATURES_OK refused -- device unusable\n");
                continue;
            }
        }

        wr32((volatile uint8_t *)b, VM_QUEUE_SEL, 0);
        uint32_t maxq = rd32((volatile uint8_t *)b, VM_QUEUE_NUM_MAX);
        if (maxq == 0)
            continue;
        dev->qsize = (uint16_t)(maxq < 128 ? maxq : 128);

        int rc = (ver == 1) ? setup_queue_legacy(dev)
                            : setup_queue_modern(dev);
        if (rc != 0)
            continue;

        uint32_t st = rd32((volatile uint8_t *)b, VM_STATUS);
        wr32((volatile uint8_t *)b, VM_STATUS, st | VM_S_DRIVER_OK);
        dev->last_used_idx = dev->used->idx;
        return 0;
    }
    return -1;
}

int vmmio_submit_wait(struct vmmio_dev *dev, int ndesc)
{
    (void)ndesc;
    uint16_t before = dev->last_used_idx;

    /* Publish: head (index 0) into the avail ring, fence so the
     * device sees the descriptors before the index bump, notify. */
    dev->avail->ring[dev->avail->idx % dev->qsize] = 0;
    __asm__ volatile("fence rw, rw" ::: "memory");
    dev->avail->idx++;
    __asm__ volatile("fence rw, rw" ::: "memory");
    wr32((volatile uint8_t *)dev->base, VM_QUEUE_NOTIFY, 0);

    /* Busy-wait bounded by wall clock (~1 s at 10 MHz timebase). */
    uint64_t deadline = rv_rdtime() + 10000000;
    while (dev->used->idx == before) {
        if (rv_rdtime() > deadline)
            return -1;
        __asm__ volatile("fence rw, rw" ::: "memory");
    }
    dev->last_used_idx = dev->used->idx;

    /* Ack the interrupt bit so the line does not stay asserted (we
     * poll in V7, but the PLIC line is enabled for the NIC later --
     * a stuck INT_STATUS would wedge it). */
    uint32_t is = rd32((volatile uint8_t *)dev->base, VM_INT_STATUS);
    if (is)
        wr32((volatile uint8_t *)dev->base, VM_INT_ACK, is);

    return (int)dev->used->ring[(dev->last_used_idx - 1) % dev->qsize].len;
}
