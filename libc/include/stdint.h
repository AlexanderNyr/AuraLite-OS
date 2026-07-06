#ifndef AURALITE_LIBC_STDINT_H
#define AURALITE_LIBC_STDINT_H

/*
 * stdint.h — POSIX.1-2024 <stdint.h> for AuraLite user programs.
 *
 * Both Clang and GCC ship a self-contained, freestanding-safe <stdint.h>
 * that defines intN_t/uintN_t/intptr_t/uintptr_t/INTn_MAX/SIZE_MAX/etc. from
 * compiler builtins with no host libc dependency (confirmed: -ffreestanding
 * makes the bundled header skip the __STDC_HOSTED__ branch that would
 * otherwise chain into glibc's <bits/...> headers).  This thin wrapper keeps
 * that behaviour while giving AuraLite a header of its own under libc/include
 * that future work (e.g. Q1.5 <inttypes.h>) can rely on unconditionally.
 */

#include_next <stdint.h>

#endif /* AURALITE_LIBC_STDINT_H */
