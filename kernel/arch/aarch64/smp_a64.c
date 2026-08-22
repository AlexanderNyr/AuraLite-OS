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
#include "kernel/lib/kprintf.h"

extern const uint64_t kernel_layout[];       /* boot.S; [8] = secondary PA */

#define SMP_A64_MAX   16   /* x16 like rv64; v2 reality caps runs at 8 */
#define SEC_STACK_SZ  8192
#define SGI_ID        0u

/* Banked GIC registers (offsets from the DTB-discovered bases). */
#define GICD_ISENABLER0 0x100u
#define GICD_SGIR       0xF00u
#define GICC_CTLR       0x000u
#define GICC_PMR        0x004u
#define GICC_IAR        0x00Cu
#define GICC_EOIR       0x010u

static uint8_t sec_stacks[SMP_A64_MAX][SEC_STACK_SZ]
    __attribute__((aligned(16)));

static volatile uint64_t cores_online;
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
    uint64_t n = atomic_add(&cores_online, 1);
    kprintf("[smp] core %llu online (stack top %p, #%llu)\n",
            (unsigned long long)id,
            (void *)((uintptr_t)sec_stacks[id % SMP_A64_MAX]
                     + SEC_STACK_SZ),
            (unsigned long long)n);

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
            return;                 /* boot.S parks us in wfi */
        }
        if (intid != 1023u)          /* not ours: complete and move on */
            gicc[GICC_EOIR / 4] = iar;
        __asm__ volatile("wfe");
    }
}

static void spin_delay(uint64_t loops)
{
    for (volatile uint64_t i = 0; i < loops; i++)
        ;
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
    uint32_t sgi_targets = 0;

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
        started++;
    }

    for (int t = 0; t < 1000 && cores_online < started; t++)
        spin_delay(100000);
    kprintf("[smp] online: %llu/%u started core(s)\n",
            (unsigned long long)cores_online, started);

    if (cores_online > 0) {
        /* One SGI to the whole target list; sev first so pollers in
         * wfe wake even if their interface latched late. */
        __asm__ volatile("dsb ish" ::: "memory");
        gicd[GICD_SGIR / 4] = ((uint32_t)sgi_targets << 16) | SGI_ID;
        __asm__ volatile("sev" ::: "memory");
        for (int t = 0; t < 1000 && ipi_acks < cores_online; t++)
            spin_delay(100000);
        kprintf("[smp] IPI round-trip: %llu/%llu ack(s)\n",
                (unsigned long long)ipi_acks,
                (unsigned long long)cores_online);
    }
}
