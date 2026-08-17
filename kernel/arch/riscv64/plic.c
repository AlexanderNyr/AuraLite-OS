/* kernel/arch/riscv64/plic.c -- PLIC driver (RISCV_PLAN V2).
 *
 * Register map (SiFive PLIC spec, what qemu-virt implements):
 *
 *   base + 0x000000 + irq*4   priority for line irq (0 = never fires)
 *   base + 0x002000 + ctx*0x80  enable bits, 32 lines per word
 *   base + 0x200000 + ctx*0x1000 + 0  threshold (fires if prio > thr)
 *   base + 0x200000 + ctx*0x1000 + 4  claim (read) / complete (write)
 *
 * Context numbering on virt: hart N's M-context is 2N, its S-context
 * is 2N+1.  Only the boot hart's S-context is programmed in V2.
 *
 * MMIO through volatile uint32_t: the PLIC is a 32-bit peripheral;
 * satp=0 in V2 so base is a physical address used directly (V3 moves
 * this behind the HHDM, same as every other MMIO user).
 */

#include <stdint.h>

#include "kernel/arch/riscv64/plic.h"

#define PLIC_PRIORITY(irq)   (0x000000u + (irq) * 4u)
#define PLIC_ENABLE(ctx)     (0x002000u + (ctx) * 0x80u)
#define PLIC_THRESHOLD(ctx)  (0x200000u + (ctx) * 0x1000u)
#define PLIC_CLAIM(ctx)      (0x200000u + (ctx) * 0x1000u + 4u)

#define PLIC_MAX_IRQ 96      /* virt wires 96 sources; enough said */

static volatile uint8_t *plic;         /* MMIO base */
static uint32_t s_ctx;                 /* boot hart's S-context */
static plic_handler_t handlers[PLIC_MAX_IRQ];
static volatile uint64_t completions;

static inline volatile uint32_t *reg32(uint32_t off)
{
    return (volatile uint32_t *)(plic + off);
}

void plic_init(uint64_t base, uint64_t hart)
{
    plic  = (volatile uint8_t *)base;
    s_ctx = (uint32_t)(hart * 2 + 1);

    /* Threshold 0: any priority > 0 fires.  Lines stay disabled until
     * plic_enable is asked for them by name. */
    *reg32(PLIC_THRESHOLD(s_ctx)) = 0;
}

void plic_enable(uint32_t irq, plic_handler_t fn)
{
    if (!plic || irq == 0 || irq >= PLIC_MAX_IRQ)
        return;
    handlers[irq] = fn;
    *reg32(PLIC_PRIORITY(irq)) = 1;
    volatile uint32_t *en = reg32(PLIC_ENABLE(s_ctx) + (irq / 32) * 4);
    *en |= 1u << (irq % 32);
}

void plic_dispatch(void)
{
    /* Claim until the well is dry: multiple lines can be pending
     * behind one S-external interrupt. */
    for (;;) {
        uint32_t irq = *reg32(PLIC_CLAIM(s_ctx));
        if (irq == 0)
            return;
        if (irq < PLIC_MAX_IRQ && handlers[irq])
            handlers[irq](irq);
        /* Complete by writing the id back -- even for a line without
         * a handler; a stuck claim gates every lower-priority line. */
        *reg32(PLIC_CLAIM(s_ctx)) = irq;
        completions++;
    }
}

uint64_t plic_completions(void)
{
    return completions;
}
