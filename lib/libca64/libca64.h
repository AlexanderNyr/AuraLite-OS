/* lib/libca64/libca64.h -- the a64 bring-up libc (ARM64_PLAN A5b;
 * PARITY P8 shrank it to a port shim over lib/libcmini/libcmini.h).
 *
 * What is left: the trap symbol (svc behind __syscall_a64) and the
 * suffixed helper names inita64.c predates P8 with. */

#ifndef AURALITE_LIBCA64_H
#define AURALITE_LIBCA64_H

long __syscall_a64(long n, long a1, long a2, long a3, long a4, long a5);
#define AURA_SYSCALL __syscall_a64

#include "lib/libcmini/libcmini.h"

/* Pre-P8 names, kept for inita64.c and the AURA_PUTS seam. */
#define strlen_a64 aura_strlen
#define puts_a64   aura_puts

#endif /* AURALITE_LIBCA64_H */
