/* gdt.c — build and load the flat long-mode GDT. */

#include <stdint.h>
#include "kernel/arch/x86_64/gdt.h"

/* Implemented in gdt_flush.asm: void gdt_flush(uint64_t gdtr_ptr); */
extern void gdt_flush(uint64_t gdtr_ptr);

static struct gdt_entry gdt[GDT_NUM_ENTRIES];
static struct gdt_ptr   gdtr;

/*
 * Encode one GDT descriptor.
 *
 * @param  index   slot in the table
 * @param  base    32-bit base (always 0 for our flat segments)
 * @param  limit   20-bit segment limit (granularity 4 KiB makes this 4 GiB)
 * @param  access  access byte (present, type, S, DPL)
 * @param  flags   high nibble of the granularity byte: G, D/B, L, AVL
 */
static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[index].granularity |= (flags & 0xF0);

    gdt[index].access      = access;
}

void gdt_init(void) {
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)&gdt;

    /* 0x9A = Present|Ring0|Code|Execute/Read. flags 0xA0 => G=1, L=1 (64-bit). */
    gdt_set_entry(0, 0, 0,         0x00, 0x00);   /* null */
    gdt_set_entry(1, 0, 0xFFFFF,   0x9A, 0xA0);   /* kernel code (64-bit) */
    /* 0x92 = Present|Ring0|Data|Read/Write. flags 0xC0 => G=1, D/B=1.       */
    gdt_set_entry(2, 0, 0xFFFFF,   0x92, 0xC0);   /* kernel data */

    gdt_flush((uint64_t)(uintptr_t)&gdtr);
}
