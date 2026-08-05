#ifndef AURALITE_LIBC_SYS_RANDOM_H
#define AURALITE_LIBC_SYS_RANDOM_H

/* <sys/random.h> — POSIX.1-2024 / Linux getrandom(2) (phase Q16). */

#include <sys/types.h>

#define GRND_NONBLOCK 0x0001   /* don't block (the pool is always ready) */
#define GRND_RANDOM   0x0002   /* legacy hint; accepted, same source */

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);

#endif /* AURALITE_LIBC_SYS_RANDOM_H */
