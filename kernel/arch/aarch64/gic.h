/* kernel/arch/aarch64/gic.h -- GICv2 driver (ARM64_PLAN A2). */

#ifndef AURALITE_ARCH_AARCH64_GIC_H
#define AURALITE_ARCH_AARCH64_GIC_H

#include <stdint.h>

typedef void (*gic_handler_t)(uint32_t intid);

/* Bring up the distributor + this CPU's interface.  Bases come from
 * the DTB walk (A1 discovered GICD 0x8000000 / GICC 0x8010000). */
/* R4: `is_v3` comes from the DTB compatible (see fdt.h) -- with v3
 * the second base is the REDISTRIBUTOR region. */
void gic_init(uint64_t gicd_base, uint64_t gicc_base, int is_v3);

/* Enable one INTID and route it to fn.  INTIDs arrive PRE-NORMALISED
 * from the A1 walker (SPI+32 / PPI+16 already applied) -- this driver
 * never adds 32 to anything, by design. */
void gic_enable(uint32_t intid, gic_handler_t fn);

/* IAR claim / EOIR complete loop; called from the IRQ vector. */
void gic_dispatch(void);

/* Completions so far (the smoke's claim/complete gate reads this). */
uint64_t gic_completions(void);

/* ---- R4: the GICv3 lane (chosen by the DTB at init) --------------- */
int gic_is_v3(void);
volatile uint8_t *gic_v3_own_rdist(void);   /* this PE's GICR frame */
void gic_v3_wake_rdist(volatile uint8_t *rd);
void gic_v3_cpu_init(void);

#endif /* AURALITE_ARCH_AARCH64_GIC_H */
