#ifndef AURALITE_ARCH_X86_64_SYSCALL_H
#define AURALITE_ARCH_X86_64_SYSCALL_H

#include <stdint.h>
#include "kernel/arch/x86_64/cpu_local.h"

/* Per-CPU SYSCALL entry-state accessors.  The captured user frame lives in
 * struct cpu_local (see kernel/arch/x86_64/syscall_entry.asm's SMP MODEL
 * comment) -- it used to live in .data globals, which raced the moment two
 * CPUs could run syscalls concurrently.  These macros keep the existing C
 * call sites (`syscall_saved_rcx` et al.) working unchanged against the
 * current CPU's slots. */
#define syscall_saved_rcx  (get_cpu_local()->syscall_saved_rip)
#define syscall_saved_r11  (get_cpu_local()->syscall_saved_rflags)
#define syscall_saved_rsp  (get_cpu_local()->syscall_saved_rsp)
#define syscall_saved_rbx  (get_cpu_local()->syscall_saved_rbx)
#define syscall_saved_rbp  (get_cpu_local()->syscall_saved_rbp)
#define syscall_saved_r12  (get_cpu_local()->syscall_saved_r12)
#define syscall_saved_r13  (get_cpu_local()->syscall_saved_r13)
#define syscall_saved_r14  (get_cpu_local()->syscall_saved_r14)
#define syscall_saved_r15  (get_cpu_local()->syscall_saved_r15)

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

int is_restartable(uint64_t num);

#endif /* AURALITE_ARCH_X86_64_SYSCALL_H */
