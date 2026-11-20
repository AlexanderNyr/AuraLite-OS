#ifndef AURALITE_ARCH_X86_64_APWAKE_H
#define AURALITE_ARCH_X86_64_APWAKE_H

/* RES-16: route RTC periodic interrupts to CPU 1, and check the saved RIP
 * in the RTC handler to prove that a device IRQ woke that AP from HLT. */
int smp_irq_ap_wake_selftest(void);

#endif /* AURALITE_ARCH_X86_64_APWAKE_H */
