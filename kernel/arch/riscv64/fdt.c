/* kernel/arch/riscv64/fdt.c -- minimal FDT parser (RISCV_PLAN V1).
 *
 * THE BYTE-ORDER FILE.  Everything in a flattened device tree --
 * header fields, token stream, property values -- is stored
 * BIG-endian, on a little-endian CPU, in a little-endian kernel.
 * This is the one place on the port where byte order bites (plan
 * risk list; V0 already proved the magic reads 0xD00DFEED only
 * through be32).  Every multi-byte read in this file goes through
 * be32()/be64() below; a bare load of DTB memory anywhere in this
 * file is a bug by definition.
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
 * [dtb, dtb+totalsize).  QEMU's tree is trusted in practice but the
 * parser doesn't know that -- V7 wants to reuse it on trees from
 * boards we have never seen.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/fdt.h"
#include "kernel/arch/riscv64/paging_rv.h"

/* ---- big-endian reads (the whole point of this file) ------------------- */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
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
 * State machine over nesting depth.  The tree shape on riscv (virt and
 * real boards alike):
 *
 *   /                     -- #address-cells / #size-cells for its children
 *     chosen              -- bootargs, linux,initrd-{start,end}
 *     memory@X            -- device_type "memory", reg = RAM banks
 *     reserved-memory     -- children's reg = firmware carve-outs
 *     cpus                -- children cpu@N, device_type "cpu"
 *     soc                 -- #address-cells/#size-cells for ITS children,
 *       uart@X            --   compatible "ns16550a"
 *       plic@X            --   compatible "riscv,plic0" / "sifive,plic-1.0.0"
 *       virtio_mmio@X     --   compatible "virtio,mmio"
 *
 * reg parsing needs the PARENT's cell counts, so the walk keeps a
 * small per-depth stack of them (default 2/1 per spec s.2.3.5 when a
 * parent doesn't say).
 */

#define MAX_DEPTH 16

int fdt_parse(uint64_t dtb_phys, uint64_t boot_hartid,
              boot_info_t *bi, fdt_platform_t *plat)
{
    /* V3: boot.S turned Sv39 on before any C ran, so physical
     * pointers no longer dereference bare -- the DTB is read through
     * the HHDM.  (The early identity gigapage would still carry a
     * bare read TODAY, but paging_rv_init drops it, and bootargs --
     * which points into this buffer -- outlives that drop.) */
    const uint8_t *dtb = (const uint8_t *)p2v_rv(dtb_phys);

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
    int node_is_cpu = 0;

    /* reg of the CURRENT node, decoded lazily once compatible is seen
     * (PROP order in a node is not guaranteed; remember both). */
    uint64_t cur_reg_base = 0;
    const uint8_t *cur_reg = 0;
    uint32_t cur_irq = 0;                /* first cell of `interrupts` */
    enum { DEV_NONE, DEV_UART, DEV_PLIC, DEV_VIRTIO } cur_dev = DEV_NONE;

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
            }
            node_is_cpu = (in_cpus >= 0 && node_is(name, "cpu"));
            cur_reg = 0; cur_dev = DEV_NONE;
            continue;
        }

        if (tok == FDT_END_NODE) {
            /* Leaving a device node: pair reg with what compatible said. */
            if (cur_dev != DEV_NONE && cur_reg) {
                /* reg decodes with the PARENT's cells. */
                uint32_t ac = acells[depth > 0 ? depth - 1 : 0];
                cur_reg_base = (ac == 2) ? be64(cur_reg) : be32(cur_reg);
                if (cur_dev == DEV_UART && plat->uart_base == 0) {
                    plat->uart_base = cur_reg_base;
                    plat->uart_irq  = cur_irq;   /* PLIC line (V2 wires it) */
                } else if (cur_dev == DEV_PLIC && plat->plic_base == 0)
                    plat->plic_base = cur_reg_base;
                else if (cur_dev == DEV_VIRTIO &&
                         plat->virtio_count < FDT_MAX_VIRTIO)
                    plat->virtio_base[plat->virtio_count++] = cur_reg_base;
            }
            cur_reg = 0; cur_irq = 0; cur_dev = DEV_NONE; node_is_cpu = 0;

            if (depth == in_chosen) in_chosen = -1;
            if (depth == in_memory) in_memory = -1;
            if (depth == in_resv)   in_resv   = -1;
            if (depth == in_cpus)   in_cpus   = -1;
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
            /* What rdtime counts in.  On virt: 10000000 (10 MHz).
             * A property of /cpus itself, not of any cpu@N child. */
            plat->timebase_freq = be32(val);
            continue;
        }

        if (node_is_cpu) {
            if (streq(pname, "device_type") && len >= 3 &&
                streq((const char *)val, "cpu") &&
                bi->cpu_count < BOOT_MAX_CPUS) {
                bi->cpu_count++;         /* hartid filled from reg below */
            }
            if (streq(pname, "reg") && bi->cpu_count > 0 &&
                bi->cpu_count <= BOOT_MAX_CPUS) {
                uint64_t hart = (len == 8) ? be64(val) : be32(val);
                boot_cpu_t *c = &bi->cpus[bi->cpu_count - 1];
                c->processor_id = (uint32_t)hart;
                c->lapic_id     = (uint32_t)hart;  /* the "which CPU" slot;
                                                    * this arch calls it a
                                                    * hartid */
            }
            continue;
        }

        /* -- devices: compatible decides, reg is remembered ---------- */
        if (streq(pname, "reg")) {
            cur_reg = val;
            continue;
        }
        if (streq(pname, "interrupts") && len >= 4) {
            cur_irq = be32(val);         /* first cell = the PLIC line */
            continue;
        }
        if (streq(pname, "compatible")) {
            const char *list = (const char *)val;
            if (compat_has(list, len, "ns16550a"))
                cur_dev = DEV_UART;
            else if (compat_has(list, len, "riscv,plic0") ||
                     compat_has(list, len, "sifive,plic-1.0.0"))
                cur_dev = DEV_PLIC;
            else if (compat_has(list, len, "virtio,mmio"))
                cur_dev = DEV_VIRTIO;
            continue;
        }
    }
    return FDT_ERR_TRUNCATED;            /* fell off the end: no FDT_END */

done:
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
     * the higher-half code cannot address directly (medany's auipc
     * cannot span the HHDM gap), so boot.S exports them as data. */
    {
        extern const uint64_t kernel_layout[8];
        mmap_add(bi, kernel_layout[0],
                 kernel_layout[7] - kernel_layout[0],
                 BOOT_MEM_KERNEL);
    }

    /* The DTB too -- V2 still reads it after the allocator exists. */
    mmap_add(bi, dtb_phys, totalsize, BOOT_MEM_BOOTLOADER);

    bi->hhdm_offset    = 0xFFFFFFC000000000ULL;  /* D3: Sv39 direct map;
                                                  * satp=0 until V3, the
                                                  * field is the CONTRACT,
                                                  * V3 makes it true */
    bi->boot_from_uefi = 0;                      /* SBI path; no third value
                                                  * invented -- the field
                                                  * answers "UEFI?" and the
                                                  * answer is no */
    bi->bsp_lapic_id   = (uint32_t)boot_hartid;
    if (bi->cpu_count == 0) {                    /* tree had no /cpus?  We
                                                  * are provably one hart */
        bi->cpu_count = 1;
        bi->cpus[0].processor_id = (uint32_t)boot_hartid;
        bi->cpus[0].lapic_id     = (uint32_t)boot_hartid;
    }

    /* Magic LAST: a struct with a valid magic is complete by
     * definition, so the write order must make that true. */
    bi->magic = BOOT_MAGIC;
    return 0;
}
