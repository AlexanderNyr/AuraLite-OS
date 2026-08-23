/* smp_a64.c — PARITY_PLAN.md P6: SMP bring-up on aarch64 via PSCI
 * CPU_ON (smp_rv.c's mirror; the function psci.c's own header
 * comment promised back in A0: "D5's named exit ramp").
 *
 * D5 scope, verbatim from P5: secondaries come up, report in with a
 * counted receipt, answer one IPI round-trip, and park in wfi.  No
 * scheduler claims.
 *
 * The IPI is a GICv2 SGI, and the secondaries POLL for it —
 * deliberately off the trap path (vectors, VBAR and the IRQ stack
 * stay boot-CPU property until a scheduler phase claims otherwise).
 * Each secondary brings up its own BANKED CPU interface (GICC_CTLR/
 * PMR and the SGI enable bit in the banked ISENABLER0) and claims
 * from GICC_IAR with PSTATE.I masked: the interface signals, the
 * core never takes the exception, IAR/EOIR work regardless.
 *
 * THE x16 BOUNDARY, measured not wished: GICv2's CPU interface is
 * architecturally 8 cores (SGIR's CPUTargetList is 8 bits; the
 * distributor banks 8 interfaces).  QEMU virt refuses -smp > 8 with
 * gic-version=2, and this driver IS a v2 driver (A2).  So the code
 * carries SMP_A64_MAX 16 like the rv64 side, rv64 PROVES -smp 16,
 * and this port proves -smp 8 — the v2 ceiling.  GICv3 (affinity-
 * routed SGIs via ICC_SGI1R_EL1) is the named residue that lifts it.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/smp_a64.h"
#include "kernel/arch/aarch64/psci.h"
#include "kernel/arch/aarch64/gic.h"      /* R4: the v3 lane */
#include "kernel/arch/aarch64/paging_a64.h"  /* R5 */
#include "kernel/arch/aarch64/trap_a64.h"
#include "kernel/arch/aarch64/user_a64.h"
#include "kernel/lib/kprintf.h"

extern const uint64_t kernel_layout[];       /* boot.S; [8] = secondary PA */

#define SMP_A64_MAX   16   /* x16 like rv64; v2 reality caps runs at 8 */
#define SEC_STACK_SZ  8192
#define SGI_ID        0u

/* R4: v3 redistributor offsets (own-frame SGI polling; see gic.c). */
#define GICR_SGI_OFF    0x10000u
#define GICR_IGROUPR0   0x0080u
#define GICR_ISPENDR0   0x0200u
#define GICR_ICPENDR0   0x0280u
#define ICC_SGI1R_EL1   "S3_0_C12_C11_5"

/* Banked GIC registers (offsets from the DTB-discovered bases). */
#define GICD_ISENABLER0 0x100u
#define GICD_SGIR       0xF00u
#define GICC_CTLR       0x000u
#define GICC_PMR        0x004u
#define GICC_IAR        0x00Cu
#define GICC_EOIR       0x010u

static uint8_t sec_stacks[SMP_A64_MAX][SEC_STACK_SZ]
    __attribute__((aligned(16)));

static uint64_t cnt_now(void);          /* fwd: defined with the waits */
static uint64_t cnt_freq(void);

static volatile uint64_t cores_online;
static volatile uint64_t job_post;      /* R5: 1 = run init */
static volatile uint64_t job_core;
static volatile uint64_t job_done;
static volatile int      job_code;
static volatile uint64_t ipi_acks;
static volatile uint32_t *gicd;              /* HHDM VAs, set by bringup */
static volatile uint32_t *gicc;

static uint64_t atomic_add(volatile uint64_t *p, uint64_t v)
{
    return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}

static uint64_t my_aff0(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0xFFu;
}

/* The secondary's C half (boot.S calls this on its own stack). */
void secondary_main_a64(void)
{
    uint64_t id = my_aff0();

    /* R7 rider (the R5 CI red, dissected): on v3 the SGI's group
     * claim must happen BEFORE this core counts itself online.  The
     * boot core fires ONE SGI the moment the counter says everyone
     * is here -- and a group-0 SGI is DISCARDED at the redistributor,
     * not left pending (the first 0/15 run's lesson).  A core that
     * incremented first and claimed second can lose its only shot
     * forever; core 9 did exactly that on the R5 run (14/15, its
     * "online" line printed after the boot core's count).  v2 never
     * flaked here because a v2 SGI latches pending even while the
     * target's interface is still down. */
    volatile uint8_t *rd = 0;
    if (gic_is_v3()) {
        rd = gic_v3_own_rdist();
        gic_v3_wake_rdist(rd);
        *(volatile uint32_t *)(rd + GICR_SGI_OFF + GICR_IGROUPR0) |=
            1u << SGI_ID;
        __asm__ volatile("dsb sy" ::: "memory");
    }

    uint64_t n = atomic_add(&cores_online, 1);
    kprintf("[smp] core %llu online (stack top %p, #%llu)\n",
            (unsigned long long)id,
            (void *)((uintptr_t)sec_stacks[id % SMP_A64_MAX]
                     + SEC_STACK_SZ),
            (unsigned long long)n);

    if (rd) {
        /* R4: poll the SGI pending bit in OUR redistributor -- off
         * the trap path AND off the CPU interface (the receipt needs
         * delivery to the redistributor, nothing more; D5
         * unchanged).  The group claim already happened above,
         * before the counter. */
        volatile uint32_t *pend =
            (volatile uint32_t *)(rd + GICR_SGI_OFF + GICR_ISPENDR0);
        volatile uint32_t *clr =
            (volatile uint32_t *)(rd + GICR_SGI_OFF + GICR_ICPENDR0);
        for (;;) {
            if (*pend & (1u << SGI_ID)) {
                *clr = 1u << SGI_ID;
                kprintf("[smp] core %llu: IPI received, parking\n",
                        (unsigned long long)id);
                atomic_add(&ipi_acks, 1);
                goto job_loop;      /* R5 */
            }
            /* R5, second draft.  wfe alone lost wakeups (13/15: sev
             * landed before two redistributors latched the SGI);
             * a yield spin made it WORSE (10/15: fifteen spinning
             * vCPUs starve the stragglers on a loaded host).  The
             * shape that holds: sleep in wfe here, and the BOOT
             * core re-sevs every wait iteration -- the sleeper can
             * miss one event but never the stream. */
            __asm__ volatile("wfe");
        }
    }

    /* This core's banked interface: unmask everything, enable SGI 0,
     * enable the interface — then poll IAR. */
    gicd[GICD_ISENABLER0 / 4] = 1u << SGI_ID;
    gicc[GICC_PMR / 4]  = 0xFFu;
    gicc[GICC_CTLR / 4] = 1u;
    __asm__ volatile("dsb ish" ::: "memory");

    for (;;) {
        uint32_t iar = gicc[GICC_IAR / 4];
        uint32_t intid = iar & 0x3FFu;
        if (intid == SGI_ID) {
            gicc[GICC_EOIR / 4] = iar;
            /* R3 CI fix: the LINE goes out BEFORE the counter ticks.
             * kprintf serialises under its lock, so once the boot
             * CPU sees acks==N every per-CPU ack line has already
             * drained -- run 32579828982 lost core 5's line to the
             * PSCI power-off racing a slow runner's ring. */
            kprintf("[smp] core %llu: IPI received, parking\n",
                    (unsigned long long)id);
            atomic_add(&ipi_acks, 1);
            goto job_loop;          /* R5 */
        }
        if (intid != 1023u)          /* not ours: complete and move on */
            gicc[GICC_EOIR / 4] = iar;
        __asm__ volatile("wfe");
    }

job_loop:
    /* R5: one strictly-serialized user job (smp_rv.c's mirror --
     * the boot core waits, so user_a64.c's one-image statics stay
     * single-entrant).  sev from the boot core wakes the wfe. */
    for (;;) {
        if (job_post) {
            uint64_t claim = __atomic_exchange_n(&job_post, 0,
                                                 __ATOMIC_SEQ_CST);
            if (claim) {
                job_core = id;
                __asm__ volatile("msr ttbr1_el1, %0\n\t"
                                 "msr ttbr0_el1, %1\n\t"
                                 "tlbi vmalle1\n\t"
                                 "dsb ish\n\t"
                                 "isb"
                                 :: "r"(paging_a64_final_ttbr1),
                                    "r"(paging_a64_final_ttbr0));
                trap_init_a64_secondary();
                job_code = user_a64_run_elf("bina64/init");
                __atomic_store_n(&job_done, 1, __ATOMIC_SEQ_CST);
                return;             /* park */
            }
        }
        __asm__ volatile("wfe");
    }
}

/* R5: run bina64/init at EL0 on ONE parked secondary. */
int smp_a64_run_init_on_secondary(int *out_core)
{
    if (cores_online == 0)
        return -1000;
    job_post = 1;
    __asm__ volatile("dsb ish; sev" ::: "memory");
    uint64_t deadline = cnt_now() + 60ull * cnt_freq();
    while (!job_done && cnt_now() < deadline)
        __asm__ volatile("yield");
    if (!job_done) {
        job_post = 0;
        return -1001;
    }
    if (out_core)
        *out_core = (int)job_core;
    return job_code;
}

/* R4: waits are bounded by GUEST TIME, not iterations -- the R1
 * UHCI lesson, re-learned here when a loaded 16-vCPU TCG run
 * expired the old 100M-iteration budget at 10/15 acks.  CNTVCT
 * ticks at CNTFRQ regardless of how slowly this vCPU is scheduled. */
static uint64_t cnt_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static uint64_t cnt_freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 62500000ull;
}

static void wait_counter(volatile uint64_t *ctr, uint64_t want,
                         uint64_t seconds)
{
    uint64_t deadline = cnt_now() + seconds * cnt_freq();
    while (*ctr < want && cnt_now() < deadline)
        __asm__ volatile("yield");
}

void smp_a64_bringup(const boot_info_t *bi,
                     uint64_t gicd_va, uint64_t gicc_va)
{
    gicd = (volatile uint32_t *)gicd_va;
    gicc = (volatile uint32_t *)gicc_va;

    uint32_t ncpus = bi->cpu_count ? bi->cpu_count : 1;
    if (ncpus > SMP_A64_MAX)
        ncpus = SMP_A64_MAX;
    uint64_t boot_id = my_aff0();
    kprintf("[smp] %u core(s) in the DTB, boot core %llu\n",
            ncpus, (unsigned long long)boot_id);
    if (ncpus < 2) {
        kprintf("[smp] nothing to start (single-core run)\n");
        return;
    }

    uint64_t entry_pa = kernel_layout[8];
    uint32_t started = 0;
    uint32_t sgi_targets = 0;                    /* v2: 8 CPUTargetList bits */
    static uint16_t sgi_targets_v3[(SMP_A64_MAX + 15) / 16];  /* v3: aff0 per aff1 */
    for (unsigned z = 0; z < sizeof(sgi_targets_v3)/sizeof(sgi_targets_v3[0]); z++)
        sgi_targets_v3[z] = 0;

    for (uint32_t i = 0; i < ncpus; i++) {
        uint64_t mpidr = bi->cpus[i].lapic_id;   /* MPIDR slot */
        if ((mpidr & 0xFFu) == boot_id)
            continue;
        uint64_t stack_top =
            (uintptr_t)sec_stacks[(mpidr & 0xFFu) % SMP_A64_MAX]
            + SEC_STACK_SZ;
        long rc = psci_cpu_on(mpidr, entry_pa, stack_top);
        if (rc != 0) {
            kprintf("[smp] CPU_ON(%llu) FAILED: PSCI error %ld\n",
                    (unsigned long long)mpidr, rc);
            continue;
        }
        sgi_targets |= 1u << (mpidr & 0x7u);     /* v2: 8 target bits */
        sgi_targets_v3[(mpidr >> 8) & 0xFFu] |=
            (uint16_t)(1u << (mpidr & 0xFu));    /* v3: aff0 bit in its cluster */
        started++;
    }

    wait_counter(&cores_online, started, 60);
    kprintf("[smp] online: %llu/%u started core(s)\n",
            (unsigned long long)cores_online, started);

    if (cores_online > 0) {
        __asm__ volatile("dsb ish" ::: "memory");
        if (gic_is_v3()) {
            /* R4: affinity-routed SGIs -- one ICC_SGI1R_EL1 write
             * per aff1 cluster (target list = 16 aff0 bits). */
            for (uint32_t cl = 0; cl < (SMP_A64_MAX + 15) / 16; cl++) {
                uint16_t tl = sgi_targets_v3[cl];
                if (!tl)
                    continue;
                uint64_t v = (uint64_t)tl |
                             ((uint64_t)cl << 16) |
                             ((uint64_t)SGI_ID << 24);
                __asm__ volatile("msr " ICC_SGI1R_EL1 ", %0" :: "r"(v));
            }
            __asm__ volatile("isb");
        } else {
            /* One SGI to the whole target list; sev first so pollers
             * in wfe wake even if their interface latched late. */
            gicd[GICD_SGIR / 4] = ((uint32_t)sgi_targets << 16) | SGI_ID;
        }
        __asm__ volatile("sev" ::: "memory");
        {
            /* Re-sev while waiting: pending SGIs latched after a
             * sleeper's poll get another event, every pass. */
            uint64_t dl = cnt_now() + 60ull * cnt_freq();
            while (ipi_acks < cores_online && cnt_now() < dl) {
                __asm__ volatile("sev" ::: "memory");
                __asm__ volatile("yield");
            }
        }
        kprintf("[smp] IPI round-trip: %llu/%llu ack(s)\n",
                (unsigned long long)ipi_acks,
                (unsigned long long)cores_online);
    }
}
