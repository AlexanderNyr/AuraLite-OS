#ifndef AURALITE_ARCH_X86_64_SMP_H
#define AURALITE_ARCH_X86_64_SMP_H

#include <stdint.h>

/*
 * Symmetric Multi-Processing.
 *
 * smp_init() wakes every application processor (AP) enumerated by the
 * bootloader's ACPI MADT walk (boot_info.cpus[]) with the classic
 * INIT-SIPI-SIPI sequence (see kernel/arch/x86_64/smp.c).  Each AP loads
 * the kernel's GDT/IDT, installs its per-CPU TSS and cpu_local state,
 * enables its Local APIC, arms its own calibrated LAPIC timer tick (its
 * local preemption source), reports online and enters the scheduler's idle
 * loop with interrupts on.  The BSP waits for each AP to report online
 * before waking the next.  Since step 3.2, all online CPUs execute real
 * threads in parallel (see kernel/proc/scheduler.c).
 */

/* Initialise SMP: wake all APs and wait for them to come online. */
void smp_init(void);

/* Total number of CPUs currently online (including the BSP). */
uint32_t smp_get_cpu_count(void);

/* OPT_PLAN.md O5: LAPIC id of a kernel cpu id (BSP = 0), for the
 * shootdown sender's addressed IPIs. */
uint32_t smp_get_lapic_id(uint32_t cpu_id);

/* Number of CPUs the scheduler is allowed to place runnable threads on.
 * Since SMP step 3.2 this equals smp_get_cpu_count(): every online CPU has
 * a run queue, per-CPU syscall/TSS state and its own LAPIC timer tick, so
 * threads scheduled onto any queue genuinely execute there. */
uint32_t smp_get_schedulable_cpu_count(void);

/* Gate self-test: boot with QEMU -smp N and verify N CPUs come online. */
void smp_self_test(void);

#endif /* AURALITE_ARCH_X86_64_SMP_H */
