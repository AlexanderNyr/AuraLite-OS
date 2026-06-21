#ifndef AURALITE_ARCH_X86_64_GDT_H
#define AURALITE_ARCH_X86_64_GDT_H

/*
 * Minimal flat Global Descriptor Table for long mode.
 *
 * In 64-bit mode segmentation is mostly disabled: code uses a 64-bit segment
 * (L=1) and data segments cover the whole address space. We still need a valid
 * GDT so that segment selectors (CS=0x08, DS/SS=0x10) resolve correctly, which
 * is required for our own interrupt/syscall entries later.
 */

#define GDT_NUM_ENTRIES 3   /* null, kernel code, kernel data */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;   /* bits 0-3: limit[19:16]; bits 4-7: flags G/D/L/A */
    uint8_t  base_high;
} __attribute__((packed));

/* 10-byte pseudo-descriptor loaded by LGDT: 16-bit limit + 64-bit base. */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Build the GDT and load it. Defined in gdt.c; the actual LGDT + segment
   reload is performed by gdt_flush() in gdt_flush.asm. */
void gdt_init(void);

#endif /* AURALITE_ARCH_X86_64_GDT_H */
