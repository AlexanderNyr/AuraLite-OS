#ifndef AURALITE_ARCH_X86_64_IRQ_H
#define AURALITE_ARCH_X86_64_IRQ_H

#include <stdint.h>
#include "kernel/arch/x86_64/isr.h"

/*
 * 8259A PIC I/O ports and the End-Of-Interrupt command. IRQ lines are remapped
 * by pic_init() onto vectors PIC_OFFSET .. PIC_OFFSET+15 (32..47).
 */

#define PIC1_CMD      0x20
#define PIC1_DATA     0x21
#define PIC2_CMD      0xA0
#define PIC2_DATA     0xA1

#define PIC_EOI       0x20
#define PIC_OFFSET    0x20

/* IMCR (Interrupt Mode Control Register) -- present on MP-compliant chipsets.
 * Selects whether the legacy 8259 PICs feed INTR straight into the BSP
 * (PIC/"Virtual Wire" mode, value 0x00) or whether all interrupt delivery
 * must go through the I/O APIC (APIC mode, value 0x01).  Real UEFI firmware
 * commonly leaves the chipset in APIC mode by the time the OS gets control
 * (Windows/Linux always use the I/O APIC), which silently disconnects the
 * 8259 from the CPU: IRQ0 (PIT)/IRQ1 (keyboard)/IRQ12 (mouse) then never
 * fire even though pic_init()/irq_register_handler() report success. QEMU's
 * default `-M pc` machine leaves IMCR in PIC mode, which is why this bug is
 * invisible under emulation and only surfaces on physical hardware. Since
 * this kernel does not yet drive the I/O APIC, force PIC mode explicitly. */
#define IMCR_ADDR_PORT  0x22
#define IMCR_DATA_PORT  0x23
#define IMCR_SELECT     0x70   /* select the IMCR register via port 0x22 */
#define IMCR_MODE_PIC   0x00   /* route 8259 INTR directly to the BSP    */
#define IMCR_MODE_APIC  0x01   /* decouple 8259; deliver via I/O APIC    */

typedef void (*irq_handler_t)(struct registers *regs);

void pic_init(void);
void pic_eoi(int irq);
void pic_mask(int irq);
void pic_unmask(int irq);

/* Switch the system off the 8259 for I/O-APIC delivery: mask all 16 PIC IRQs
 * and force the IMCR into APIC mode so the 8259 is fully decoupled from the
 * BSP's INTR line.  Called by ioapic_init() once the I/O APIC is driving the
 * legacy IRQs.  No-op/harmless on chipsets without an IMCR. */
void pic_disable_for_apic(void);

void irq_register_handler(int irq, irq_handler_t handler);
void irq_dispatch(int irq, struct registers *regs);

#endif /* AURALITE_ARCH_X86_64_IRQ_H */
