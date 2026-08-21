/* string_fast.c — x86_64 kernel memcpy/memset/memmove (OPT_PLAN.md O1).
 *
 * The byte-at-a-time loops these replace lived in kernel/lib/string.c and
 * were the plan's Fact 1: every frame flip, COW copy, spawn read and
 * implicit struct copy paid one loop iteration per byte.  The replacement
 * is `rep movsb`/`rep stosb` — no SSE, no FPU state, no alignment
 * requirements, and the fastest general copy available to `-mno-sse`
 * kernel code on everything QEMU emulates (and, via ERMSB, on real
 * hardware since Ivy Bridge).
 *
 * This file lives under kernel/arch/x86_64/ ON PURPOSE: the V6 asm
 * ratchet (tools/check_width_sweep.py, ratchet 4) holds portable
 * kernel/driver code at zero new inline assembly, and it is right to —
 * kernel/lib/string.c keeps a portable fallback for every function here
 * (compiled when ARCH_X86_64 is not defined: the host unit tests and any
 * future arch that adopts the shared tree), and this file is the x86_64
 * backend that shadows them at kernel link time.
 *
 * Two measured/recorded decisions:
 *
 *   - The bulk is `rep movsq`/`rep stosq` (8 bytes per iteration) with a
 *     `rep movsb`/`rep stosb` tail, NOT plain `rep movsb` for everything.
 *     Membench measured why (OPT_PLAN D1, §6): QEMU/TCG — this project's
 *     primary target — emulates rep-string ops one ITERATION at a time,
 *     so `rep movsb` runs at byte-loop speed there (~11 MB/s, no win at
 *     all), while the same rep with 8-byte elements moves 8× per
 *     iteration (the 8-byte-loop memmove jumped 144 → 1242 MB/s in the
 *     same boot).  On real hardware both spellings hit the fast-string
 *     path (ERMSB extends to movsq), so the movsq form is never the
 *     wrong choice — it just refuses to be a TCG no-op.
 *
 *   - SMALL_N = 64: below the `rep` setup cost the scalar loop wins.
 *     Under TCG the difference at <64 B is noise either way, and on real
 *     ERMSB hardware Intel's own guidance puts the break-even in the
 *     32–128 B band.  64 is the middle of that band and a cache line.
 *
 *   - The backward memmove case uses a plain word-wise C loop, NOT
 *     `std; rep movsb; cld`.  Not for DF safety — this kernel's entry
 *     paths already clear the flag (isr_stubs.asm:73 `cld` with the ABI
 *     comment, syscall_entry.asm:131) — but because a backward
 *     `rep movsb` gets NO fast-string path on any microarchitecture
 *     (ERMSB is forward-only; backward decays to one byte per µop),
 *     while the 8-byte loop moves eight.  Never setting DF also keeps
 *     the interaction surface at zero for whatever entry path is added
 *     next, which is worth having for free.  Backward overlapping
 *     copies are rare (tmpfs truncate shuffles, GUI scroll regions);
 *     8× the old byte loop is enough for them.
 */
#include <stdint.h>
#include <stddef.h>
#include "kernel/lib/string.h"
#include "kernel/arch/x86_64/string_fast.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/lib/kprintf.h"

/* HW_PLAN H2: the small-copy crossover is RUNTIME now.  The compile-
 * time SMALL_N = 64 was tuned for the wrong machine on ERMSB parts:
 * enhanced rep movsb/stosb has no setup-cost cliff (Intel SDM vol.1
 * 7.3.9.3 -- "fast-string operation ... optimized to provide high
 * performance even for small counts"), so the scalar pre-loop is pure
 * overhead there.  string_fast_init() reads CPUID.7.0:EBX.9 once at
 * boot and drops the threshold to 0 on ERMS parts; qemu64 TCG has no
 * ERMS (measured, H0's receipt) and keeps the measured-good 64.  The
 * threshold line is printed so every lane's smoke can pin which world
 * it booted in -- and so the metal receipt (plan §6) has its line. */
static size_t small_n = 64;     /* the O1-measured default; see above */

void string_fast_init(void)
{
    uint32_t ebx7;

    cpuid_count(7, 0, 0, &ebx7, 0, 0);
    if ((ebx7 >> 9) & 1) {
        small_n = 0;
        kprintf("[cpu]   memcpy small-copy crossover: 0 (ERMS fast-string)\n");
    } else {
        kprintf("[cpu]   memcpy small-copy crossover: 64 (no ERMS)\n");
    }
}

void *memcpy(void *dst, const void *src, size_t n) {
    if (n < small_n) {
        unsigned char       *d = (unsigned char *)dst;
        const unsigned char *s = (const unsigned char *)src;
        while (n--) *d++ = *s++;
        return dst;
    }
    void  *d = dst;
    size_t q = n >> 3;
    size_t r = n & 7;
    /* rdi/rsi advance across both reps; unaligned movsq is fine on x86. */
    __asm__ volatile ("rep movsq"
                      : "+D"(d), "+S"(src), "+c"(q)
                      :
                      : "memory");
    __asm__ volatile ("rep movsb"
                      : "+D"(d), "+S"(src), "+c"(r)
                      :
                      : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    if (n < small_n) {
        unsigned char *d = (unsigned char *)dst;
        unsigned char  v = (unsigned char)c;
        while (n--) *d++ = v;
        return dst;
    }
    void    *d = dst;
    size_t   q = n >> 3;
    size_t   r = n & 7;
    uint64_t v = (unsigned char)c;
    v *= 0x0101010101010101ULL;      /* byte replicated to all 8 lanes */
    /* rax holds the replicated word; its low byte is c, so the stosb
     * tail reuses it as-is. */
    __asm__ volatile ("rep stosq"
                      : "+D"(d), "+c"(q)
                      : "a"(v)
                      : "memory");
    __asm__ volatile ("rep stosb"
                      : "+D"(d), "+c"(r)
                      : "a"(v)
                      : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) return dst;

    if (d < s) {
        /* Forward copy: rep movsb is defined byte-sequential, so it is
         * overlap-safe in this direction — same routine as memcpy. */
        return memcpy(dst, src, n);
    }

    /* Backward copy without touching DF (see the header comment).
     * 8-byte tail-first chunks, then the byte remainder. */
    while (n >= 8) {
        n -= 8;
        uint64_t w;
        __builtin_memcpy(&w, s + n, 8);
        __builtin_memcpy(d + n, &w, 8);
    }
    while (n--) d[n] = s[n];
    return dst;
}
