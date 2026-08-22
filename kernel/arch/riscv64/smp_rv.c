/* smp_rv.c — PARITY_PLAN.md P5: SMP bring-up on rv64 via SBI HSM.
 *
 * Scope is D5's, exactly: secondary harts come up, report in with a
 * counted receipt, answer one IPI round-trip, and park in wfi.  No
 * scheduler claims — per-CPU runqueues on this port are NAMED
 * RESIDUE, and the honest statement of this file is "N harts online,
 * 1 scheduled".
 *
 * Mechanics: the boot hart hands each stopped hart (DTB list, its
 * own id skipped) a per-hart stack top as HSM's opaque argument;
 * _secondary_start (boot.S) turns satp on and jumps high;
 * secondary_main_rv() below reports in, then POLLS sip.SSIP for the
 * IPI — deliberately not the trap path.  The trap vector, its
 * stacks and sscratch discipline are single-hart property until a
 * scheduler phase claims otherwise; wfi wakes on a pending interrupt
 * regardless of sstatus.SIE, so polling costs nothing and touches
 * nothing it does not own.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/smp_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/lib/kprintf.h"
#include "boot/shared/boot_info.h"

extern const uint64_t kernel_layout[];       /* boot.S; [8] = secondary PA */

#define SMP_RV_MAX   16  /* x16: the P6 amendment; QEMU virt takes -smp 16 */
#define SEC_STACK_SZ 8192

/* Per-hart stacks, .bss.  Static because the harts outlive every
 * allocator lifetime question — 8 KiB apiece, same budget as the
 * trap stack. */
static uint8_t sec_stacks[SMP_RV_MAX][SEC_STACK_SZ]
    __attribute__((aligned(16)));

static volatile uint64_t harts_online;       /* amoadd'd by secondaries */
static volatile uint64_t ipi_acks;

static uint64_t atomic_add(volatile uint64_t *p, uint64_t v)
{
    return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}

/* The secondary's C half (boot.S calls this with a0=hartid intact). */
void secondary_main_rv(uint64_t hartid)
{
    uint64_t n = atomic_add(&harts_online, 1);
    kprintf("[smp] hart %llu online (stack top %p, #%llu)\n",
            (unsigned long long)hartid,
            (void *)((uintptr_t)sec_stacks[hartid % SMP_RV_MAX]
                     + SEC_STACK_SZ),
            (unsigned long long)n);

    /* IPI round-trip: poll sip.SSIP (bit 1); clear it; ack. */
    for (;;) {
        uint64_t sip;
        __asm__ volatile("csrr %0, sip" : "=r"(sip));
        if (sip & (1UL << 1)) {
            __asm__ volatile("csrc sip, %0" :: "r"(1UL << 1));
            /* R3 CI fix: the LINE goes out BEFORE the counter ticks.
             * kprintf serialises under its lock, so once the boot
             * CPU sees acks==N every per-CPU ack line has already
             * drained -- run 32579828982 lost core 5's line to the
             * PSCI power-off racing a slow runner's ring. */
            kprintf("[smp] hart %llu: IPI received, parking\n",
                    (unsigned long long)hartid);
            atomic_add(&ipi_acks, 1);
            return;                 /* boot.S parks us in wfi */
        }
        __asm__ volatile("wfi");
    }
}

static void spin_delay(uint64_t loops)
{
    for (volatile uint64_t i = 0; i < loops; i++)
        ;
}

void smp_rv_bringup(uint64_t boot_hartid, const boot_info_t *bi)
{
    uint32_t ncpus = bi->cpu_count ? bi->cpu_count : 1;
    if (ncpus > SMP_RV_MAX)
        ncpus = SMP_RV_MAX;
    kprintf("[smp] %u hart(s) in the DTB, boot hart %llu\n",
            ncpus, (unsigned long long)boot_hartid);
    if (ncpus < 2) {
        kprintf("[smp] nothing to start (single-hart run)\n");
        return;
    }

    uint64_t entry_pa = kernel_layout[8];
    uint32_t started = 0;
    unsigned long mask = 0;

    for (uint32_t i = 0; i < ncpus; i++) {
        uint64_t hid = bi->cpus[i].lapic_id;         /* hartid slot */
        if (hid == boot_hartid)
            continue;
        uint64_t stack_top = (uintptr_t)sec_stacks[hid % SMP_RV_MAX]
                             + SEC_STACK_SZ;
        struct sbiret r = sbi_hart_start(hid, entry_pa, stack_top);
        if (r.error != 0) {
            kprintf("[smp] hart_start(%llu) FAILED: SBI error %ld\n",
                    (unsigned long long)hid, r.error);
            continue;
        }
        mask |= 1UL << hid;
        started++;
    }

    /* Wait for the report-ins (TCG is slow; bounded spin, honest
     * count either way). */
    for (int t = 0; t < 1000 && harts_online < started; t++)
        spin_delay(100000);
    kprintf("[smp] online: %llu/%u started hart(s)\n",
            (unsigned long long)harts_online, started);

    /* One IPI round-trip to everyone who reported in. */
    if (harts_online > 0) {
        struct sbiret r = sbi_send_ipi(mask, 0);
        if (r.error != 0) {
            kprintf("[smp] send_ipi FAILED: SBI error %ld\n", r.error);
            return;
        }
        for (int t = 0; t < 1000 && ipi_acks < harts_online; t++)
            spin_delay(100000);
        kprintf("[smp] IPI round-trip: %llu/%llu ack(s)\n",
                (unsigned long long)ipi_acks,
                (unsigned long long)harts_online);
    }
}
