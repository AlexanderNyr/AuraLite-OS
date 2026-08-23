/* kernel/dt/fdt.c -- minimal FDT parser (RISCV_PLAN V1; promoted from
 * kernel/arch/riscv64/ in ARM64_PLAN A1 -- see fdt.h for the terms).
 *
 * THE BYTE-ORDER FILE.  Everything in a flattened device tree --
 * header fields, token stream, property values -- is stored
 * BIG-endian, on a little-endian CPU, in a little-endian kernel.
 * This is the one place on either port where byte order bites (V0
 * already proved the magic reads 0xD00DFEED only through be32).
 * Every multi-byte read in this file goes through be32()/be64()
 * below; a bare load of DTB memory anywhere in this file is a bug by
 * definition.
 *
 * Structure of a DTB (devicetree spec v0.4, chapter 5):
 *
 *   header (40 bytes, 10 be32 fields)
 *   memory reservation block  (pairs of be64 address/size, 0/0 ends)
 *   structure block           (be32 tokens: BEGIN_NODE/END_NODE/PROP/END)
 *   strings block             (property NAMES, NUL-terminated, offsets
 *                              from PROP tokens point in here)
 *
 * The walk is single-pass and bounds-checked against totalsize: a
 * malformed tree returns FDT_ERR_*, it never reads outside
 * [dtb, dtb+totalsize).  QEMU's trees are trusted in practice but the
 * parser doesn't know that -- it now walks two different boards'
 * trees and wants to walk trees from boards we have never seen.
 */

#include <stdint.h>

#include "kernel/dt/fdt.h"

/* ---- big-endian reads (the whole point of this file) ------------------- */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    /* Widen by assignment, not by cast: this file is portable code
     * now (ratchet 1 counts it since the A1 promotion), and the
     * ratchet's rule -- casts are for addresses via paddr_t/uintptr_t,
     * widths widen implicitly -- applies to the walker like anything
     * else in kernel/. */
    uint64_t hi = be32(p);

    return (hi << 32) | be32(p + 4);
}

/* ---- tokens and header layout (spec names, spec values) ---------------- */

#define FDT_MAGIC_V     0xD00DFEEDu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

/* Header field offsets (all be32). */
#define H_MAGIC          0
#define H_TOTALSIZE      4
#define H_OFF_STRUCT     8
#define H_OFF_STRINGS   12
#define H_OFF_RSVMAP    16
#define H_VERSION       20
#define H_LAST_COMP     24

/* GIC `interrupts` cell 0 (the type) -- devicetree GIC binding. */
#define GIC_CELL_SPI 0u
#define GIC_CELL_PPI 1u

/* ---- tiny string helpers (no libc in the kernel) ------------------------ */

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Does `name` start with `prefix` followed by end-of-string or '@'?
 * Node names carry unit addresses ("memory@80000000"); matching must
 * not care about them. */
static int node_is(const char *name, const char *prefix)
{
    while (*prefix && *name == *prefix) { name++; prefix++; }
    return *prefix == 0 && (*name == 0 || *name == '@');
}

/* A `compatible` property is a list of NUL-terminated strings packed
 * back to back; a driver matches if ANY entry matches. */
static int compat_has(const char *list, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        if (streq(list + i, want))
            return 1;
        while (i < len && list[i] != 0)
            i++;
        i++;                              /* skip the NUL */
    }
    return 0;
}

/* ---- interrupt normalisation (ARM64_PLAN A1) -----------------------------
 *
 * Drivers see FINAL interrupt numbers only; the off-by-32 lives and
 * dies in this one function.  PLIC trees: `interrupts` is one raw
 * cell, already final.  GIC trees: 3 cells <type nr flags>, and the
 * hardware INTID is nr+32 for SPIs, nr+16 for PPIs -- the DTB counts
 * within each type's own space, the GICD registers count from 0
 * across all of them.  A driver that adds 32 itself is the bug class
 * this function exists to prevent. */
static uint32_t irq_normalise(uint32_t intc_kind,
                              const uint8_t *val, uint32_t len)
{
    if (intc_kind == FDT_INTC_GIC && len >= 12) {
        uint32_t type = be32(val), nr = be32(val + 4);
        if (type == GIC_CELL_SPI)
            return nr + 32;
        if (type == GIC_CELL_PPI)
            return nr + 16;
        return nr;                        /* extended/other: raw */
    }
    if (len >= 4)
        return be32(val);                 /* PLIC: one raw cell */
    return 0;
}

/* ---- mmap helpers ------------------------------------------------------- */

static void mmap_add(boot_info_t *bi, uint64_t base, uint64_t len,
                     uint32_t type)
{
    if (len == 0 || bi->mmap_count >= BOOT_MAX_MMAP)
        return;
    boot_mmap_entry_t *e = &bi->mmap[bi->mmap_count++];
    e->base   = base;
    e->length = len;
    e->type   = type;
    e->_pad   = 0;
}

/* ---- the walk ------------------------------------------------------------
 *
 * State machine over nesting depth.  The tree shapes this walker has
 * actually met (riscv virt / aarch64 virt; structure is the spec's,
 * only the leaves differ):
 *
 *   /                     -- #address-cells / #size-cells for its children
 *     chosen              -- bootargs, linux,initrd-{start,end}
 *     memory@X            -- device_type "memory", reg = RAM banks
 *     reserved-memory     -- children's reg = firmware carve-outs
 *     cpus                -- children cpu@N, device_type "cpu"
 *     psci                -- method "hvc"/"smc" (aarch64)
 *     soc                 -- riscv: #cells for ITS children
 *       uart@X            --   compatible "ns16550a"
 *       plic@X            --   compatible "riscv,plic0"/"sifive,plic-1.0.0"
 *       virtio_mmio@X     --   compatible "virtio,mmio"
 *     pl011@X             -- aarch64: compatible "arm,pl011" (at depth 1;
 *     intc@X              --   the virt board has no soc wrapper --
 *     virtio_mmio@X       --   the depth-1/depth-2 split is why device
 *                              matching is depth-independent below)
 *
 * reg parsing needs the PARENT's cell counts, so the walk keeps a
 * small per-depth stack of them (default 2/1 per spec s.2.3.5 when a
 * parent doesn't say).
 */

#define MAX_DEPTH 16

int fdt_parse(uint64_t dtb_phys, uint64_t boot_hartid,
              boot_info_t *bi, fdt_platform_t *plat)
{
    /* How a physical DTB address becomes a pointer is the consuming
     * arch's business (riscv64: HHDM -- boot.S turned Sv39 on before
     * any C ran; aarch64 A1: identity, MMU off until A3): contract 1
     * in fdt.h.  bootargs points into this buffer and outlives the
     * call, so the arch must keep the mapping alive. */
    const uint8_t *dtb = (const uint8_t *)dt_phys_to_virt(dtb_phys);

    /* -- header ---------------------------------------------------- */
    if (be32(dtb + H_MAGIC) != FDT_MAGIC_V)
        return FDT_ERR_MAGIC;
    if (be32(dtb + H_LAST_COMP) > 17)
        return FDT_ERR_VERSION;

    uint32_t totalsize   = be32(dtb + H_TOTALSIZE);
    uint32_t off_struct  = be32(dtb + H_OFF_STRUCT);
    uint32_t off_strings = be32(dtb + H_OFF_STRINGS);
    uint32_t off_rsvmap  = be32(dtb + H_OFF_RSVMAP);

    if (off_struct >= totalsize || off_strings >= totalsize ||
        off_rsvmap >= totalsize)
        return FDT_ERR_BOUNDS;

    /* -- zero the outputs; bi->magic stays 0 until the very end ----- */
    uint8_t *z = (uint8_t *)bi;
    for (uint32_t i = 0; i < sizeof(*bi); i++) z[i] = 0;
    z = (uint8_t *)plat;
    for (uint32_t i = 0; i < sizeof(*plat); i++) z[i] = 0;

    /* -- memory reservation block: firmware carve-outs --------------
     * (OpenSBI usually reserves itself via /reserved-memory instead,
     * but the spec block is cheap to honour and some firmware uses it.) */
    for (uint32_t o = off_rsvmap; o + 16 <= totalsize; o += 16) {
        uint64_t base = be64(dtb + o), size = be64(dtb + o + 8);
        if (base == 0 && size == 0)
            break;
        mmap_add(bi, base, size, BOOT_MEM_RESERVED);
    }

    /* -- structure walk --------------------------------------------- */
    uint32_t o = off_struct;
    int depth = -1;                      /* becomes 0 at the root node */

    uint32_t acells[MAX_DEPTH], scells[MAX_DEPTH];

    /* What kind of node the walk is inside, tracked by depth. */
    int in_chosen = -1, in_memory = -1, in_resv = -1, in_cpus = -1;
    int in_psci = -1;
    int node_is_cpu = 0;

    /* reg of the current node, decoded lazily once compatible is seen
     * (PROP order in a node is not guaranteed; remember both).  The
     * SAME deferral applies to `interrupts`: normalisation needs
     * intc_kind, which may be discovered after the device node --
     * so the RAW property is remembered per device and normalised at
     * `done:`, when the controller kind is settled.
     *
     * Per-DEPTH, not per-walk: a device node can have children (the
     * aarch64 GIC carries a v2m@ child), and a single cur_dev would
     * be wiped by the child's END_NODE before the parent's own
     * END_NODE pairs reg with compatible.  Measured on the first A1
     * boot -- gicd came out 0 -- and exactly the kind of riscv-shaped
     * assumption the promotion exists to flush out: the riscv virt
     * tree has no device nodes with children, so a single scalar
     * passed V1's gates for a year. */
    enum dev_kind { DEV_NONE, DEV_UART, DEV_PLIC, DEV_GIC, DEV_VIRTIO,
                    DEV_PCIE, DEV_FWCFG };
    uint64_t cur_reg_base = 0;
    const uint8_t *nreg[MAX_DEPTH];
    uint32_t nreg_len[MAX_DEPTH];
    const uint8_t *nranges[MAX_DEPTH];   /* R7: the pcie node's ranges */
    uint32_t nranges_len[MAX_DEPTH];
    const uint8_t *nirq_raw[MAX_DEPTH];
    uint32_t nirq_len[MAX_DEPTH];
    enum dev_kind ndev[MAX_DEPTH];
    for (int i = 0; i < MAX_DEPTH; i++) {
        nreg[i] = 0; nreg_len[i] = 0; nranges[i] = 0; nranges_len[i] = 0;
        nirq_raw[i] = 0; nirq_len[i] = 0; ndev[i] = DEV_NONE;
    }

    /* Deferred raw `interrupts` for the devices that keep theirs. */
    const uint8_t *uart_irq_raw = 0;   uint32_t uart_irq_len = 0;
    const uint8_t *vio_irq_raw[FDT_MAX_VIRTIO];
    uint32_t vio_irq_len[FDT_MAX_VIRTIO];
    for (uint32_t i = 0; i < FDT_MAX_VIRTIO; i++) {
        vio_irq_raw[i] = 0;
        vio_irq_len[i] = 0;
    }

    uint64_t initrd_start = 0, initrd_end = 0;

    while (o + 4 <= totalsize) {
        uint32_t tok = be32(dtb + o);
        o += 4;

        if (tok == FDT_NOP)
            continue;

        if (tok == FDT_END)
            goto done;

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(dtb + o);
            uint32_t n = 0;
            while (o + n < totalsize && dtb[o + n] != 0)
                n++;
            if (o + n >= totalsize)
                return FDT_ERR_BOUNDS;
            o = (o + n + 1 + 3) & ~3u;   /* skip name, align 4 */

            depth++;
            if (depth >= MAX_DEPTH)
                return FDT_ERR_BOUNDS;
            /* Children default to the spec's 2/1 unless the node says
             * otherwise (a property seen later this node overrides). */
            acells[depth] = 2;
            scells[depth] = 1;

            if (depth == 1) {
                if (node_is(name, "chosen"))          in_chosen = depth;
                else if (node_is(name, "memory"))     in_memory = depth;
                else if (node_is(name, "reserved-memory")) in_resv = depth;
                else if (node_is(name, "cpus"))       in_cpus  = depth;
                else if (node_is(name, "psci"))       in_psci  = depth;
            }
            node_is_cpu = (in_cpus >= 0 && node_is(name, "cpu"));
            nreg[depth] = 0; nreg_len[depth] = 0;
            nranges[depth] = 0; nranges_len[depth] = 0;
            nirq_raw[depth] = 0; nirq_len[depth] = 0;
            ndev[depth] = DEV_NONE;
            continue;
        }

        if (tok == FDT_END_NODE) {
            /* Leaving a device node: pair ITS OWN reg with what ITS
             * OWN compatible said -- per-depth state, so a child
             * node's exit (the GIC's v2m@) cannot wipe the parent's. */
            enum dev_kind dv = (depth >= 0) ? ndev[depth] : DEV_NONE;
            const uint8_t *rg = (depth >= 0) ? nreg[depth] : 0;
            if (dv != DEV_NONE && rg) {
                /* reg decodes with the PARENT's cells. */
                uint32_t ac = acells[depth > 0 ? depth - 1 : 0];
                cur_reg_base = (ac == 2) ? be64(rg) : be32(rg);
                if (dv == DEV_UART && plat->uart_base == 0) {
                    plat->uart_base = cur_reg_base;
                    uart_irq_raw = nirq_raw[depth];
                    uart_irq_len = nirq_len[depth];
                } else if (dv == DEV_PLIC && plat->plic_base == 0) {
                    plat->plic_base = cur_reg_base;
                    plat->intc_kind = FDT_INTC_PLIC;
                } else if (dv == DEV_GIC && plat->gicd_base == 0) {
                    /* GICv2 reg = <GICD base size> <GICC base size>,
                     * decoded with the parent's cells; on the virt
                     * board that is 2/2, and both live in one prop. */
                    uint32_t sc = scells[depth > 0 ? depth - 1 : 0];
                    uint32_t one = (ac + sc) * 4;
                    plat->gicd_base = cur_reg_base;
                    plat->gicc_base = (ac == 2) ? be64(rg + one)
                                                : be32(rg + one);
                    plat->intc_kind = FDT_INTC_GIC;
                } else if (dv == DEV_VIRTIO &&
                           plat->virtio_count < FDT_MAX_VIRTIO) {
                    uint32_t i = plat->virtio_count++;
                    plat->virtio_base[i] = cur_reg_base;
                    vio_irq_raw[i] = nirq_raw[depth];
                    vio_irq_len[i] = nirq_len[depth];
                } else if (dv == DEV_PCIE && plat->pcie_ecam_base == 0) {
                    /* R7: reg = <ECAM base size> with the PARENT's
                     * cells (both boards: 2/2).  The SIZE matters
                     * here -- it bounds the bus walk (1 MiB per
                     * bus). */
                    uint32_t sc = scells[depth > 0 ? depth - 1 : 0];
                    if (nreg_len[depth] < (ac + sc) * 4)
                        goto pcie_done;  /* truncated reg: no ECAM */
                    plat->pcie_ecam_base = cur_reg_base;
                    plat->pcie_ecam_size =
                        (sc == 2) ? be64(rg + ac * 4) : be32(rg + ac * 4);
                    /* ranges decodes with THIS node's cells (3/2 per
                     * the PCI binding) against the parent's address
                     * cells.  Entry: <phys.hi phys.mid phys.lo>
                     * <parent addr> <size>; phys.hi bits 25:24 name
                     * the space -- 0b10 is 32-bit memory, the window
                     * BARs live in.  IO (0b01) and 64-bit (0b11)
                     * entries are skipped, not misread. */
                    const uint8_t *rp = nranges[depth];
                    uint32_t rlen = nranges_len[depth];
                    uint32_t cac = acells[depth];      /* 3 */
                    uint32_t csc = scells[depth];      /* 2 */
                    uint32_t one = (cac + ac + csc) * 4;
                    if (rp && cac == 3) {
                        for (uint32_t off = 0; off + one <= rlen;
                             off += one) {
                            uint32_t hi = be32(rp + off);
                            if (((hi >> 24) & 0x03u) != 0x02u)
                                continue;
                            uint64_t mid = be32(rp + off + 4);
                            plat->pcie_mmio_pci =
                                (mid << 32) | be32(rp + off + 8);
                            plat->pcie_mmio_cpu =
                                (ac == 2) ? be64(rp + off + 12)
                                          : be32(rp + off + 12);
                            plat->pcie_mmio_size =
                                (csc == 2) ? be64(rp + off + 12 + ac * 4)
                                           : be32(rp + off + 12 + ac * 4);
                            break;
                        }
                    }
                    pcie_done: ;
                } else if (dv == DEV_FWCFG && plat->fwcfg_base == 0) {
                    /* R11/RES-34: AMEND-5's deferral ends here — the
                     * a64 fw-cfg is MMIO, its base is DTB truth like
                     * every other window on this board. */
                    plat->fwcfg_base = cur_reg_base;
                }
            }
            if (depth >= 0) {
                nreg[depth] = 0; nreg_len[depth] = 0;
                nranges[depth] = 0; nranges_len[depth] = 0;
                nirq_raw[depth] = 0; nirq_len[depth] = 0;
                ndev[depth] = DEV_NONE;
            }
            node_is_cpu = 0;

            if (depth == in_chosen) in_chosen = -1;
            if (depth == in_memory) in_memory = -1;
            if (depth == in_resv)   in_resv   = -1;
            if (depth == in_cpus)   in_cpus   = -1;
            if (depth == in_psci)   in_psci   = -1;
            depth--;
            continue;
        }

        if (tok != FDT_PROP)
            return FDT_ERR_TRUNCATED;    /* unknown token: stream is junk */

        if (o + 8 > totalsize)
            return FDT_ERR_BOUNDS;
        uint32_t len     = be32(dtb + o);
        uint32_t nameoff = be32(dtb + o + 4);
        o += 8;
        if (o + len > totalsize || off_strings + nameoff >= totalsize)
            return FDT_ERR_BOUNDS;
        const char *pname = (const char *)(dtb + off_strings + nameoff);
        const uint8_t *val = dtb + o;
        o = (o + len + 3) & ~3u;

        /* -- cell bookkeeping (applies to this node's CHILDREN) ----- */
        if (streq(pname, "#address-cells") && len == 4) {
            acells[depth] = be32(val);
            continue;
        }
        if (streq(pname, "#size-cells") && len == 4) {
            scells[depth] = be32(val);
            continue;
        }

        /* -- /chosen ------------------------------------------------ */
        if (in_chosen >= 0) {
            /* initrd bounds arrive as 4 OR 8 byte cells depending on
             * who wrote the tree; accept both. */
            if (streq(pname, "linux,initrd-start"))
                initrd_start = (len == 8) ? be64(val) : be32(val);
            else if (streq(pname, "linux,initrd-end"))
                initrd_end = (len == 8) ? be64(val) : be32(val);
            else if (streq(pname, "bootargs") && len > 0)
                plat->bootargs = (const char *)val;
            continue;
        }

        /* -- /psci: the conduit (aarch64) ----------------------------- */
        if (in_psci >= 0 && streq(pname, "method") && len >= 4) {
            const char *m = (const char *)val;
            if (streq(m, "hvc"))      plat->psci_method = FDT_PSCI_HVC;
            else if (streq(m, "smc")) plat->psci_method = FDT_PSCI_SMC;
            continue;
        }

        /* -- /memory: reg = RAM banks -------------------------------- */
        if (in_memory >= 0 && streq(pname, "reg")) {
            uint32_t ac = acells[depth - 1], sc = scells[depth - 1];
            uint32_t stride = (ac + sc) * 4;
            if (stride == 0)
                continue;
            for (uint32_t b = 0; b + stride <= len; b += stride) {
                uint64_t base = (ac == 2) ? be64(val + b)
                                          : be32(val + b);
                uint64_t size = (sc == 2) ? be64(val + b + ac * 4)
                                          : be32(val + b + ac * 4);
                mmap_add(bi, base, size, BOOT_MEM_USABLE);
            }
            continue;
        }

        /* -- /reserved-memory children: reg = carve-outs ------------- */
        if (in_resv >= 0 && depth > in_resv && streq(pname, "reg")) {
            uint32_t ac = acells[depth - 1], sc = scells[depth - 1];
            uint32_t stride = (ac + sc) * 4;
            if (stride == 0)
                continue;
            for (uint32_t b = 0; b + stride <= len; b += stride) {
                uint64_t base = (ac == 2) ? be64(val + b)
                                          : be32(val + b);
                uint64_t size = (sc == 2) ? be64(val + b + ac * 4)
                                          : be32(val + b + ac * 4);
                mmap_add(bi, base, size, BOOT_MEM_RESERVED);
            }
            continue;
        }

        /* -- /cpus/cpu@N --------------------------------------------- */
        if (in_cpus >= 0 && depth == in_cpus &&
            streq(pname, "timebase-frequency") && len == 4) {
            /* What rdtime counts in.  On riscv virt: 10000000 (10
             * MHz).  A property of /cpus itself, not of any cpu@N
             * child -- and aarch64 trees simply do not carry it (the
             * frequency lives in CNTFRQ_EL0 there; plat field stays
             * 0 and the aarch64 kernel never reads it). */
            plat->timebase_freq = be32(val);
            continue;
        }

        if (node_is_cpu) {
            if (streq(pname, "device_type") && len >= 3 &&
                streq((const char *)val, "cpu") &&
                bi->cpu_count < BOOT_MAX_CPUS) {
                bi->cpu_count++;         /* id filled from reg below */
            }
            if (streq(pname, "reg") && bi->cpu_count > 0 &&
                bi->cpu_count <= BOOT_MAX_CPUS) {
                /* riscv trees: 4- or 8-byte hartid.  aarch64 trees:
                 * #address-cells of /cpus is 1 on virt (measured),
                 * reg = MPIDR affinity.  Same slot either way. */
                uint64_t hart = (len == 8) ? be64(val) : be32(val);
                boot_cpu_t *c = &bi->cpus[bi->cpu_count - 1];
                c->processor_id = (uint32_t)hart;
                c->lapic_id     = (uint32_t)hart;  /* the "which CPU" slot;
                                                    * hartid / MPIDR */
            }
            continue;
        }

        /* -- devices: compatible decides, reg is remembered ---------- */
        if (depth >= 0 && streq(pname, "reg")) {
            nreg[depth] = val;
            nreg_len[depth] = len;
            continue;
        }
        if (depth >= 0 && streq(pname, "ranges") && len >= 4) {
            nranges[depth] = val;        /* R7: decoded at END_NODE */
            nranges_len[depth] = len;
            continue;
        }
        if (depth >= 0 && streq(pname, "interrupts") && len >= 4) {
            nirq_raw[depth] = val;       /* RAW; normalised at done: */
            nirq_len[depth] = len;
            continue;
        }
        if (depth >= 0 && streq(pname, "compatible")) {
            const char *list = (const char *)val;
            if (compat_has(list, len, "ns16550a") ||
                compat_has(list, len, "arm,pl011"))
                ndev[depth] = DEV_UART;
            else if (compat_has(list, len, "riscv,plic0") ||
                     compat_has(list, len, "sifive,plic-1.0.0"))
                ndev[depth] = DEV_PLIC;
            else if (compat_has(list, len, "arm,cortex-a15-gic") ||
                     compat_has(list, len, "arm,gic-400") ||
                     (compat_has(list, len, "arm,gic-v3") &&
                      (plat->gic_is_v3 = 1)))
                /* R4: v3 hands the SAME two-range shape -- range 0 is
                 * the distributor, range 1 the REDISTRIBUTOR region;
                 * the slot names stay, gic.c's PIDR2 read decides
                 * what the second base means. */
                ndev[depth] = DEV_GIC;
            else if (compat_has(list, len, "virtio,mmio"))
                ndev[depth] = DEV_VIRTIO;
            else if (compat_has(list, len, "pci-host-ecam-generic"))
                ndev[depth] = DEV_PCIE;  /* R7: reg + ranges at END_NODE */
            else if (compat_has(list, len, "qemu,fw-cfg-mmio"))
                ndev[depth] = DEV_FWCFG; /* R11/RES-34: the a64 knob */
            continue;
        }
    }
    return FDT_ERR_TRUNCATED;            /* fell off the end: no FDT_END */

done:
    /* -- normalise the deferred interrupts: intc_kind is now final -- */
    if (uart_irq_raw)
        plat->uart_irq = irq_normalise(plat->intc_kind,
                                       uart_irq_raw, uart_irq_len);
    for (uint32_t i = 0; i < plat->virtio_count; i++)
        if (vio_irq_raw[i])
            plat->virtio_irq[i] = irq_normalise(plat->intc_kind,
                                                vio_irq_raw[i],
                                                vio_irq_len[i]);

    /* -- assemble the rest of boot_info_t ---------------------------- */

    if (initrd_end > initrd_start) {
        bi->initrd_phys = initrd_start;
        bi->initrd_size = initrd_end - initrd_start;
        /* The initrd is RAM the allocator must not hand out (same
         * reason the x86 loaders mark it BOOT_MEM_KERNEL). */
        mmap_add(bi, initrd_start, initrd_end - initrd_start,
                 BOOT_MEM_KERNEL);
    }

    /* The kernel image itself.  The bounds are absolute low symbols
     * the higher-half riscv code cannot address directly (medany's
     * auipc cannot span the HHDM gap), so each arch's boot.S exports
     * them as data -- contract 2 in fdt.h. */
    {
        extern const uint64_t kernel_layout[8];
        mmap_add(bi, kernel_layout[0],
                 kernel_layout[7] - kernel_layout[0],
                 BOOT_MEM_KERNEL);
    }

    /* The DTB too -- it is still read after the allocator exists. */
    mmap_add(bi, dtb_phys, totalsize, BOOT_MEM_BOOTLOADER);

    /* hhdm_offset: the D3 constant -- and on aarch64 the SAME number
     * by TTBR1 arithmetic (ARM64_PLAN D3), which is why this is not
     * an arch hook: one direct-map constant, two proofs.  The riscv
     * field is the contract V3 made true; A3 does the same. */
    bi->hhdm_offset    = 0xFFFFFFC000000000ULL;
    bi->boot_from_uefi = 0;                      /* SBI/PSCI path; the field
                                                  * answers "UEFI?" and the
                                                  * answer is no */
    bi->bsp_lapic_id   = (uint32_t)boot_hartid;
    if (bi->cpu_count == 0) {                    /* tree had no /cpus?  We
                                                  * are provably one CPU */
        bi->cpu_count = 1;
        bi->cpus[0].processor_id = (uint32_t)boot_hartid;
        bi->cpus[0].lapic_id     = (uint32_t)boot_hartid;
    }

    /* Magic LAST: a struct with a valid magic is complete by
     * definition, so the write order must make that true. */
    bi->magic = BOOT_MAGIC;
    return 0;
}
