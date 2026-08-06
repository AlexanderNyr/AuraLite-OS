#ifndef AURALITE_LIBC_SYS_RANDOM_H
#define AURALITE_LIBC_SYS_RANDOM_H

/* <sys/random.h> — POSIX.1-2024 / Linux getrandom(2) (phase Q16).
 * Since INTERNET_PLAN N0 the kernel source is a ChaCha20 CSPRNG seeded from
 * RDSEED/RDRAND or interrupt jitter; before it is seeded getrandom() blocks
 * (or returns -1/EAGAIN with GRND_NONBLOCK). */

#include <sys/types.h>

#define GRND_NONBLOCK 0x0001   /* don't block: EAGAIN while unseeded */
#define GRND_RANDOM   0x0002   /* legacy hint; accepted, same source */

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);

#endif /* AURALITE_LIBC_SYS_RANDOM_H */
