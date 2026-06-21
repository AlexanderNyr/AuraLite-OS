#ifndef AURALITE_ARCH_X86_64_GDT_H
#define AURALITE_ARCH_X86_64_GDT_H

#include <stdint.h>

/*
 * Global Descriptor Table for long mode.
 *
 * In 64-bit mode segmentation is mostly disabled: code uses a 64-bit segment
 * (L=1) and data segments cover the whole address space. We still need a valid
 * GDT so that segment selectors resolve correctly. Phase 8 adds user-mode
 * segments (Ring 3) and a TSS descriptor.
 *
 *   Index 0: null
 *   Index 1: kernel code (64-bit)     selector 0x08
 *   Index 2: kernel data              selector 0x10
 *   Index 3: user code (64-bit)       selector 0x18 | RPL3 = 0x1B
 *   Index 4: user data                selector 0x20 | RPL3 = 0x23
 *   Index 5: 64-bit TSS descriptor    selector 0x28
 *
 * NOTE: a 64-bit TSS descriptor is 16 bytes (occupies 2 slots), so the table
 * needs 7 entries total (indices 0-5 for code/data/TSS-lo, +6 for TSS-hi).
 */

#define GDT_NUM_ENTRIES 7   /* +1 for the upper half of the 16-byte TSS desc */

/* Segment selectors (index | RPL).
 *
 * NOTE: SYSRET loads CS = STAR[63:48]+0x10 and SS = STAR[63:48]+0x08.
 * We set STAR[63:48] = 0x10, so SYSRET produces CS=0x20|RPL3 and SS=0x18|RPL3.
 * Therefore user DATA must be at index 3 (0x18) and user CODE at index 4 (0x20).
 */
#define GDT_SEL_KCODE  0x08
#define GDT_SEL_KDATA  0x10
#define GDT_SEL_UDATA  0x18        /* user data segment (index 3) */
#define GDT_SEL_UCODE  0x20        /* user code segment (index 4) */
#define GDT_SEL_UCODE_R3 0x23      /* user code segment, RPL=3 */
#define GDT_SEL_UDATA_R3 0x1B      /* user data segment, RPL=3 */
#define GDT_SEL_TSS    0x28

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

/* Build the GDT and load it. */
void gdt_init(void);

/* Encode a 64-bit TSS descriptor at the given GDT index (16 bytes, so it
 * occupies index and index+1). Used by tss.c. */
void gdt_set_tss(int index, uint64_t base, uint32_t limit);

/* Encoded by gdt.c for tss.c to reference. */
extern struct gdt_entry gdt[GDT_NUM_ENTRIES];

#endif /* AURALITE_ARCH_X86_64_GDT_H */
