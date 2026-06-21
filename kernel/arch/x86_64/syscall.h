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

/* The C-level syscall dispatch (called from syscall_entry.asm). */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* AURALITE_ARCH_X86_64_SYSCALL_H */
