#ifndef AURALITE_LIBC_POLL_H
#define AURALITE_LIBC_POLL_H

#include <signal.h>   /* Q16: sigset_t for ppoll; self-contained header */
#include <time.h>     /* Q16: struct timespec for ppoll */

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

typedef unsigned long nfds_t;

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* Q16: ppoll — poll with a relative timespec and an atomically-installed
 * signal mask (like pselect; NULL sigmask behaves like poll with a
 * timespec timeout). */
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
          const sigset_t *sigmask);

#endif /* AURALITE_LIBC_POLL_H */
