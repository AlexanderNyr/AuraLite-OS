/* string.c — freestanding memory/string routines for the kernel.
 *
 * Compiled with -mno-sse, so these are plain scalar loops (no vectorisation).
 * They are also referenced by the compiler for implicit struct copies / large
 * initialisers, so the names and signatures must match the standard library.
 *
 * OPT_PLAN.md O1: memset/memcpy/memmove are the portable FALLBACK now —
 * on the x86_64 kernel build they are shadowed by the `rep movsb`/`rep
 * stosb` backend in kernel/arch/x86_64/string_fast.c (which see, for why
 * the assembly lives in the arch tree: the V6 asm ratchet holds this file
 * at zero inline assembly, and the host unit tests compile exactly these
 * portable bodies).  memcmp/strlen below went word-wide in the same phase
 * — in portable C, so every consumer of this file gets them.
 */

#include <stdint.h>
#include "kernel/lib/string.h"

/* The x86_64 kernel links the rep-string backend instead (string_fast.c).
 * These portable bodies stay for the host unit tests and for any future
 * arch that adopts the shared tree (D5 residue: rv64/a64 want exactly
 * these shapes, 8-byte loops, once their kernels move off their private
 * copies). */
#ifndef ARCH_X86_64

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    while (n--) {
        *d++ = v;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

#endif /* !ARCH_X86_64 */

/* Word-wide memcmp (OPT_PLAN O1): 8-byte chunks through __builtin_memcpy
 * loads (defined behaviour at any alignment; clang lowers each to one
 * mov), byte-scan the first differing word.  No `rep cmpsb` — it is
 * micro-coded byte-at-a-time everywhere and would also be assembly in a
 * ratcheted file. */
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n >= 8) {
        uint64_t wa, wb;
        __builtin_memcpy(&wa, pa, 8);
        __builtin_memcpy(&wb, pb, 8);
        if (wa != wb) {
            for (int i = 0; i < 8; i++) {
                if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
            }
        }
        pa += 8;
        pb += 8;
        n  -= 8;
    }
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

/* Word-wide strlen (OPT_PLAN O1): byte-step to 8-alignment, then scan
 * aligned words with the has-zero-byte trick.  Aligned 8-byte reads
 * cannot cross a page boundary, so this never touches a byte the string
 * itself could not. */
size_t strlen(const char *s) {
    const char *p = s;
    while (((uintptr_t)p & 7) != 0) {
        if (*p == '\0') return (size_t)(p - s);
        p++;
    }
    for (;;) {
        uint64_t w;
        __builtin_memcpy(&w, p, 8);
        uint64_t zero = (w - 0x0101010101010101ULL) & ~w &
                        0x8080808080808080ULL;
        if (zero != 0) {
            /* Little-endian: the lowest set 0x80 marks the first NUL. */
            return (size_t)(p - s) + ((size_t)__builtin_ctzll(zero) >> 3);
        }
        p += 8;
    }
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n > 0 && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {
        /* copy until NUL */
    }
    return dst;
}
