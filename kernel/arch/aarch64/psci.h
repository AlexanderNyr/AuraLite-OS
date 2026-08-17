/* kernel/arch/aarch64/psci.h -- PSCI service calls (ARM64_PLAN A0). */

#ifndef AURALITE_ARCH_AARCH64_PSCI_H
#define AURALITE_ARCH_AARCH64_PSCI_H

void psci_system_off(void) __attribute__((noreturn));

#endif /* AURALITE_ARCH_AARCH64_PSCI_H */
