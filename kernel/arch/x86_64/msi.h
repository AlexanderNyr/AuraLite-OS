/* kernel/arch/x86_64/msi.h — message-signalled interrupts on the local APIC
 * (RESIDUE3 C4 / RES-36).
 *
 * The device side (which capability, which table entry, which address/data
 * pair) is portable and lives in kernel/drivers/pci/msi_caps.c.  This header
 * is the OTHER half: the vectors a message may name, the handlers those
 * vectors dispatch to, and the EOI they owe.
 *
 * Vector allocation is deliberately primitive: one fixed block, handed out in
 * order, never reused.  A driver that needs a vector asks once at probe time;
 * a driver that needed to hand one back would be a driver with a
 * hot-plug/teardown path, and this kernel's virtio devices are probed once at
 * boot.  Saying that here is cheaper than a free-list nobody exercises.
 *
 * The block sits ABOVE everything else in use, and the constants below are
 * the whole map — an IDT gate exists for every one of the 256 vectors, so a
 * message to an unclaimed vector in this range is a quiet no-op rather than a
 * fault, but a message to a vector OUTSIDE it (a PIC IRQ, the TLB IPI) would
 * run somebody else's handler.
 */
#ifndef AURALITE_ARCH_X86_64_MSI_H
#define AURALITE_ARCH_X86_64_MSI_H

#include <stdint.h>

#include "drivers/pci/msi_caps.h"

/* 0x20-0x2F  PIC-remapped IRQs         (kernel/arch/x86_64/irq.c)
 * 0x30-0x3F  IOAPIC/PCI INTx lines     (same table, above the PIC block)
 * 0xF0       TLB shootdown IPI         (lapic.h)
 * 0x60-0x6F  THIS block: MSI/MSI-X vectors                       */
#define MSI_VECTOR_BASE   0x60u
#define MSI_VECTOR_COUNT  16

/* The MSI-X TABLE INDEX this driver programs (always entry 0: one message
 * per device is what a queue needs, and the table is per-function).  This is
 * NOT the x86 vector -- the vector goes in the table entry's DATA field, the
 * index goes in the device's own `queue_msix_vector` register.  Conflating
 * the two is the mistake that makes a working MSI-X setup answer NO_VECTOR:
 * measured on virtio-blk, whose table has 3 entries, so writing 96 there is
 * out of range while writing it into the entry would be perfect. */
#define MSI_TABLE_ENTRY  0u

/* Does this build actually program MSI?  Off only via the fw_cfg knob
 * (`-fw_cfg name=opt/auralite.msi,string=off`), which exists so the INTx
 * fallback has a lane that is not a hope. */
int  msi_enabled(void);
void msi_set_enabled(int on, const char *source);

/* Reserve a vector and bind a handler to it.  Returns the vector, or -1 when
 * the block is exhausted or MSI is switched off.  The handler runs in
 * interrupt context with interrupts disabled and MUST NOT sleep. */
int  msi_alloc_vector(void (*fn)(void *cookie), void *cookie, const char *name);

/* Call the handler bound to @vector and EOI it.  Returns 1 when the vector
 * belonged to this block, 0 when it did not (the caller then treats it as the
 * spurious/unknown case it is). */
int  msi_dispatch(uint8_t vector);

/* Attach a PCI function to a message-signalled interrupt in one step:
 * scan its capabilities, reserve a vector, program the device, and print the
 * receipt.  @io is the caller's transport (config accessors + BAR mapper).
 * Returns 0 and sets *vector_out on success; -1 when there is no capability
 * (INTx stays the default), -2 when the device refused, -3 on a transport
 * error, -4 when MSI is switched off or the vector block is exhausted. */
int  msi_attach_pci(const msi_pci_io_t *io, void (*fn)(void *cookie),
                    void *cookie, const char *name, uint32_t *vector_out);

/* Diagnostics for /proc and the gates. */
uint32_t msi_vectors_allocated(void);
uint32_t msi_interrupts_delivered(void);

#endif /* AURALITE_ARCH_X86_64_MSI_H */
