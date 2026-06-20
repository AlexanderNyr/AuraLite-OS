#ifndef NOVOS_ARCH_X86_64_IDT_H
#define NOVOS_ARCH_X86_64_IDT_H

#include <stdint.h>

/*
 * 64-bit IDT gate descriptor (16 bytes). See Intel SDM Vol.3, Figure 6-7:
 * "Format of a 64-Bit IDT Gate Descriptor".
 */

#define IDT_ENTRIES 256

#define KERNEL_CODE_SELECTOR 0x08

struct idt_entry {
    uint16_t offset_low;    /* bits 0..15 of handler address  */
    uint16_t selector;      /* code segment selector in GDT    */
    uint8_t  ist;           /* IST index (0 = use current RSP0) */
    uint8_t  type_attr;     /* P:DPL:0:GateType                */
    uint16_t offset_mid;    /* bits 16..31                      */
    uint32_t offset_high;   /* bits 32..63                      */
    uint32_t zero;          /* reserved, must be 0              */
} __attribute__((packed));

/* 10-byte pseudo-descriptor loaded by LIDT. */
struct idt_ptr {
    uint16_t limit;         /* one less than table size in bytes */
    uint64_t base;          /* linear address of the IDT          */
} __attribute__((packed));

/*
 * type_attr encodings (present | DPL<<5 | 0 | gate type).
 *   Gate type 0xE = 64-bit interrupt gate, 0xF = 64-bit trap gate.
 */
#define IDT_GATE_INTERRUPT 0x8E   /* P=1, DPL=0, interrupt gate */
#define IDT_GATE_TRAP      0x8F   /* P=1, DPL=0, trap gate      */
#define IDT_GATE_USER_INT  0xEE   /* P=1, DPL=3, interrupt gate */

void idt_init(void);
void idt_set_gate(int n, uint64_t handler, uint8_t flags);

#endif /* NOVOS_ARCH_X86_64_IDT_H */
