#ifndef AURALITE_KERNEL_RNG_H
#define AURALITE_KERNEL_RNG_H

#include <stdint.h>
#include <stddef.h>

/* Kernel CSPRNG (INTERNET_PLAN.md phase N0).
 *
 * The generator is a ChaCha20-based DRBG (kernel/rng_core.h, RFC 8439).
 * What makes N0 different from the Q16 xorshift128+ pool it replaces is the
 * ENTROPY, not the mixing:
 *
 *   - RDSEED (CPUID.7:EBX bit 18) or RDRAND (CPUID.1:ECX bit 30) seed the
 *     DRBG directly when the CPU provides them;
 *   - otherwise a jitter pool accumulates interrupt-arrival timing deltas
 *     (kernel/arch/x86_64/irq.c calls rng_jitter_event() on every IRQ), and
 *     the DRBG is seeded only once the pool's measured variation reaches
 *     RNG_POOL_BITS estimated bits;
 *   - until then the generator reports NOT READY: rng_try_fill() returns
 *     -ENOSYS and getentropy()/getrandom() surface that to userspace
 *     (D1: a loud failure beats a quiet fake).
 *
 * This is hobby-OS entropy suitable for seeding TLS handshake keys
 * (INTERNET_PLAN N3+); it is not audited.  The estimated entropy is logged
 * at boot so a weak source is visible rather than silently accepted.
 */

/* Estimated jitter-pool entropy required before seeding without hardware
 * RNG.  Public because the boot log names the threshold. */
#define RNG_POOL_BITS 128

/* Initialise the module: detect RDRAND/RDSEED, seed when possible.
 * Called once from kmain after the timer; idempotent.  When no hardware
 * RNG exists the module stays UNREADY and the jitter pool finishes the
 * seeding later (see rng_jitter_event / rng_available). */
void rng_init(void);

/* 1 when the generator is ready to serve bytes RIGHT NOW.  As a side
 * effect it completes seeding from the jitter pool the first time the
 * pool's estimate crosses RNG_POOL_BITS, so callers may simply poll. */
int rng_available(void);

/* Fill `len` bytes of `out`.  Returns 0 on success, -ENOSYS when the
 * generator is not ready (no hardware RNG and the jitter pool has not
 * reached RNG_POOL_BITS yet).  Never partially fills on failure. */
int rng_try_fill(void *out, size_t len);

/* Legacy convenience wrappers kept for API stability.  rng_fill() fills
 * when ready and zeroes the buffer otherwise (callers that must tell the
 * difference use rng_try_fill); rng_u64() returns 0 when not ready. */
void rng_fill(void *out, size_t len);
uint64_t rng_u64(void);

/* Interrupt-timing jitter source.  Called from irq_dispatch() for every
 * hardware IRQ on every CPU.  Must be callable before rng_init() — events
 * are simply accumulated until the module is up. */
void rng_jitter_event(uint64_t tsc_now);

#endif /* AURALITE_KERNEL_RNG_H */
