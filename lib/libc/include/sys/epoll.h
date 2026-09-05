/* sys/epoll.h — the epoll triple, on top of select() (RESIDUE2 T4).
 *
 * AuraLite's readiness story is select(): poll() rides it (POSIX_PLAN §P9)
 * and now epoll does too — level-triggered, exactly as TODO.md always said
 * it would ("epoll on top of select()").  No new syscalls: an "epoll
 * instance" is a userspace interest set, and epoll_wait() is one select()
 * over the registered descriptors.
 *
 * What that design buys (and costs), stated rather than hidden:
 *   - O(n) per wait over the INTEREST set, not over all fds — fine here,
 *     the set is capped at EPOLL_MAX_INTEREST and the guests are small;
 *   - level-triggered semantics only (no EPOLLET — edge state would need
 *     the kernel's event queue, which is precisely what this box defers);
 *   - EPOLLONESHOT is supported in the usual way (mask cleared after a
 *     report, re-armed by EPOLL_CTL_MOD);
 *   - EPOLLERR/EPOLLHUP report when select() flags an exception, plus the
 *     classic cases the host kernel infers (read end of a pipe whose
 *     writers are gone is readable-and-HUP).
 *
 * The event mask names Linux's ABI values (they are the de-facto standard
 * every program that uses epoll is compiled against).
 */

#ifndef AURALITE_LIBC_SYS_EPOLL_H
#define AURALITE_LIBC_SYS_EPOLL_H

#include <stdint.h>

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;    /* EPOLLIN | ... */
    epoll_data_t data;
} __attribute__((packed));

#define EPOLLIN       0x001u
#define EPOLLPRI      0x002u
#define EPOLLOUT      0x004u
#define EPOLLRDNORM   0x040u
#define EPOLLWRNORM   0x100u
#define EPOLLRDBAND   0x080u
#define EPOLLWRBAND   0x200u
#define EPOLLMSG      0x400u
#define EPOLLERR      0x008u
#define EPOLLHUP      0x010u
#define EPOLLRDHUP    0x2000u
#define EPOLLONESHOT  (1u << 30)
#define EPOLLET       (1u << 31)   /* accepted, ignored: level-triggered only */

/* epoll_create1 flags */
#define EPOLL_CLOEXEC  0x80000

/* op codes */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/*
 * Create an epoll instance.  The classic epoll_create(size) argument is
 * ignored on every modern kernel too; epoll_create1(flags) honours
 * EPOLL_CLOEXEC.  Both return a non-negative instance id or -1 + errno
 * (EINVAL on unknown flags, ENOSPC when the instance table is full).
 */
int epoll_create(int size);
int epoll_create1(int flags);

/*
 * Register (ADD), re-arm/re-mask (MOD) or remove (DEL) @fd on @ep.
 * @event may be NULL for DEL.  Returns 0 or -1 + errno
 * (EBADF/ENOENT/EEXIST per the man page's contract).
 */
int epoll_ctl(int ep, int op, int fd, struct epoll_event *event);

/*
 * Wait for readiness: up to @maxevents reports into @events (must be > 0).
 * @timeout: -1 blocks, 0 polls, > 0 waits that many milliseconds.
 * Returns the number of reports (0 on timeout), or -1 + errno (EINTR
 * passes through from select()).
 */
int epoll_wait(int ep, struct epoll_event *events, int maxevents, int timeout);

#endif /* AURALITE_LIBC_SYS_EPOLL_H */
