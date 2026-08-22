/* lib/libc32/libc32.h -- the i386 bring-up libc (I386_PLAN I7;
 * PARITY P8 shrank it to a port shim over lib/libcmini/libcmini.h).
 *
 * What is left: the trap symbol (int 0x80 behind __syscall32) and
 * the suffixed helper names init32.c predates P8 with. */

#ifndef AURALITE_LIBC_H
#define AURALITE_LIBC_H

long __syscall32(long n, long a1, long a2, long a3, long a4, long a5);
#define AURA_SYSCALL __syscall32

#include "lib/libcmini/libcmini.h"

/* Pre-P8 names, kept for init32.c and the AURA_PUTS seam. */
#define strlen32 aura_strlen
#define puts32   aura_puts

#endif /* AURALITE_LIBC_H */
