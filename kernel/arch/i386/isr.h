/* kernel/arch/i386/isr.h -- exception/interrupt frame + dispatcher
 * (I386_PLAN I2). */

#ifndef AURALITE_ARCH_I386_ISR_H
#define AURALITE_ARCH_I386_ISR_H

#include <stdint.h>

/* Matches the push order in isr_stubs32.asm exactly (bottom = last push).
 * useresp/ss exist only when the interrupt arrived from Ring 3; the
 * dispatcher checks cs & 3 before touching them. */
struct registers32 {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;  /* pusha */
    uint32_t vector, error_code;
    uint32_t eip, cs, eflags;
    uint32_t useresp, ss;
} __attribute__((packed));

/* NOTE: pusha pushes in the order eax,ecx,edx,ebx,esp,ebp,esi,edi (so edi
 * ends up lowest); the struct above lists them ascending from the frame
 * pointer.  isr_stubs32.asm pushes DS after pusha, so ds sits below the
 * pusha block. */

void isr_dispatch32(struct registers32 *regs);

#endif /* AURALITE_ARCH_I386_ISR_H */
