/* kernel/arch/riscv64/plic.h -- Platform-Level Interrupt Controller
 * (RISCV_PLAN V2).
 *
 * The PLIC is this platform's IOAPIC-shaped thing: external device
 * lines fan in, one hart context claims/completes.  V2 scope is the
 * boot hart's S-context only (BSP-only discipline until V4's
 * scheduler exists to give other harts work).
 */

#ifndef AURALITE_ARCH_RISCV64_PLIC_H
#define AURALITE_ARCH_RISCV64_PLIC_H

#include <stdint.h>

typedef void (*plic_handler_t)(uint32_t irq);

/* Map the PLIC at base (from the DTB), set threshold 0 for the boot
 * hart's S-context.  hart is the boot hartid (context number derives
 * from it: context = hart * 2 + 1 for S-mode on virt). */
void plic_init(uint64_t base, uint64_t hart);

/* Enable an IRQ line at priority 1 and register its handler. */
void plic_enable(uint32_t irq, plic_handler_t fn);

/* Claim/complete loop -- called from the trap handler on S-external. */
void plic_dispatch(void);

/* Round-trips completed (claim followed by complete) -- the V2 gate
 * reads this to prove the path with a real device interrupt. */
uint64_t plic_completions(void);

#endif /* AURALITE_ARCH_RISCV64_PLIC_H */
