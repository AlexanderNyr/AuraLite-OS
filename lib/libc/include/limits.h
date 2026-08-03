#ifndef AURALITE_LIBC_LIMITS_H
#define AURALITE_LIBC_LIMITS_H

/*
 * limits.h — implementation and POSIX limits for AuraLite user programs.
 *
 * The integer-type ranges follow the LP64 model used by the x86_64-elf target
 * (int = 32-bit, long = 64-bit, char is signed).  The POSIX path/name limits
 * mirror the VFS constants in kernel/fs/vfs.h where applicable.
 */

/* ---- <limits.h> C standard: numeric type ranges (LP64) ---- */
#define CHAR_BIT    8

#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255

/* char is signed on this target. */
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX

#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535

#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U

#define LONG_MIN    (-9223372036854775807L - 1L)
#define LONG_MAX    9223372036854775807L
#define ULONG_MAX   18446744073709551615UL

#define LLONG_MIN   (-9223372036854775807LL - 1LL)
#define LLONG_MAX   9223372036854775807LL
#define ULLONG_MAX  18446744073709551615ULL

/* ---- POSIX limits ---- */
#define PATH_MAX    4096
#define NAME_MAX    255
#define ARG_MAX     131072
#define OPEN_MAX    64          /* matches VFS_MAX_FDS (kernel/fs/vfs.h) */
#define PIPE_BUF    4096        /* matches PIPE_BUF_SIZE (kernel/fs/vfs.c) */
#define NGROUPS_MAX 32          /* matches tcb_t::supplementary_gids[] (P7) */

#endif /* AURALITE_LIBC_LIMITS_H */
