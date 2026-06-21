#ifndef AURALITE_ARCH_X86_64_TSS_H
#define AURALITE_ARCH_X86_64_TSS_H

#include <stdint.h>

/*
 * 64-bit Task State Segment (TSS).
 *
 * In long mode the TSS is NOT used for hardware task switching. Its only
 * critical field is RSP0: when an interrupt or exception transfers control
 * from Ring 3 to Ring 0, the CPU loads RSP from TSS.RSP0 (to give the kernel a
 * known, safe stack). The remaining fields (IST1-7, IOPB) are reserved/zero
 * for now.
 *
 * We also use the IST (Interrupt Stack Table) to give the double-fault (#DF)
 * handler its own stack, so a kernel-stack overflow cannot escalate to a
 * triple fault.
 */

struct tss_entry {
    uint32_t reserved0;
    uint32_t rsp0_low;
    uint32_t rsp0_high;
    uint32_t rsp1_low;
    uint32_t rsp1_high;
    uint32_t rsp2_low;
    uint32_t rsp2_high;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t ist1_low;
    uint32_t ist1_high;
    uint32_t ist2_low;
    uint32_t ist2_high;
    uint32_t ist3_low;
    uint32_t ist3_high;
    uint32_t ist4_low;
    uint32_t ist4_high;
    uint32_t ist5_low;
    uint32_t ist5_high;
    uint32_t ist6_low;
    uint32_t ist6_high;
    uint32_t ist7_low;
    uint32_t ist7_high;
    uint32_t reserved3;
    uint32_t reserved4;
    uint16_t reserved5;
    uint16_t iomap_base;
} __attribute__((packed));

/* Build the TSS descriptor in the GDT, set RSP0, and load TR. */
void tss_init(void);

/* Update RSP0 (the kernel stack used on Ring 3 -> Ring 0 transitions).
 * Called when switching to a user thread so each has its own kernel stack. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* AURALITE_ARCH_X86_64_TSS_H */
