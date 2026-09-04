/* kernel/arch/x86_64/ioapic.c -- I/O APIC driver (MATURITY_PLAN.md phase M2).
 *
 * Routes the 16 legacy ISA IRQs through the I/O APIC instead of the 8259 PIC,
 * so the BSP stops receiving them over the PIC/"virtual-wire" (LINT0 ExtINT)
 * path and the 8259 can be fully masked.  This is the prerequisite for SMP-
 * safe, level-aware interrupt delivery and for leaving PIC mode behind on
 * real hardware.
 *
 * This is the QEMU-primary increment (plan decision D5): the I/O APIC is
 * assumed at the PC-standard base 0xFEC00000, and the one legacy remap that
 * matters -- ISA IRQ0 (the PIT) is wired to GSI 2 -- is hard-coded.  Real
 * machines publish the IOAPIC base and any further Interrupt Source Overrides
 * in the ACPI MADT.
 *
 * RESIDUE R11 (RES-37): the MADT half of that follow-up is in.  Both
 * loaders already publish rsdp_phys; the MADT walk below reads
 * RSDP -> RSDT/XSDT -> MADT and returns the first type-1 (I/O APIC)
 * entry's address.  The kernel still USES the PC-standard base -- what
 * changed is that the boot now prints whether ACPI agrees with the
 * hardcode ("MADT agree" / "MADT says 0x..., hardcode stands" /
 * "no MADT") -- so the metal receipt package can carry the discovery
 * line, and a machine where the two differ is named on sight instead
 * of silently mis-programmed.
 *
 * RESIDUE2 T2: the last QEMU-hardcoded piece is gone.  The same walk
 * now collects the type-2 Interrupt Source Override entries and the
 * redirection table is programmed FROM THEM (bus source -> GSI, with
 * the polarity/trigger bits the table carries).  The PC-standard
 * defaults (PIT on GSI2, everything else identity, edge/high) remain
 * as the fallback for a machine with no MADT or no ISOs, and every
 * divergence between the two routings is printed by name at boot --
 * the RES-37 agree/disagree discipline extended from the base address
 * to the whole legacy routing.
 *
 * Safety: if no I/O APIC is present (the version register reads as
 * all-zero/all-one), this returns non-zero WITHOUT touching the PIC, so the
 * established PIC virtual-wire path keeps working unchanged. */

#include <stdint.h>
#include "kernel/arch/x86_64/ioapic.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/boot_info.h"
#include "kernel/lib/kprintf.h"

/* I/O APIC register interface: an 8-bit IOREGSEL index register at +0x00
 * selects which 32-bit register the IOWIN window at +0x10 reads/writes. */
#define IOAPIC_PHYS       0xFEC00000ULL
#define IOAPIC_REGSEL     0x00
#define IOAPIC_WIN        0x10

#define IOAPIC_ID         0x00   /* register index: ID            */
#define IOAPIC_VER        0x01   /* register index: version       */
#define IOAPIC_REDTBL     0x10   /* first redirection-table reg   */

/* Becomes 1 once the BSP has switched to I/O-APIC delivery; irq_dispatch()
 * checks it to skip the (now-meaningless) 8259 EOI. */
volatile int apic_irq_mode = 0;

static volatile uint32_t *ioapic_win;   /* HHDM-mapped register window */
static int                ioapic_count; /* number of redirection entries */

static inline uint32_t io_read(uint8_t reg) {
    ioapic_win[IOAPIC_REGSEL / 4] = reg;
    return ioapic_win[IOAPIC_WIN / 4];
}

static inline void io_write(uint8_t reg, uint32_t val) {
    ioapic_win[IOAPIC_REGSEL / 4] = reg;
    ioapic_win[IOAPIC_WIN / 4] = val;
}

/* A redirection-table entry is a 64-bit value split across two 32-bit
 * registers: low half at 0x10+2*gsi, high half at 0x11+2*gsi.  Write the high
 * half (destination) first so that, the instant the low half unmasks and arms
 * the vector, the destination field already holds the BSP APIC ID -- no
 * transient window can deliver to APIC ID 0 by accident. */
static void io_set_redir(int gsi, uint64_t entry) {
    io_write((uint8_t)(IOAPIC_REDTBL + 2 * gsi + 1), (uint32_t)(entry >> 32));
    io_write((uint8_t)(IOAPIC_REDTBL + 2 * gsi),     (uint32_t)(entry));
}

/* Build a redirection entry for a fixed-delivery ISA interrupt aimed
 * (physical destination mode) at the given APIC ID, with the polarity and
 * trigger mode the ACPI table asks for. */
static uint64_t io_make_entry(int vector, uint32_t dest_apic_id,
                              int level, int active_low) {
    return ((uint64_t)(vector & 0xFF))            /* [7:0]   vector             */
         | (0ULL << 8)                            /* [10:8]  fixed delivery     */
         | (0ULL << 11)                           /* [11]    physical dest mode */
         | ((active_low ? 1ULL : 0ULL) << 13)     /* [13]    polarity           */
         | ((level ? 1ULL : 0ULL) << 15)          /* [15]    trigger mode       */
         | (0ULL << 16)                           /* [16]    unmasked           */
         | (((uint64_t)(dest_apic_id & 0xFF)) << 56); /* [63:56] dest APIC ID */
}

/* ------------------------------------------------------------------ */
/* MADT walk (R11/RES-37 base + RESIDUE2 T2 Interrupt Source Overrides). */
/* Read-only, boot-time, single-threaded.                              */
/* ------------------------------------------------------------------ */

/* Map [phys, phys+len) at hhdm+phys and return the pointer.  ACPI
 * tables live in reserved memory the HHDM prebuild may not cover, so
 * map explicitly -- same approach the register window below uses.
 * paging_map is idempotent for an already-mapped page. */
static const uint8_t *acpi_map(uint64_t hhdm, uint64_t phys, uint64_t len) {
    uint64_t first = phys & ~0xFFFULL;
    uint64_t last  = (phys + len - 1) & ~0xFFFULL;
    for (uint64_t p = first; p <= last; p += 0x1000) {
        paging_map(hhdm + p, p, PAGE_FLAGS_MMIO);
    }
    return (const uint8_t *)(uintptr_t)(hhdm + phys);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* RESIDUE2 T2: Interrupt Source Overrides (MADT type-2 entries) drive the
 * redirection table.  Entry layout at +off: [type:1][len:1][bus:1]
 * [source:1][gsi:4][flags:2]; flags [1:0] polarity (1 = active high,
 * 3 = active low, 0 = bus default -> high for ISA), [3:2] trigger
 * (1 = edge, 3 = level, 0 = bus default -> edge for ISA), and GSI
 * 0xFFFFFFFF marks a disabled source. */
#define MADT_MAX_ISO 16
struct madt_iso {
    uint8_t  bus;
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
};

static uint64_t        madt_ioapic_addr;   /* first type-1 entry, 0 if none */
static int             madt_iso_count;     /* entries captured below */
static struct madt_iso madt_isos[MADT_MAX_ISO];

static const struct madt_iso *iso_for(uint8_t bus, uint8_t source) {
    for (int i = 0; i < madt_iso_count; i++) {
        if (madt_isos[i].bus == bus && madt_isos[i].source == source) {
            return &madt_isos[i];
        }
    }
    return NULL;
}

/* Walk RSDP -> RSDT/XSDT -> MADT once, filling in madt_ioapic_addr and
 * the ISO table.  Everything absent/implausible simply stays zero/empty;
 * callers fall back to the PC-standard routing and say so at boot.
 * Length caps keep a corrupt table from walking us off a cliff. */
static void madt_walk(uint64_t hhdm) {
    uint64_t rsdp_phys = boot_get_rsdp();
    if (!rsdp_phys) {
        return;
    }
    const uint8_t *rsdp = acpi_map(hhdm, rsdp_phys, 36);

    /* Revision >= 2: prefer the XSDT (64-bit pointers at +24), fall
     * back to the RSDT (32-bit pointers at +16) -- the same order
     * stage2's own MADT walk uses. */
    uint64_t sdt_phys = 0;
    int      wide     = 0;
    if (rsdp[15] >= 2) {
        sdt_phys = rd64(rsdp + 24);
        wide     = 1;
    }
    if (!sdt_phys) {
        sdt_phys = rd32(rsdp + 16);
        wide     = 0;
    }
    if (!sdt_phys) {
        return;
    }

    const uint8_t *sdt = acpi_map(hhdm, sdt_phys, 36);
    uint32_t sdt_len = rd32(sdt + 4);
    if (sdt_len < 36 || sdt_len > 0x10000) {
        return;
    }
    sdt = acpi_map(hhdm, sdt_phys, sdt_len);

    uint32_t esize   = wide ? 8 : 4;
    uint32_t entries = (sdt_len - 36) / esize;
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t tbl_phys = wide ? rd64(sdt + 36 + i * 8)
                                 : rd32(sdt + 36 + i * 4);
        if (!tbl_phys) {
            continue;
        }
        const uint8_t *tbl = acpi_map(hhdm, tbl_phys, 44);
        if (tbl[0] != 'A' || tbl[1] != 'P' || tbl[2] != 'I' ||
            tbl[3] != 'C') {
            continue;
        }
        uint32_t tlen = rd32(tbl + 4);
        if (tlen < 44 || tlen > 0x10000) {
            return;
        }
        tbl = acpi_map(hhdm, tbl_phys, tlen);

        /* Interrupt-controller structures start at +44:
         * [type:1][len:1][payload]; type 1 = I/O APIC, address at +4. */
        uint32_t off = 44;
        while (off + 2 <= tlen) {
            uint8_t etype = tbl[off];
            uint8_t elen  = tbl[off + 1];
            if (elen < 2 || off + elen > tlen) {
                break;                      /* corrupt list: stop, named 0 */
            }
            if (etype == 1 && elen >= 12 && madt_ioapic_addr == 0) {
                madt_ioapic_addr = rd32(tbl + off + 4);
            } else if (etype == 2 && elen >= 12 &&
                       madt_iso_count < MADT_MAX_ISO) {
                madt_isos[madt_iso_count].bus    = tbl[off + 2];
                madt_isos[madt_iso_count].source = tbl[off + 3];
                madt_isos[madt_iso_count].gsi    = rd32(tbl + off + 4);
                madt_isos[madt_iso_count].flags  =
                    (uint16_t)(tbl[off + 8] | ((uint16_t)tbl[off + 9] << 8));
                madt_iso_count++;
            }
            off += elen;
        }
    }
}

int ioapic_route_gsi(int gsi, int vector, uint32_t dest_apic_id) {
    if (!apic_irq_mode || !ioapic_win) {
        return -1;
    }
    if (gsi < 0 || gsi >= ioapic_count) {
        return -1;
    }
    /* Edge/high/fixed, unmasked -- the ISA shape; the caller restores the
     * pin when it is done (see the RES-16 wake selftest). */
    io_set_redir(gsi, io_make_entry(vector, dest_apic_id, 0, 0));
    return 0;
}

int ioapic_init(void) {
    uint64_t hhdm = boot_get_hhdm_offset();
    if (!hhdm) {
        kprintf("[ioapic] no HHDM offset; staying on PIC\n");
        return -1;
    }

    /* R11 (RES-37) + RESIDUE2 T2: one MADT walk now answers BOTH halves
     * of the discovery contract -- where the I/O APIC is (printed against
     * the PC-standard base below, QEMU the NULL test) and how the legacy
     * IRQs are actually wired (the ISO list programmed further down).
     * QEMU is the NULL test (they agree); a metal machine where they
     * differ is named at boot -- that line is a receipt slot in the R11
     * package. */
    madt_walk(hhdm);
    uint64_t madt_base = madt_ioapic_addr;
    if (madt_base == 0) {
        kprintf("[ioapic] base 0x%llx (no MADT I/O APIC entry found)\n",
                (unsigned long long)IOAPIC_PHYS);
    } else if (madt_base == IOAPIC_PHYS) {
        kprintf("[ioapic] base 0x%llx (MADT agree)\n",
                (unsigned long long)IOAPIC_PHYS);
    } else {
        kprintf("[ioapic] base 0x%llx (MADT says 0x%llx, hardcode stands"
                " -- named, not silent)\n",
                (unsigned long long)IOAPIC_PHYS,
                (unsigned long long)madt_base);
    }

    /* Map the I/O APIC MMIO window into the HHDM region. */
    uint64_t virt = hhdm + IOAPIC_PHYS;
    paging_map(virt, IOAPIC_PHYS, PAGE_FLAGS_MMIO);
    ioapic_win = (volatile uint32_t *)(uintptr_t)virt;

    /* Probe: the version register's low byte is the IOAPIC version (0x10/0x11
     * on the 82093AA and QEMU, 0x20/0x21 on newer parts) and bits [23:16]
     * hold max-redirection-entry, so entry count = that + 1.  An absent IOAPIC
     * reads back all-zero or all-one; either way this guard rejects it and
     * leaves the PIC path intact. */
    uint32_t ver   = io_read(IOAPIC_VER);
    uint32_t vmax  = (ver >> 16) & 0xFF;
    uint32_t vlow  = ver & 0xFF;
    ioapic_count   = (int)vmax + 1;
    if (vlow == 0x00 || vlow == 0xFF || ioapic_count <= 0 || ioapic_count > 64) {
        kprintf("[ioapic] no I/O APIC at 0x%llx (ver=0x%x); staying on PIC\n",
                (unsigned long long)IOAPIC_PHYS, ver);
        return -1;
    }

    /* Step 1: mask every redirection entry first so nothing can fire while we
     * reprogram the legacy routing. */
    for (int i = 0; i < ioapic_count; i++) {
        io_set_redir(i, 1ULL << 16);              /* masked, everything else 0 */
    }

    /* Step 2 (RESIDUE2 T2): program the legacy ISA IRQs FROM the MADT's
     * Interrupt Source Overrides; the PC-standard defaults below are the
     * fallback for a machine with no MADT (or no ISOs), not the primary
     * truth.  Every divergence between the two routings is printed by
     * name -- the RES-37 agree/disagree discipline, extended from the
     * base address to the routing itself.
     *
     * Vectors stay at 32 + irq -- exactly the PIC remap offsets -- so every
     * already-registered handler keeps firing on the same vector.
     * Destination is the BSP APIC ID in physical mode.
     *
     * IRQ2 must be SKIPPED: it is the 8259 cascade line (no real device sits
     * on it), and in the identity map IRQ2 would land on GSI2 -- the very pin
     * the PIT override occupies.  Programming it would overwrite the PIT's
     * redirection entry with IRQ2's vector, sending PIT ticks to vector 34 and
     * freezing the timer (and the scheduler) after zero ticks. */
    uint32_t bsp = lapic_read_id();
    int iso_applied = 0, iso_disagree = 0;
    for (int irq = 0; irq < 16; irq++) {
        if (irq == 2) continue;                   /* cascade; GSI2 is the PIT  */
        int gsi, level, low;
        const struct madt_iso *ov = iso_for(0 /* ISA */, (uint8_t)irq);
        if (ov) {
            if (ov->gsi == 0xFFFFFFFFu) {
                kprintf("[ioapic] ISA IRQ%u marked disabled by the MADT; "
                        "left masked\n", irq);
                continue;
            }
            gsi = (int)ov->gsi;
            int pol = (ov->flags >> 1) & 3;       /* 0 bus-default, 1 high, 3 low */
            int trg = (ov->flags >> 3) & 3;       /* 0 bus-default, 1 edge, 3 lvl */
            low   = (pol == 3);
            level = (trg == 3);
            iso_applied++;
            /* Name the divergence from the PC-standard default, if any. */
            int dgsi = (irq == 0) ? 2 : irq;
            if (gsi != dgsi || level || low) {
                kprintf("[ioapic] override: ISA IRQ%u -> GSI%u %s-%s "
                        "(PC default GSI%u edge-high)\n",
                        irq, gsi, level ? "level" : "edge",
                        low ? "low" : "high", dgsi);
                iso_disagree++;
            }
        } else {
            gsi   = (irq == 0) ? 2 : irq;         /* PIT: IRQ0 -> GSI2 (MADT absent) */
            level = 0;
            low   = 0;
        }
        if (gsi >= ioapic_count) {
            kprintf("[ioapic] ISA IRQ%u -> GSI%u beyond %d entries; "
                    "left masked\n", irq, gsi, ioapic_count);
            continue;
        }
        io_set_redir(gsi, io_make_entry(32 + irq, bsp, level, low));
    }

    /* Step 3: switch the BSP off the PIC.  Mask the 8259 completely and flip
     * the IMCR to APIC delivery so the (now silent) 8259 is fully decoupled,
     * then mask LINT0 -- it carried the 8259's ExtINT vectors and the IOAPIC
     * delivers directly now.  LINT1 (NMI) is untouched. */
    pic_disable_for_apic();
    lapic_mask_lint0();

    apic_irq_mode = 1;
    kprintf("[ioapic] I/O APIC @0x%llx ver 0x%x, %d redirection entries; "
            "BSP on APIC IRQs (PIT@GSI2, kbd@GSI1)\n",
            (unsigned long long)IOAPIC_PHYS, vlow, ioapic_count);
    /* RESIDUE2 T2 receipt line: what drove the routing. */
    if (madt_iso_count > 0) {
        kprintf("[ioapic] overrides: %d ISO(s) applied from the MADT "
                "(%d diverge%s from PC defaults) -- MADT drives the "
                "routing\n",
                iso_applied, iso_disagree,
                (iso_disagree == 1) ? "" : "s");
    } else {
        kprintf("[ioapic] overrides: none in the MADT; PC-standard "
                "defaults programmed\n");
    }
    return 0;
}
