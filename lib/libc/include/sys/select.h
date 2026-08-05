#ifndef AURALITE_LIBC_SYS_SELECT_H
#define AURALITE_LIBC_SYS_SELECT_H

#include "../sys/types.h"
#include "../time.h"
#include "../signal.h"   /* Q16: sigset_t for pselect */

#define FD_SETSIZE 64

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(s)     __builtin_memset((s), 0, sizeof(fd_set))
#define FD_SET(fd, s)  ((s)->fds_bits[(fd) / 64] |=  (1UL << ((fd) % 64)))
#define FD_CLR(fd, s)  ((s)->fds_bits[(fd) / 64] &= ~(1UL << ((fd) % 64)))
#define FD_ISSET(fd, s)((s)->fds_bits[(fd) / 64] &   (1UL << ((fd) % 64)))

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

/* Q16: pselect — select with a relative timespec and a signal mask that
 * is installed atomically with the block (no lost-wakeup race). */
int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);

#endif /* AURALITE_LIBC_SYS_SELECT_H */
