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
 * enables its Local APIC, reports online and enters the scheduler's idle
 * loop.  The BSP waits for each AP to report online before waking the next.
 */

/* Initialise SMP: wake all APs and wait for them to come online. */
void smp_init(void);

/* Total number of CPUs currently online (including the BSP). */
uint32_t smp_get_cpu_count(void);

/* Number of CPUs the scheduler is allowed to place runnable threads on.
 * Fewer than (or equal to) smp_get_cpu_count(): until syscall/uaccess entry
 * state is fully per-CPU, schedule() keeps all real thread execution on the
 * BSP (cpu_id == 0, see kernel/proc/scheduler.c), so the load balancer must
 * never hand a thread to an AP's run queue -- it would sit there unscheduled
 * forever, and the BSP cannot rely on work-stealing to rescue it in time
 * (a bounded spin like scheduler_self_test()'s 20 sched_yield()s provably
 * hangs that way: one of the two test threads lands on the AP queue and
 * never runs to completion). */
uint32_t smp_get_schedulable_cpu_count(void);

/* Gate self-test: boot with QEMU -smp N and verify N CPUs come online. */
void smp_self_test(void);

#endif /* AURALITE_ARCH_X86_64_SMP_H */
