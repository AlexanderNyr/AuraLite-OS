/* libc/src/epoll.c — the epoll triple on top of select() (RESIDUE2 T4).
 *
 * See sys/epoll.h for the design contract.  Implementation notes:
 *
 *  - an "epoll instance" is an index into a static table, OFFSET above the
 *    real fd space (EPOLL_FD_BASE).  It is deliberately NOT a kernel fd:
 *    the tree's fd table belongs to the VFS, and minting real descriptors
 *    for userspace bookkeeping would need a devfs node per instance —
 *    machinery this box explicitly defers ("no new syscalls beyond the
 *    epoll triple", and the triple stays userspace).  The cost is stated:
 *    close(ep) hits the kernel as EBADF (harmless — the slot frees at
 *    process exit or EPOLL_CTL teardown of its last interest), and an
 *    epoll id must not be dup'd.
 *  - one epoll_wait() == one select() over the interest set, level-triggered.
 *  - EPOLLONESHOT disarms the mask after a report; EPOLL_CTL_MOD re-arms.
 *    EPOLLET is accepted and ignored (edge state needs a kernel queue).
 */

#include "sys/epoll.h"
#include "sys/select.h"
#include "errno.h"
#include "string.h"
#include "time.h"       /* usleep: the empty-interest-set sleep */

#define EP_MAX_INSTANCES 8
#define EP_MAX_FDS       64        /* == FD_SETSIZE: the interest ceiling */

#define EPOLL_FD_BASE    0x10000   /* above every real fd the VFS mints */

struct ep_interest {
    int          registered;
    uint32_t     mask;             /* events asked for, sans EPOLLET */
    epoll_data_t data;
};

struct ep_instance {
    int in_use;
    struct ep_interest fds[EP_MAX_FDS];
};

static struct ep_instance g_ep[EP_MAX_INSTANCES];

static struct ep_instance *ep_valid(int ep) {
    int idx = ep - EPOLL_FD_BASE;
    if (idx < 0 || idx >= EP_MAX_INSTANCES || !g_ep[idx].in_use) return 0;
    return &g_ep[idx];
}

int epoll_create(int size) {
    /* size is a historical no-op on every modern kernel too. */
    (void)size;
    return epoll_create1(0);
}

int epoll_create1(int flags) {
    if (flags & ~(uint32_t)EPOLL_CLOEXEC) { errno = EINVAL; return -1; }
    for (int i = 0; i < EP_MAX_INSTANCES; i++) {
        if (!g_ep[i].in_use) {
            memset(&g_ep[i], 0, sizeof(g_ep[i]));
            g_ep[i].in_use = 1;
            return EPOLL_FD_BASE + i;
        }
    }
    errno = ENOSPC;               /* instance table exhausted */
    return -1;
}

int epoll_ctl(int ep, int op, int fd, struct epoll_event *event) {
    struct ep_instance *e = ep_valid(ep);
    if (!e) { errno = EBADF; return -1; }
    if (fd < 0 || fd >= EP_MAX_FDS) { errno = EBADF; return -1; }

    struct ep_interest *slot = &e->fds[fd];
    switch (op) {
    case EPOLL_CTL_ADD:
        if (slot->registered) { errno = EEXIST; return -1; }
        if (!event) { errno = EINVAL; return -1; }
        slot->registered = 1;
        slot->mask  = event->events & ~(uint32_t)EPOLLET;
        slot->data  = event->data;
        return 0;
    case EPOLL_CTL_MOD:
        if (!slot->registered) { errno = ENOENT; return -1; }
        if (!event) { errno = EINVAL; return -1; }
        slot->mask = event->events & ~(uint32_t)EPOLLET;
        slot->data = event->data;
        return 0;
    case EPOLL_CTL_DEL:
        if (!slot->registered) { errno = ENOENT; return -1; }
        memset(slot, 0, sizeof(*slot));
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int epoll_wait(int ep, struct epoll_event *events, int maxevents, int timeout) {
    struct ep_instance *e = ep_valid(ep);
    if (!e) { errno = EBADF; return -1; }
    if (!events || maxevents <= 0) { errno = EINVAL; return -1; }

    fd_set rfds, wfds, xfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&xfds);
    int maxfd = -1;

    for (int fd = 0; fd < EP_MAX_FDS; fd++) {
        struct ep_interest *s = &e->fds[fd];
        if (!s->registered || !s->mask) continue;
        if (s->mask & (EPOLLIN | EPOLLRDNORM | EPOLLPRI))  FD_SET(fd, &rfds);
        if (s->mask & (EPOLLOUT | EPOLLWRNORM))            FD_SET(fd, &wfds);
        /* Exception reporting is always armed: ERR/HUP must wake the
         * waiter even if the caller only asked for IN/OUT. */
        FD_SET(fd, &xfds);
        if (fd > maxfd) maxfd = fd;
    }
    if (maxfd < 0) {
        /* Empty interest set: POSIX blocks; there is nothing to select on,
         * so sleep the timeout out honestly (select with NULL sets is
         * unspecified on some stacks, and a poll loop would spin). */
        if (timeout < 0) {
            usleep(86400u * 1000000u);      /* "forever": one day */
            errno = EINTR;                  /* only a signal can cut this */
            return -1;
        }
        usleep((unsigned long)timeout * 1000u);
        return 0;
    }

    struct timeval tv, *ptv = 0;
    if (timeout >= 0) {
        tv.tv_sec  = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }

    int r = select(maxfd + 1, &rfds, &wfds, &xfds, ptv);
    if (r < 0) return -1;        /* EINTR passes through with errno set */

    int n = 0;
    for (int fd = 0; fd < EP_MAX_FDS && n < maxevents; fd++) {
        struct ep_interest *s = &e->fds[fd];
        if (!s->registered || !s->mask) continue;

        uint32_t got = 0;
        if (FD_ISSET(fd, &rfds)) got |= EPOLLIN;
        if (FD_ISSET(fd, &wfds)) got |= EPOLLOUT;
        if (FD_ISSET(fd, &xfds)) got |= EPOLLERR | EPOLLHUP;
        if (!got) continue;

        events[n].events = got;
        events[n].data   = s->data;
        n++;

        if (s->mask & EPOLLONESHOT) s->mask = 0;   /* disarmed until MOD */
    }
    return n;
}
