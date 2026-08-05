#ifndef AURALITE_KERNEL_RNG_H
#define AURALITE_KERNEL_RNG_H

#include <stdint.h>
#include <stddef.h>

/* Kernel CSPRNG (POSIX2024 phase Q16).
 *
 * Graduates the syscall layer's one-off rdtsc-xorshift filler into a seeded
 * xorshift128+ pool.  This is a solid PRNG, NOT a cryptographic RNG: the
 * seed is drawn from rdtsc / timer ticks / pointer addresses / RDRAND (when
 * the CPU provides it), and the state is mixed from those sources.  The
 * security note in TODO.md states the limits honestly rather than
 * overclaiming.  getrandom(2) and getentropy(2) both draw from this pool.
 */

/* Initialise the pool.  Called once at boot from kmain; idempotent. */
void rng_init(void);

/* Fill `len` bytes of `out` from the pool. */
void rng_fill(void *out, size_t len);

/* Return one 64-bit word from the pool. */
uint64_t rng_u64(void);

#endif /* AURALITE_KERNEL_RNG_H */
