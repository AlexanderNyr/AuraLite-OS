/* kernel/arch/i386/gdt.c -- build and load the 32-bit GDT + TSS
 * (I386_PLAN I2).
 *
 * Sibling of kernel/arch/x86_64/gdt.c with the two structural
 * differences 32-bit protected mode imposes:
 *
 *   1. Code/data descriptors carry real base/limit semantics (we still
 *      use flat 0..4GiB, G=1, D=1 -- but the CPU checks them, unlike
 *      long mode where most fields are ignored).
 *   2. The TSS descriptor is 8 bytes (type 0x9 = available 32-bit TSS),
 *      not the 16-byte 64-bit variant, and the TSS itself is the
 *      ring-transition mechanism: ESP0/SS0 are consulted on every
 *      Ring 3 -> Ring 0 interrupt.  tss_set_esp0() is therefore part of
 *      the context-switch contract from phase I4 on.
 */

#include <stdint.h>
#include "kernel/arch/i386/gdt.h"

/* Implemented in gdt_flush32.asm. */
extern void gdt_flush32(uint32_t gdtr_ptr);
extern void tss_flush32(uint16_t selector);

static struct gdt_entry gdt[GDT_NUM_ENTRIES];
static struct gdt_ptr   gdtr;
static struct tss32     tss;

static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[index].granularity |= (flags & 0xF0);

    gdt[index].access      = access;
}

void tss_set_esp0(uint32_t esp0)
{
    tss.esp0 = esp0;
}

void gdt_init(void)
{
    /* null */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Flat 4 GiB segments.  access: P | DPL | S | type.
     * 0x9A = present, ring 0, code, R/E;  0x92 = present, ring 0, data, R/W.
     * 0xFA / 0xF2 are the DPL=3 variants.
     * flags 0xC0 = G (4 KiB granularity) | D (32-bit). */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);   /* kernel code */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);   /* kernel data */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);   /* user   code */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);   /* user   data */

    /* TSS descriptor: type 0x9 (available 32-bit TSS), byte granularity,
     * limit = sizeof - 1.  access 0x89 = P | DPL0 | type 9. */
    for (unsigned i = 0; i < sizeof(tss); i++)
        ((uint8_t *)&tss)[i] = 0;
    tss.ss0        = KERNEL_DATA_SELECTOR;
    tss.esp0       = 0;                          /* armed per-thread in I4 */
    tss.iomap_base = sizeof(tss);                /* no I/O bitmap */
    gdt_set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint32_t)&gdt;

    gdt_flush32((uint32_t)&gdtr);
    tss_flush32(TSS_SELECTOR);
}
