/* kernel/arch/aarch64/gic.c -- GICv2 driver (ARM64_PLAN A2).
 *
 * The PLIC's counterpart, different in every register (plan Fact
 * 5.4): a banked distributor (GICD) that routes, and a per-CPU
 * interface (GICC) that delivers.  Same driver shape though --
 * init / enable-by-number / claim-complete dispatch -- so a reader
 * of plic.c knows where they are in this file.
 *
 * Register subset used (GICv2 spec, what qemu-virt implements):
 *
 *   GICD + 0x000  CTLR       group 0 enable (bit 0)
 *   GICD + 0x100  ISENABLERn set-enable, 32 INTIDs per word
 *   GICD + 0x400  IPRIORITYRn one byte per INTID (0 = highest)
 *   GICD + 0x800  ITARGETSRn  one byte per INTID, CPU mask (SPIs only)
 *   GICC + 0x00   CTLR       CPU interface enable (bit 0)
 *   GICC + 0x04   PMR        priority mask (0xFF = let everything in)
 *   GICC + 0x0C   IAR        claim: read INTID (1023 = spurious)
 *   GICC + 0x10   EOIR       complete: write the INTID back
 *
 * INTIDs arrive pre-normalised from the A1 walker; the off-by-32
 * lives in exactly one place and this file is not it.
 *
 * MMIO through volatile uint32_t: the GIC is a 32-bit peripheral.
 * The MMU is off in A2, so the bases are physical addresses used
 * directly (A3 moves this behind the HHDM with a Device-nGnRE
 * mapping, same as every other MMIO user -- until then, all memory
 * IS Device memory and the ordering question cannot arise).
 */

#include <stdint.h>

#include "kernel/arch/aarch64/gic.h"

#define GICD_CTLR         0x000u
#define GICD_ISENABLER    0x100u
#define GICD_IPRIORITYR   0x400u
#define GICD_ITARGETSR    0x800u

#define GICC_CTLR         0x00u
#define GICC_PMR          0x04u
#define GICC_IAR          0x0Cu
#define GICC_EOIR         0x10u

#define GIC_SPURIOUS      1023u
#define GIC_MAX_INTID     1020u   /* 1020..1023 are special per spec */

static volatile uint8_t *gicd;         /* distributor MMIO base */
static volatile uint8_t *gicc;         /* CPU interface MMIO base */

/* virt wires 32 PPI/SGI + up to ~288 SPIs; 1020 handler slots keeps
 * the array honest against the architectural ceiling instead of a
 * board guess (8 bytes each -- cheap insurance). */
static gic_handler_t handlers[GIC_MAX_INTID];
static volatile uint64_t completions;

static inline volatile uint32_t *d32(uint32_t off)
{
    return (volatile uint32_t *)(gicd + off);
}

static inline volatile uint32_t *c32(uint32_t off)
{
    return (volatile uint32_t *)(gicc + off);
}

void gic_init(uint64_t gicd_base, uint64_t gicc_base)
{
    gicd = (volatile uint8_t *)gicd_base;
    gicc = (volatile uint8_t *)gicc_base;

    /* Distributor first, then the CPU interface -- lines stay
     * disabled until gic_enable asks for them by name, so nothing
     * can fire half-configured (the trap_init ordering rule). */
    *d32(GICD_CTLR) = 1;               /* group 0 forwarding on */
    *c32(GICC_PMR)  = 0xFF;            /* mask nothing */
    *c32(GICC_CTLR) = 1;               /* deliver to this CPU */
}

void gic_enable(uint32_t intid, gic_handler_t fn)
{
    if (!gicd || intid >= GIC_MAX_INTID)
        return;
    handlers[intid] = fn;

    /* Priority: one byte per INTID.  0xA0 -- comfortably above the
     * PMR floor, below nothing we use (every line we enable gets the
     * same value; priority games are a later phase's problem if
     * ever). */
    volatile uint8_t *prio =
        (volatile uint8_t *)(gicd + GICD_IPRIORITYR + intid);
    *prio = 0xA0;

    /* SPIs additionally need a target CPU (PPIs are banked per-CPU
     * and their ITARGETSR bytes are read-only).  CPU 0's mask is
     * 0x01 -- boot CPU only, D5. */
    if (intid >= 32) {
        volatile uint8_t *tgt =
            (volatile uint8_t *)(gicd + GICD_ITARGETSR + intid);
        *tgt = 0x01;
    }

    *d32(GICD_ISENABLER + (intid / 32) * 4) = 1u << (intid % 32);
}

void gic_dispatch(void)
{
    /* Claim until the well is dry: multiple INTIDs can be pending
     * behind one IRQ exception -- the plic_dispatch loop, IAR/EOIR
     * flavoured. */
    for (;;) {
        uint32_t iar   = *c32(GICC_IAR);
        uint32_t intid = iar & 0x3FFu;

        if (intid >= GIC_SPURIOUS)
            return;                    /* 1023: nothing pending */
        if (intid < GIC_MAX_INTID && handlers[intid])
            handlers[intid](intid);
        /* Complete with the FULL IAR value (spec: the CPUID field
         * must round-trip for SGIs) -- even for a line without a
         * handler; a stuck claim gates everything behind it. */
        *c32(GICC_EOIR) = iar;
        completions++;
    }
}

uint64_t gic_completions(void)
{
    return completions;
}
