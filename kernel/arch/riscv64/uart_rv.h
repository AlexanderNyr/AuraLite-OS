/* kernel/arch/riscv64/uart_rv.h -- 16550 RX over the PLIC
 * (RISCV_PLAN V7).
 *
 * The same 16550 programming model as COM1 (plan Fact 3), one
 * difference: registers are MMIO bytes at uart_base instead of port
 * I/O -- which is exactly why arch.h fences the port functions on
 * this arch.  V2 proved the PLIC line live with the THRE trick; V7
 * turns the line into the console's input path.
 */

#ifndef AURALITE_ARCH_RISCV64_UART_RV_H
#define AURALITE_ARCH_RISCV64_UART_RV_H

#include <stdint.h>

/* Program the 16550 (RX interrupt on), wire uart_irq through the
 * PLIC into the cons ring. */
void uart_rv_init(uint64_t base_hhdm_va, uint32_t irq);

/* Pop one byte from the interrupt-fed ring, or -1 when empty. */
int uart_rv_getc(void);

/* Bytes received by IRQ so far (the smoke test's proof that input
 * went through the PLIC path, not a poll). */
uint64_t uart_rv_rx_count(void);

#endif /* AURALITE_ARCH_RISCV64_UART_RV_H */
