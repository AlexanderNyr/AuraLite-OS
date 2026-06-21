#ifndef AURALITE_ARCH_X86_64_SYSCALL_H
#define AURALITE_ARCH_X86_64_SYSCALL_H

#include <stdint.h>

/*
 * Fast system call interface (SYSCALL/SYSRET).
 *
 * Configures the MSRs (STAR, LSTAR, SFMASK) so that the SYSCALL instruction
 * transfers control to a kernel handler in Ring 0 without going through the
 * IDT. The convention (Linux-compatible):
 *
 *   syscall number : RAX
 *   arguments      : RDI, RSI, RDX, R10, R8, R9
 *   return value   : RAX
 */

void syscall_init(void);

/* Set the kernel stack that the SYSCALL handler switches to (must be called
 * before entering Ring 3). This prevents the kernel from running on the
 * user's stack, which would corrupt user data. */
void set_syscall_stack(uint64_t stack_top);

/* The C-level syscall dispatch (called from syscall_entry.asm). */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* AURALITE_ARCH_X86_64_SYSCALL_H */
