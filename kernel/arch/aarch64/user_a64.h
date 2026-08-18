/* kernel/arch/aarch64/user_a64.h -- EL0 + svc (ARM64_PLAN A4).
 * user_rv.h's sibling: the same D4 numbers, the fourth trap
 * mechanism into the one syscall table. */

#ifndef AURALITE_ARCH_AARCH64_USER_A64_H
#define AURALITE_ARCH_AARCH64_USER_A64_H

#include <stdint.h>

#include "kernel/arch/aarch64/trap_a64.h"

/* D4: AuraLite's numbers (Linux-x86_64-tracking), the SAME table the
 * other three trap mechanisms dispatch -- and worth restating on
 * this arch in particular: Linux's OWN aarch64 numbers diverge from
 * its x86_64 ones (openat 56, write 64...), which is exactly why D4
 * said "one table, ours".  Nothing renumbers. */
#define SYS_A64_READ    0
#define SYS_A64_WRITE   1
#define SYS_A64_GETPID  39
#define SYS_A64_EXIT    60
#define SYS_A64_YIELD   158

/* Register convention: x8 = number, x0-x5 = args, x0 = return with
 * in-band negative errno (the Linux aarch64 convention, kept). */

/* Run a flat binary image at USER_TEXT_VADDR_A64 (stack below it) to
 * completion; returns its exit code, or negative on setup failure. */
#define USER_TEXT_VADDR_A64 0x0000000040000000UL

int user_a64_run_image(const uint8_t *code, uint64_t code_len);

/* The [user] gate: the exit-42 round trip + the privileged-op
 * negative control.  0 = pass. */
int user_a64_selftest(void);

/* trap_a64.c hooks: the SVC path and the contained-fault path. */
void user_a64_syscall(a64_trap_frame_t *f);
int  user_a64_fault(a64_trap_frame_t *f, uint64_t esr, uint64_t far);

#endif /* AURALITE_ARCH_AARCH64_USER_A64_H */
