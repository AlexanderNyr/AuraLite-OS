#ifndef NOVOS_ARCH_X86_64_ISR_H
#define NOVOS_ARCH_X86_64_ISR_H

#include <stdint.h>

/*
 * Saved register frame, matching the push order in isr.asm exactly.
 * Field 0 (r15) sits at the lowest stack address; the CPU-saved portion
 * (rip..ss) sits highest.
 *
 * In 64-bit mode the CPU always pushes SS, RSP, RFLAGS, CS, RIP for an
 * interrupt/exception (plus an error code where applicable), so this frame is
 * uniform regardless of privilege level.
 */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no;        /* pushed by stub */
    uint64_t err_code;      /* CPU-pushed, or stub-pushed dummy 0 */
    uint64_t rip, cs, rflags, rsp, ss;   /* CPU-saved */
};

/* Invoked by isr_common_stub in isr.asm with rdi = &registers. */
void isr_handler(struct registers *regs);

#endif /* NOVOS_ARCH_X86_64_ISR_H */
