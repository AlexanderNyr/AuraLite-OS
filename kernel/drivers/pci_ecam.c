/* kernel/drivers/pci_ecam.c -- generic-ECAM config access + the bus-0
 * walk (RESIDUE_PLAN R7, ledger RES-20; header for the full story).
 *
 * Everything here is measured against the two DTBs the tenants boot
 * with: rv64 reg=<0x30000000 +0x10000000> mem32 window 0x40000000,
 * aarch64 reg=<0x40_10000000 +0x10000000> mem32 window 0x10000000 --
 * the aarch64 ECAM sits ABOVE 4 GiB, which is why the tenant maps the
 * window and hands a VA in, instead of this file assuming any
 * phys-to-virt formula.
 */

#include <stdint.h>

#include "kernel/drivers/pci_ecam.h"

static volatile uint8_t *cfg(struct pci_ecam *e, uint32_t bdf, uint32_t off)
{
    return e->va + ((uintptr_t)bdf << 12) + off;
}

uint32_t pci_ecam_r32(struct pci_ecam *e, uint32_t bdf, uint32_t off)
{
    return *(volatile uint32_t *)cfg(e, bdf, off);
}
uint16_t pci_ecam_r16(struct pci_ecam *e, uint32_t bdf, uint32_t off)
{
    return *(volatile uint16_t *)cfg(e, bdf, off);
}
uint8_t pci_ecam_r8(struct pci_ecam *e, uint32_t bdf, uint32_t off)
{
    return *(volatile uint8_t *)cfg(e, bdf, off);
}
void pci_ecam_w32(struct pci_ecam *e, uint32_t bdf, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)cfg(e, bdf, off) = v;
}
void pci_ecam_w16(struct pci_ecam *e, uint32_t bdf, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)cfg(e, bdf, off) = v;
}

/* Local decimal/hex printers over ops->puts (the shared-driver rule:
 * no kprintf dependency; same shape vblk_rv.c uses over sbi_puts). */
static void put_udec(const struct vmmio_arch_ops *ops, uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    char s[2] = {0, 0};
    while (i--) { s[0] = buf[i]; ops->puts(s); }
}
static void put_hex16(const struct vmmio_arch_ops *ops, uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    char s[5];
    s[0] = d[(v >> 12) & 15]; s[1] = d[(v >> 8) & 15];
    s[2] = d[(v >> 4) & 15];  s[3] = d[v & 15]; s[4] = 0;
    ops->puts(s);
}

int pci_ecam_walk(struct pci_ecam *e)
{
    const struct vmmio_arch_ops *ops = e->ops;

    /* Attribute gate first: config reads through a Normal mapping are
     * the same reordered/combined bug class the virtio transport
     * refuses (Fact 5.2) -- refuse before the first read. */
    if (!ops->mmio_is_device(e->va)) {
        ops->puts("[pci] ECAM window not Device-mapped -- refusing "
                  "to walk (Fact 5.2)\n");
        return -1;
    }

    int found = 0;
    for (uint32_t dev = 0; dev < 32; dev++) {
        uint32_t fn_limit = 1;
        for (uint32_t fn = 0; fn < fn_limit; fn++) {
            uint32_t bdf = (dev << 3) | fn;
            uint16_t vend = pci_ecam_r16(e, bdf, PCI_CFG_VENDOR);
            if (vend == 0xFFFF)
                continue;
            if (fn == 0 &&
                (pci_ecam_r8(e, bdf, PCI_CFG_HDRTYPE) & 0x80u))
                fn_limit = 8;            /* multi-function: probe all */
            uint16_t did  = pci_ecam_r16(e, bdf, PCI_CFG_DEVICE);
            uint32_t cls  = pci_ecam_r32(e, bdf, PCI_CFG_CLASSREV) >> 8;
            ops->puts("[pci] 00:");
            put_udec(ops, dev);
            ops->puts(".");
            put_udec(ops, fn);
            ops->puts(" ");
            put_hex16(ops, vend);
            ops->puts(":");
            put_hex16(ops, did);
            ops->puts(" class ");
            put_hex16(ops, (cls >> 8) & 0xFFFFu);
            ops->puts("\n");
            found++;
        }
    }
    ops->puts("[pci] ECAM: ");
    put_udec(ops, (uint32_t)found);
    ops->puts(" function(s)\n");
    return found;
}

int pci_ecam_find(struct pci_ecam *e, uint16_t vendor, uint16_t device_lo,
                  uint16_t device_hi)
{
    for (uint32_t dev = 0; dev < 32; dev++) {
        for (uint32_t fn = 0; fn < 8; fn++) {
            uint32_t bdf = (dev << 3) | fn;
            if (pci_ecam_r16(e, bdf, PCI_CFG_VENDOR) != vendor)
                continue;
            uint16_t did = pci_ecam_r16(e, bdf, PCI_CFG_DEVICE);
            if (did >= device_lo && did <= device_hi)
                return (int)bdf;
        }
    }
    return -1;
}

uint64_t pci_ecam_place_bar(struct pci_ecam *e, uint32_t bdf,
                            uint32_t bar_idx, uint32_t *out_size)
{
    uint32_t off = PCI_CFG_BAR0 + bar_idx * 4;
    uint32_t orig = pci_ecam_r32(e, bdf, off);
    if (orig & 1u)
        return 0;                        /* IO BAR: not this window */
    int is64 = ((orig >> 1) & 3u) == 2u;

    pci_ecam_w32(e, bdf, off, 0xFFFFFFFFu);
    uint32_t mask = pci_ecam_r32(e, bdf, off) & 0xFFFFFFF0u;
    if (mask == 0)
        return 0;                        /* unimplemented BAR */
    uint32_t size = ~mask + 1u;

    /* Align the cursor to the size (BAR sizes are powers of two --
     * the mask read just proved this one's). */
    uint64_t szm1 = size - 1u;
    uint64_t cur = (e->mmio_cursor + szm1) & ~szm1;
    if (cur + size > e->mmio_size)
        return 0;                        /* window full: refuse */
    e->mmio_cursor = cur + size;

    uint64_t pci_addr = e->mmio_pci + cur;
    uint32_t lo = (uint32_t)pci_addr;
    pci_ecam_w32(e, bdf, off, lo | (orig & 0xFu));
    if (is64)
        pci_ecam_w32(e, bdf, off + 4, 0);   /* window is below 4G */

    if (out_size)
        *out_size = size;
    return e->mmio_cpu + cur;
}

void pci_ecam_enable(struct pci_ecam *e, uint32_t bdf)
{
    uint16_t cmd = pci_ecam_r16(e, bdf, PCI_CFG_COMMAND);
    pci_ecam_w16(e, bdf, PCI_CFG_COMMAND,
                 (uint16_t)(cmd | PCI_CMD_MEM | PCI_CMD_MASTER));
}
