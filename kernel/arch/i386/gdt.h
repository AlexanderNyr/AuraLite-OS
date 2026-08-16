/* kernel/arch/i386/gdt.h -- 32-bit GDT layout (I386_PLAN I2).
 *
 * Same selector assignments as the x86_64 GDT so the two kernels share
 * one mental model:
 *
 *   0x00  null
 *   0x08  kernel code   (Ring 0)
 *   0x10  kernel data   (Ring 0)
 *   0x18  user code     (Ring 3)  -> selector | 3 = 0x1B when loaded
 *   0x20  user data     (Ring 3)  -> selector | 3 = 0x23 when loaded
 *   0x28  TSS           (one slot -- a 32-bit TSS descriptor is 8 bytes,
 *                        not 16 like the 64-bit one, so no spill entry)
 *
 * The i386 TSS matters more than its 64-bit sibling: it is the ONLY
 * ring-transition stack mechanism (there is no SYSCALL MSR pair), so
 * ESP0/SS0 must be valid before the first Ring 3 entry (phase I4).
 */

#ifndef AURALITE_ARCH_I386_GDT_H
#define AURALITE_ARCH_I386_GDT_H

#include <stdint.h>

#define GDT_NUM_ENTRIES        6

#define KERNEL_CODE_SELECTOR   0x08
#define KERNEL_DATA_SELECTOR   0x10
#define USER_CODE_SELECTOR     0x18
#define USER_DATA_SELECTOR     0x20
#define TSS_SELECTOR           0x28

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* The 32-bit TSS (Intel SDM Vol.3 s.8.2.1).  Only ESP0/SS0 are used for
 * ring transitions; the hardware task-switching fields are dead weight
 * required by the layout. */
struct tss32 {
    uint32_t prev_task;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);
void tss_set_esp0(uint32_t esp0);

#endif /* AURALITE_ARCH_I386_GDT_H */
