#ifndef AURALITE_ARCH_X86_64_SMP_H
#define AURALITE_ARCH_X86_64_SMP_H

#include <stdint.h>

/*
 * Symmetric Multi-Processing.
 *
 * Uses Limine's MP request to wake application processors (APs). Each AP is
 * given a goto_address function that loads the kernel's GDT/IDT, switches to
 * its own stack, and enters an idle loop. The BSP waits for all APs to report
 * online before continuing.
 */

/* Initialise SMP: wake all APs via Limine and wait for them to come online. */
void smp_init(void);

/* Total number of CPUs currently online (including the BSP). */
uint32_t smp_get_cpu_count(void);

/* Gate self-test: boot with QEMU -smp N and verify N CPUs come online. */
void smp_self_test(void);

#endif /* AURALITE_ARCH_X86_64_SMP_H */
