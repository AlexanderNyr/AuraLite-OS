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

/* ---- R4 (RESIDUE ledger RES-13): the GICv3 lane ------------------- */
/* Detected at init from GICD_PIDR2.ArchRev -- the DTB hands the same
 * two ranges either way (for v3 the second range is the REDISTRIBUTOR
 * region, not a CPU interface; QEMU virt: GICR frames of 128 KiB per
 * PE, GICR_TYPER carries the affinity and the Last bit).  v3 keeps
 * GICD_ISENABLER/IPRIORITYR at the v2 offsets for SPIs; what changes
 * is routing (IROUTER, affinity-based), SGI/PPI configuration (the
 * PE's OWN redistributor SGI frame) and claim/complete (ICC system
 * registers -- the MMIO CPU interface is gone). */
#define GICD_CTLR_ARE_NS  (1u << 4)
#define GICD_CTLR_G1NS    (1u << 1)
#define GICD_IROUTER      0x6100u
#define GICR_STRIDE       0x20000u
#define GICR_TYPER        0x0008u
#define GICR_WAKER        0x0014u
#define GICR_SGI_OFF      0x10000u   /* the SGI/PPI frame within a redist */

/* ICC system registers by S-encoding (works under -mgeneral-regs-only
 * without asking the assembler to know the names). */
#define ICC_SRE_EL1     "S3_0_C12_C12_5"
#define ICC_PMR_EL1     "S3_0_C4_C6_0"
#define ICC_IGRPEN1_EL1 "S3_0_C12_C12_7"
#define ICC_IAR1_EL1    "S3_0_C12_C12_0"
#define ICC_EOIR1_EL1   "S3_0_C12_C12_1"

static int gic_v3;
static volatile uint8_t *gicr;         /* v3: redistributor region base */

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

int gic_is_v3(void)
{
    return gic_v3;
}

/* This PE's redistributor frame, found by affinity match (GICR_TYPER
 * bits 63:32 carry Aff3.2.1.0 of the PE the frame belongs to). */
volatile uint8_t *gic_v3_own_rdist(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t aff = (uint32_t)(((mpidr >> 32) & 0xFFu) << 24 |
                              ((mpidr >> 16) & 0xFFu) << 16 |
                              ((mpidr >> 8)  & 0xFFu) << 8  |
                              (mpidr & 0xFFu));
    for (uint32_t i = 0; ; i++) {
        volatile uint8_t *rd = gicr + (uint64_t)i * GICR_STRIDE;
        uint64_t typer = *(volatile uint64_t *)(rd + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff)
            return rd;
        if (typer & (1u << 4))         /* Last */
            return 0;
    }
}

/* Wake a redistributor: clear ProcessorSleep, wait ChildrenAsleep. */
void gic_v3_wake_rdist(volatile uint8_t *rd)
{
    volatile uint32_t *waker = (volatile uint32_t *)(rd + GICR_WAKER);
    *waker &= ~(1u << 1);
    while (*waker & (1u << 2))
        ;
}

/* This PE's CPU interface via the system registers. */
void gic_v3_cpu_init(void)
{
    uint64_t sre;
    __asm__ volatile("mrs %0, " ICC_SRE_EL1 : "=r"(sre));
    __asm__ volatile("msr " ICC_SRE_EL1 ", %0" :: "r"(sre | 1));
    __asm__ volatile("isb");
    __asm__ volatile("msr " ICC_PMR_EL1 ", %0" :: "r"(0xFFul));
    __asm__ volatile("msr " ICC_IGRPEN1_EL1 ", %0" :: "r"(1ul));
    __asm__ volatile("isb");
}

void gic_init(uint64_t gicd_base, uint64_t gicc_base, int is_v3)
{
    gicd = (volatile uint8_t *)gicd_base;
    gicc = (volatile uint8_t *)gicc_base;

    /* R4: the DTB decides the lane.  The first draft probed
     * GICD_PIDR2 -- and hung every v2 boot, because QEMU's v2
     * distributor is a 4 KiB region and +0xFFE8 lands in UNASSIGNED
     * space (abort before VBAR exists).  The device tree names the
     * generation; reading id registers the platform may not map is
     * not detection, it is roulette. */
    gic_v3 = is_v3;

    if (gic_v3) {
        gicr = gicc;                   /* the DTB's second range IS GICR */
        *d32(GICD_CTLR) = GICD_CTLR_ARE_NS;
        *d32(GICD_CTLR) = GICD_CTLR_ARE_NS | GICD_CTLR_G1NS;
        volatile uint8_t *rd = gic_v3_own_rdist();
        gic_v3_wake_rdist(rd);
        gic_v3_cpu_init();
        return;
    }

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

    if (gic_v3) {
        if (intid < 32) {
            /* SGI/PPI: this PE's own redistributor SGI frame. */
            volatile uint8_t *sgi = gic_v3_own_rdist() + GICR_SGI_OFF;
            *(volatile uint8_t *)(sgi + GICD_IPRIORITYR + intid) = 0xA0;
            *(volatile uint32_t *)(sgi + GICD_ISENABLER) =
                1u << intid;
        } else {
            /* SPI: same GICD offsets as v2 for priority/enable;
             * routing is affinity-based -- 0 = Aff 0.0.0.0, the
             * boot PE, stated instead of trusting reset values. */
            volatile uint8_t *prio =
                (volatile uint8_t *)(gicd + GICD_IPRIORITYR + intid);
            *prio = 0xA0;
            *(volatile uint64_t *)(gicd + GICD_IROUTER +
                                   8ull * intid) = 0;
            *d32(GICD_ISENABLER + (intid / 32) * 4) =
                1u << (intid % 32);
        }
        return;
    }

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
        uint32_t iar, intid;
        if (gic_v3) {
            uint64_t v;
            __asm__ volatile("mrs %0, " ICC_IAR1_EL1 : "=r"(v));
            iar = (uint32_t)v;
        } else {
            iar = *c32(GICC_IAR);
        }
        intid = iar & 0x3FFu;

        if (intid >= GIC_SPURIOUS)
            return;                    /* 1023: nothing pending */
        if (intid < GIC_MAX_INTID && handlers[intid])
            handlers[intid](intid);
        /* Complete with the FULL IAR value (spec: the CPUID field
         * must round-trip for SGIs) -- even for a line without a
         * handler; a stuck claim gates everything behind it. */
        if (gic_v3)
            __asm__ volatile("msr " ICC_EOIR1_EL1 ", %0" :: "r"((uint64_t)iar));
        else
            *c32(GICC_EOIR) = iar;
        completions++;
    }
}

uint64_t gic_completions(void)
{
    return completions;
}
