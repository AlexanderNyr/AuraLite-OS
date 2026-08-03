#ifndef AURALITE_LIBC_INTTYPES_H
#define AURALITE_LIBC_INTTYPES_H

/*
 * inttypes.h — POSIX.1-2024 <inttypes.h> for AuraLite user programs.
 *
 * Format-macro values assume the LP64 data model used throughout AuraLite's
 * x86_64-elf target: int is 32-bit, long/pointer are 64-bit.  Per both
 * Clang's and GCC's freestanding <stdint.h>, int64_t/intmax_t (and their
 * unsigned counterparts) are `long`/`unsigned long` in this data model, not
 * `long long` -- the format macros below reflect that.
 */

#include <stdint.h>

/* ---- printf() length-modified conversion specifiers ---- */

#define PRId8   "d"
#define PRIi8   "i"
#define PRIo8   "o"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"

#define PRId16  "d"
#define PRIi16  "i"
#define PRIo16  "o"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"

#define PRId32  "d"
#define PRIi32  "i"
#define PRIo32  "o"
#define PRIu32  "u"
#define PRIx32  "x"
#define PRIX32  "X"

#define PRId64  "ld"
#define PRIi64  "li"
#define PRIo64  "lo"
#define PRIu64  "lu"
#define PRIx64  "lx"
#define PRIX64  "lX"

#define PRIdLEAST8  PRId8
#define PRIdLEAST16 PRId16
#define PRIdLEAST32 PRId32
#define PRIdLEAST64 PRId64
#define PRIuLEAST8  PRIu8
#define PRIuLEAST16 PRIu16
#define PRIuLEAST32 PRIu32
#define PRIuLEAST64 PRIu64

#define PRIdFAST8   PRId32
#define PRIdFAST16  PRId32
#define PRIdFAST32  PRId32
#define PRIdFAST64  PRId64
#define PRIuFAST8   PRIu32
#define PRIuFAST16  PRIu32
#define PRIuFAST32  PRIu32
#define PRIuFAST64  PRIu64

#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIoMAX "lo"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"

#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

/* ---- scanf() length-modified conversion specifiers ---- */

#define SCNd8   "hhd"
#define SCNi8   "hhi"
#define SCNo8   "hho"
#define SCNu8   "hhu"
#define SCNx8   "hhx"

#define SCNd16  "hd"
#define SCNi16  "hi"
#define SCNo16  "ho"
#define SCNu16  "hu"
#define SCNx16  "hx"

#define SCNd32  "d"
#define SCNi32  "i"
#define SCNo32  "o"
#define SCNu32  "u"
#define SCNx32  "x"

#define SCNd64  "ld"
#define SCNi64  "li"
#define SCNo64  "lo"
#define SCNu64  "lu"
#define SCNx64  "lx"

#define SCNdMAX "ld"
#define SCNiMAX "li"
#define SCNoMAX "lo"
#define SCNuMAX "lu"
#define SCNxMAX "lx"

#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNoPTR "lo"
#define SCNuPTR "lu"
#define SCNxPTR "lx"

/* ---- intmax_t helpers (libc/src/compat.c) ---- */

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t  strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);
intmax_t  imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);

#endif /* AURALITE_LIBC_INTTYPES_H */
