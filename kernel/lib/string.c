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
 * These portable bodies stay for the host unit tests and for the DTB
 * tenants (rv64 adopted this file in HW H0; a64 in A5a [AMEND-2]).
 *
 * HW_PLAN H1: word-wide.  H0 measured the old byte loops at 249 (rv64)
 * / 197 (a64) MB/s under TCG and the disassembly showed clang had NOT
 * widened them (lbu/sb and ldrb/strb, one byte per iteration -- H0's
 * first reading blamed auto-unrolling; the objdump corrected it, the
 * plan records the correction).  These bodies move 8 bytes per
 * iteration instead.
 *
 * Two portability facts drive the shape:
 *
 *   - a64 compiles with -mstrict-align, so the compiler may only emit
 *     wide loads/stores it can PROVE aligned.  A `sw_word *` cast is
 *     that proof (C's own rule: a validly-derefenced T* is aligned for
 *     T) -- which is why the fast paths earn 8-byte alignment with a
 *     byte head first, and why the mixed-alignment middle uses
 *     __builtin_memcpy loads (correct everywhere; the compiler lowers
 *     them per target's alignment rules).
 *
 *   - Plain `uint64_t *` casts into byte buffers would be an aliasing
 *     violation the optimiser is entitled to punish; `sw_word` carries
 *     __attribute__((may_alias)) so the word accesses are exempt, the
 *     same contract __builtin_memcpy provides -- but with the
 *     alignment assertion strict-align needs. */
#ifndef ARCH_X86_64

typedef uint64_t __attribute__((may_alias)) sw_word;

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;

    if (n >= 16) {
        while ((uintptr_t)d & 7) { *d++ = v; n--; }
        sw_word w = v;
        w |= w << 8; w |= w << 16; w |= w << 32;
        sw_word *dw = (sw_word *)d;
        while (n >= 8) { *dw++ = w; n -= 8; }
        d = (unsigned char *)dw;
    }
    while (n--) {
        *d++ = v;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (n >= 16) {
        if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
            /* Co-aligned: one byte head buys BOTH sides 8-byte
             * alignment; the loop is one aligned load + store per 8
             * bytes on every target, strict-align included. */
            while ((uintptr_t)d & 7) { *d++ = *s++; n--; }
            sw_word             *dw = (sw_word *)d;
            const sw_word       *sw = (const sw_word *)s;
            while (n >= 8) { *dw++ = *sw++; n -= 8; }
            d = (unsigned char *)dw;
            s = (const unsigned char *)sw;
        } else {
            /* Mixed alignment: aligned stores, __builtin_memcpy
             * loads (correct at any alignment; rv64 lowers to one
             * ld, strict-align a64 to byte loads -- still one wide
             * STORE per 8, and mixed alignment is the rare case). */
            while ((uintptr_t)d & 7) { *d++ = *s++; n--; }
            while (n >= 8) {
                uint64_t w;
                __builtin_memcpy(&w, s, 8);
                *(sw_word *)d = w;
                d += 8; s += 8; n -= 8;
            }
        }
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        return memcpy(dst, src, n);   /* forward copy is overlap-safe */
    }

    /* Backward.  Word path only when co-aligned; the tail-first byte
     * head buys alignment at the TOP end this time. */
    d += n;
    s += n;
    if (n >= 16 && (((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while ((uintptr_t)d & 7) { *--d = *--s; n--; }
        sw_word       *dw = (sw_word *)d;
        const sw_word *sw = (const sw_word *)s;
        while (n >= 8) { *--dw = *--sw; n -= 8; }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--) {
        *--d = *--s;
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
