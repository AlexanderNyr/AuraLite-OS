/* gdt.c — build and load the long-mode GDT (kernel + user + TSS descriptors).
 *
 * Phase 8: added user code/data segments (Ring 3) and a TSS descriptor slot
 * (filled by tss_init). The TSS gives the CPU a known Ring-0 stack (RSP0) on
 * Ring 3 -> Ring 0 transitions.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/gdt.h"

/* Implemented in gdt_flush.asm: void gdt_flush(uint64_t gdtr_ptr); */
extern void gdt_flush(uint64_t gdtr_ptr);

/* Made non-static so tss.c can write the TSS descriptor (index 5) and smp.c
 * can reload the GDT on application processors. */
struct gdt_entry gdt[GDT_NUM_ENTRIES];
struct gdt_ptr   gdtr;

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

/*
 * Encode a 64-bit TSS descriptor (System descriptor, type 0x9 = available
 * 64-bit TSS). Unlike code/data descriptors, a 64-bit system descriptor is
 * 16 bytes: the first 8 bytes look like a normal entry, the second 8 bytes
 * hold the upper 32 bits of the base. Since our TSS lives in the higher half
 * (> 4 GiB), we MUST write the upper base bits, not just zero them.
 */
void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
    /* access: Present | DPL=0 | type=0x9 (available 64-bit TSS) => 0x89 */
    gdt[index].access      = 0x89;
    /* The next 8 bytes (index+1) hold bits 32..63 of the base in its low 32
     * bits; the high 32 bits must be zero. */
    if (index + 1 < GDT_NUM_ENTRIES) {
        uint64_t upper = base >> 32;
        gdt[index + 1].limit_low   = (uint16_t)(upper & 0xFFFF);
        gdt[index + 1].base_low    = (uint16_t)((upper >> 16) & 0xFFFF);
        gdt[index + 1].base_middle = 0;
        gdt[index + 1].access      = 0;
        gdt[index + 1].granularity = 0;
        gdt[index + 1].base_high   = 0;
    }
}

void gdt_init(void) {
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)&gdt;

    gdt_set_entry(0, 0, 0,         0x00, 0x00);   /* null              */
    gdt_set_entry(1, 0, 0xFFFFF,   0x9A, 0xA0);   /* kernel code (64b) */
    gdt_set_entry(2, 0, 0xFFFFF,   0x92, 0xC0);   /* kernel data       */
    /* User segments: DPL=3 (0x60 sets the DPL bits). Swapped order (data
     * before code) so SYSRET's formula (SS=base+8, CS=base+16) matches. */
    gdt_set_entry(3, 0, 0xFFFFF,   0x92 | 0x60, 0xC0);  /* user data       */
    gdt_set_entry(4, 0, 0xFFFFF,   0x9A | 0x60, 0xA0);  /* user code (64b) */
    /* Index 5: TSS descriptor — filled by tss_init via gdt_set_tss(). */

    gdt_flush((uint64_t)(uintptr_t)&gdtr);
}
