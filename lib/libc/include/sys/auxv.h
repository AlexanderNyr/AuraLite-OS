#ifndef AURALITE_LIBC_SYS_AUXV_H
#define AURALITE_LIBC_SYS_AUXV_H

/*
 * sys/auxv.h -- auxiliary-vector access (POSIX.1-2024 / glibc).
 *
 * The kernel builds the auxv on the initial process stack (M5, MATURITY_PLAN);
 * libc's __libc_start_main records a pointer to it and getauxval() scans for a
 * requested entry.  Useful for AT_PAGESZ, AT_RANDOM (a 16-byte seed), AT_PHDR,
 * AT_ENTRY, AT_EXECFN, the credentials, etc.
 */

#include <stdint.h>

/* AT_* entry types (Linux/ELF ABI). */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_EXECFN  31

/* Return the value of auxiliary-vector entry @type, or 0 if absent. */
unsigned long getauxval(unsigned long type);

#endif /* AURALITE_LIBC_SYS_AUXV_H */
