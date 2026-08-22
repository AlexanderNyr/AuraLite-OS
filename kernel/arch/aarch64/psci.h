/* kernel/arch/aarch64/psci.h -- PSCI service calls (ARM64_PLAN A0). */

#ifndef AURALITE_ARCH_AARCH64_PSCI_H
#define AURALITE_ARCH_AARCH64_PSCI_H

/* P6: start a powered-off core at a PHYSICAL address; the context
 * argument arrives in the target's x0 (we pass its stack top).
 * Returns the PSCI error code (0 = success). */
long psci_cpu_on(uint64_t target_mpidr, uint64_t entry_pa,
                 uint64_t context);

void psci_system_off(void) __attribute__((noreturn));

#endif /* AURALITE_ARCH_AARCH64_PSCI_H */
