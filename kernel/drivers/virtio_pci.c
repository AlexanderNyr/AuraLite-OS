/* kernel/drivers/virtio_pci.c -- the modern virtio-pci transport
 * (RESIDUE_PLAN R7, ledger RES-21; the header carries the contract).
 *
 * Shape notes, measured against QEMU's virtio-blk-pci:
 *   - the device is transitional (0x1af4:0x1001, revision 0) and all
 *     four vendor capabilities point into ONE 64-bit memory BAR
 *     (BAR4, 16 KiB) -- the code still walks the capability list and
 *     places every referenced BAR, because "it was BAR4 today" is a
 *     fact about one QEMU version, not a contract;
 *   - -kernel boots arrive with BARs unwritten (all zeros) and
 *     memory decode off: placement is OURS (pci_ecam_place_bar);
 *   - FEATURES_OK arrives only after VERSION_1 is acked -- verified
 *     by reading it back, refusal is loud.
 */

#include <stdint.h>

#include "kernel/drivers/virtio_pci.h"

static inline uint32_t c32r(struct vpci_dev *d, uint32_t off)
{
    return *(volatile uint32_t *)(d->common + off);
}
static inline void c32w(struct vpci_dev *d, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(d->common + off) = v;
}
static inline uint16_t c16r(struct vpci_dev *d, uint32_t off)
{
    return *(volatile uint16_t *)(d->common + off);
}
static inline void c16w(struct vpci_dev *d, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(d->common + off) = v;
}
static inline uint8_t c8r(struct vpci_dev *d, uint32_t off)
{
    return *(volatile uint8_t *)(d->common + off);
}
static inline void c8w(struct vpci_dev *d, uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)(d->common + off) = v;
}

static inline void vpci_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

int vpci_blk_probe(struct vpci_dev *d, const struct vmmio_arch_ops *ops,
                   struct pci_ecam *e,
                   volatile uint8_t *(*map_mmio)(uint64_t pa, uint32_t len))
{
    int bdf_or = pci_ecam_find(e, 0x1AF4, 0x1001, 0x1001);
    if (bdf_or < 0)
        bdf_or = pci_ecam_find(e, 0x1AF4, 0x1042, 0x1042);
    if (bdf_or < 0)
        return -1;                       /* absent: not an error */
    uint32_t bdf = (uint32_t)bdf_or;

    if (!(pci_ecam_r16(e, bdf, PCI_CFG_STATUS) & PCI_STATUS_CAPS)) {
        ops->puts("[vpci] no capability list -- legacy-only device, "
                  "refusing (modern transport)\n");
        return -1;
    }

    /* Walk the vendor caps; remember bar/offset per cfg_type. */
    uint8_t cap_bar[5] = {0, 0, 0, 0, 0};
    uint32_t cap_off[5] = {0, 0, 0, 0, 0};
    int cap_seen[5] = {0, 0, 0, 0, 0};
    uint32_t notify_mult = 0;
    uint8_t ptr = pci_ecam_r8(e, bdf, PCI_CFG_CAP_PTR);
    int fuse = 48;
    while (ptr && fuse--) {
        uint8_t id = pci_ecam_r8(e, bdf, ptr);
        uint8_t nxt = pci_ecam_r8(e, bdf, ptr + 1u);
        if (id == 0x09) {                /* vendor-specific: virtio */
            uint8_t t = pci_ecam_r8(e, bdf, ptr + 3u);
            if (t >= VPCI_CAP_COMMON && t <= VPCI_CAP_DEVICE &&
                pci_ecam_r8(e, bdf, ptr + 4u) <= 5) {
                cap_bar[t] = pci_ecam_r8(e, bdf, ptr + 4u);
                cap_off[t] = pci_ecam_r32(e, bdf, ptr + 8u);
                cap_seen[t] = 1;
                if (t == VPCI_CAP_NOTIFY)
                    notify_mult = pci_ecam_r32(e, bdf, ptr + 16u);
            }
        }
        ptr = nxt;
    }
    if (!cap_seen[VPCI_CAP_COMMON] || !cap_seen[VPCI_CAP_NOTIFY] ||
        !cap_seen[VPCI_CAP_ISR] || !cap_seen[VPCI_CAP_DEVICE]) {
        ops->puts("[vpci] virtio caps incomplete -- refusing\n");
        return -1;
    }

    /* Place every referenced BAR once, map it, remember the VA. */
    volatile uint8_t *bar_va[6] = {0, 0, 0, 0, 0, 0};
    for (int t = VPCI_CAP_COMMON; t <= VPCI_CAP_DEVICE; t++) {
        uint8_t b = cap_bar[t];
        if (bar_va[b])
            continue;
        uint32_t sz = 0;
        uint64_t pa = pci_ecam_place_bar(e, bdf, b, &sz);
        if (!pa) {
            ops->puts("[vpci] BAR placement refused -- no window\n");
            return -1;
        }
        bar_va[b] = map_mmio(pa, sz);
        if (!bar_va[b] || !ops->mmio_is_device(bar_va[b])) {
            ops->puts("[vpci] BAR not Device-mapped -- refusing "
                      "(Fact 5.2)\n");
            return -1;
        }
    }
    pci_ecam_enable(e, bdf);

    d->ops         = ops;
    d->common      = bar_va[cap_bar[VPCI_CAP_COMMON]]
                     + cap_off[VPCI_CAP_COMMON];
    d->notify_base = bar_va[cap_bar[VPCI_CAP_NOTIFY]]
                     + cap_off[VPCI_CAP_NOTIFY];
    d->notify_mult = notify_mult;
    d->isr         = bar_va[cap_bar[VPCI_CAP_ISR]]
                     + cap_off[VPCI_CAP_ISR];
    d->devcfg      = bar_va[cap_bar[VPCI_CAP_DEVICE]]
                     + cap_off[VPCI_CAP_DEVICE];
    d->last_used_idx = 0;

    /* Reset, then the status dance -- same bits, new doorbells. */
    c8w(d, VPC_STATUS, 0);
    int spin = 1000000;
    while (c8r(d, VPC_STATUS) != 0 && spin--)
        ;
    c8w(d, VPC_STATUS, VM_S_ACK);
    c8w(d, VPC_STATUS, VM_S_ACK | VM_S_DRIVER);

    /* VERSION_1 (bit 32 = word 1 bit 0) offered?  Ack exactly it. */
    c32w(d, VPC_DFSELECT, 1);
    if (!(c32r(d, VPC_DFEATURE) & 1u)) {
        ops->puts("[vpci] device does not offer VERSION_1 -- "
                  "refusing (modern transport)\n");
        return -1;
    }
    c32w(d, VPC_GFSELECT, 0);
    c32w(d, VPC_GFEATURE, 0);
    c32w(d, VPC_GFSELECT, 1);
    c32w(d, VPC_GFEATURE, 1);
    c8w(d, VPC_STATUS, VM_S_ACK | VM_S_DRIVER | VM_S_FEATURES_OK);
    if (!(c8r(d, VPC_STATUS) & VM_S_FEATURES_OK)) {
        ops->puts("[vpci] FEATURES_OK refused -- device unusable\n");
        return -1;
    }

    /* Queue 0: split ring, one frame per part (the modern-mmio
     * shape; 128 entries fit each part in one 4 KiB frame). */
    c16w(d, VPC_QSELECT, 0);
    uint16_t maxq = c16r(d, VPC_QSIZE);
    if (maxq == 0) {
        ops->puts("[vpci] queue 0 absent -- refusing\n");
        return -1;
    }
    d->qsize = (maxq < 128) ? maxq : 128;
    c16w(d, VPC_QSIZE, d->qsize);

    uint64_t desc_phys  = ops->alloc_frame();
    uint64_t avail_phys = ops->alloc_frame();
    uint64_t used_phys  = ops->alloc_frame();
    d->dma_phys         = ops->alloc_frame();
    if (!desc_phys || !avail_phys || !used_phys || !d->dma_phys)
        return -1;
    d->desc  = ops->p2v(desc_phys);
    d->avail = ops->p2v(avail_phys);
    d->used  = ops->p2v(used_phys);
    d->dma   = ops->p2v(d->dma_phys);
    for (int i = 0; i < 4096; i++) {
        ((uint8_t *)d->desc)[i] = 0;
        ((uint8_t *)d->avail)[i] = 0;
        ((uint8_t *)d->used)[i] = 0;
    }

    c32w(d, VPC_QDESC_LO,  (uint32_t)desc_phys);
    c32w(d, VPC_QDESC_HI,  (uint32_t)(desc_phys >> 32));
    c32w(d, VPC_QAVAIL_LO, (uint32_t)avail_phys);
    c32w(d, VPC_QAVAIL_HI, (uint32_t)(avail_phys >> 32));
    c32w(d, VPC_QUSED_LO,  (uint32_t)used_phys);
    c32w(d, VPC_QUSED_HI,  (uint32_t)(used_phys >> 32));

    uint16_t qoff = c16r(d, VPC_QNOTIFY_OFF);
    d->notify_q0 = (volatile uint16_t *)
        (d->notify_base + (uintptr_t)qoff * d->notify_mult);

    c16w(d, VPC_QENABLE, 1);
    c8w(d, VPC_STATUS, VM_S_ACK | VM_S_DRIVER | VM_S_FEATURES_OK |
                       VM_S_DRIVER_OK);

    /* blk config: capacity le64 at offset 0 (two dword reads -- the
     * spec forbids pretending 64-bit config access is atomic). */
    uint64_t cap_lo = *(volatile uint32_t *)(d->devcfg + 0);
    uint64_t cap_hi = *(volatile uint32_t *)(d->devcfg + 4);
    d->capacity = (cap_hi << 32) | cap_lo;
    d->last_used_idx = d->used->idx;
    return 0;
}

/* virtio-blk request header (spec 5.2.6) -- transport-agnostic, the
 * exact struct the mmio path posts. */
#define VPCI_BLK_T_IN  0
#define VPCI_BLK_T_OUT 1
#define VPCI_BLK_S_OK  0

struct vpci_blk_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

int vpci_blk_rw(struct vpci_dev *d, int is_write, uint64_t lba,
                uint8_t *buf512)
{
    if (!d->notify_q0 || lba >= d->capacity)
        return -1;

    struct vpci_blk_hdr *hdr = (struct vpci_blk_hdr *)d->dma;
    uint8_t *data   = d->dma + 512;
    uint8_t *status = d->dma + 1024;

    hdr->type = is_write ? VPCI_BLK_T_OUT : VPCI_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = lba;
    *status = 0xFF;
    if (is_write)
        for (int i = 0; i < 512; i++)
            data[i] = buf512[i];

    d->desc[0].addr  = d->dma_phys;
    d->desc[0].len   = sizeof(*hdr);
    d->desc[0].flags = VRING_DESC_F_NEXT;
    d->desc[0].next  = 1;
    d->desc[1].addr  = d->dma_phys + 512;
    d->desc[1].len   = 512;
    d->desc[1].flags = VRING_DESC_F_NEXT |
                       (is_write ? 0 : VRING_DESC_F_WRITE);
    d->desc[1].next  = 2;
    d->desc[2].addr  = d->dma_phys + 1024;
    d->desc[2].len   = 1;
    d->desc[2].flags = VRING_DESC_F_WRITE;
    d->desc[2].next  = 0;

    uint16_t before = d->last_used_idx;
    d->avail->ring[d->avail->idx % d->qsize] = 0;
    vpci_fence();
    d->avail->idx++;
    vpci_fence();
    *d->notify_q0 = 0;

    uint64_t deadline = d->ops->ticks() + d->ops->ticks_per_sec;
    while (d->used->idx == before) {
        if (d->ops->ticks() > deadline)
            return -1;
        vpci_fence();
    }
    d->last_used_idx = d->used->idx;

    /* Read ISR once: read-to-clear, keeps the INTx line deasserted
     * (we poll, but a stuck line is somebody's future bug). */
    (void)*d->isr;

    if (*status != VPCI_BLK_S_OK)
        return -1;
    if (!is_write)
        for (int i = 0; i < 512; i++)
            buf512[i] = data[i];
    return 0;
}
