/* lib/libcrv/libcrv.h -- the rv64 bring-up libc (RISCV_PLAN V5;
 * PARITY P8 shrank it to a port shim over lib/libcmini/libcmini.h).
 *
 * What is left here is exactly what differs per port: the trap
 * symbol (ecall behind __syscall_rv, crt0-side) and the suffixed
 * helper names the init programs predate P8 with. */

#ifndef AURALITE_LIBCRV_H
#define AURALITE_LIBCRV_H

long __syscall_rv(long n, long a1, long a2, long a3, long a4, long a5);
#define AURA_SYSCALL __syscall_rv

#include "lib/libcmini/libcmini.h"

/* Pre-P8 names, kept for initrv.c and the AURA_PUTS seam. */
#define strlen_rv aura_strlen
#define puts_rv   aura_puts

#endif /* AURALITE_LIBCRV_H */
