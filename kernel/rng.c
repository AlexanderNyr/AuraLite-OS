/* kernel/rng.c — seeded xorshift128+ pool (POSIX2024 phase Q16).
 *
 * The boot-time syscalls used a per-call rdtsc-xorshift filler with no
 * persistent state.  This module keeps a 128-bit xorshift128+ state that is
 * seeded from every entropy-ish source the kernel already has:
 *
 *   - RDRAND, when CPUID.1:ECX.RDRAND is set (QEMU exposes it with
 *     -cpu qemu64? no — qemu64 lacks RDRAND, so this is a genuine
 *     "when available" path, exercised on real hardware and newer vCPUs);
 *   - RDTSC and the PIT tick counter;
 *   - the addresses of kernel objects (ASLR-ish layout noise);
 *   - the current stack pointer.
 *
 * The mixing function is SplitMix64, which turns the loosely-correlated
 * seed words into a well-distributed 128-bit state.  xorshift128+ is a
 * fast, well-understood PRNG (Vigna 2017); it is NOT a cryptographic
 * generator.  That is stated in TODO.md, not overclaimed: getrandom(2)
 * must not be advertised as /dev/urandom-equivalent.
 *
 * rng_init() is called from kmain before any syscall can run.  The pool is
 * global and read from multiple CPUs, but a torn read of a 64-bit word is
 * harmless for this purpose; no lock is taken (documented).
 */

#include "kernel/rng.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/kprintf.h"
#include "drivers/timer/pit.h"

static uint64_t rng_state[2];       /* xorshift128+ state */
static volatile int rng_ready = 0;

/* ---- seed sources ---- */

static uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int cpu_has_rdrand(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpuid_count(1, 0, &a, &b, &c, &d);
    return (c & (1u << 30)) != 0;    /* ECX.RDRAND */
}

static int rdrand64(uint64_t *out) {
    unsigned char ok = 0;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    return ok ? 1 : 0;
}

/* SplitMix64: one step of a 64-bit mixing generator (for seeding). */
static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* ---- xorshift128+ (Vigna 2017) ---- */

static uint64_t rng_next(void) {
    uint64_t x = rng_state[0];
    uint64_t const y = rng_state[1];
    rng_state[0] = y;
    x ^= x << 23;
    rng_state[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    return rng_state[1] + y;
}

void rng_init(void) {
    if (rng_ready) return;

    uint64_t seed = 0;
    uint64_t s = rdtsc64() ^ (uint64_t)(uintptr_t)&rng_state ^ 0xA5A5A5A5A5A5A5A5ULL;
    seed ^= splitmix64(&s);
    seed ^= (uint64_t)timer_get_ticks() << 32 ^ timer_get_ticks();
    seed ^= (uint64_t)(uintptr_t)rng_fill;
    seed ^= (uint64_t)(uintptr_t)rng_next;

    uint64_t r = 0;
    if (cpu_has_rdrand() && rdrand64(&r)) {
        seed ^= r;
        kprintf("[rng] RDRAND available, folded into seed\n");
    } else {
        kprintf("[rng] no RDRAND; seeding from jitter sources only\n");
    }

    /* Expand the 64-bit seed into two well-mixed 64-bit words. */
    uint64_t sm = seed;
    rng_state[0] = splitmix64(&sm);
    rng_state[1] = splitmix64(&sm);

    /* Warm-up: discard the first outputs (weak low bits after seeding). */
    for (int i = 0; i < 16; i++) (void)rng_next();

    __asm__ volatile ("" ::: "memory");
    rng_ready = 1;
    kprintf("[rng] seeded xorshift128+ pool ready\n");
}

uint64_t rng_u64(void) {
    if (!rng_ready) rng_init();
    return rng_next();
}

void rng_fill(void *out, size_t len) {
    if (!out) return;
    if (!rng_ready) rng_init();
    uint8_t *p = (uint8_t *)out;
    while (len >= 8) {
        uint64_t w = rng_next();
        for (int b = 0; b < 8; b++) p[b] = (uint8_t)(w >> (b * 8));
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t w = rng_next();
        for (size_t b = 0; b < len; b++) p[b] = (uint8_t)(w >> (b * 8));
    }
}
