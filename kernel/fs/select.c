/* kernel/fs/select.c — select / poll (P10) */

#include "kernel/fs/vfs.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/usercopy.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/time.h"
#include <stdint.h>

#define FD_SETSIZE 64

typedef struct {
    uint64_t fds_bits[FD_SETSIZE / 64];
} fd_set;

static inline void FD_ZERO(fd_set *set) { memset(set, 0, sizeof(fd_set)); }
static inline void FD_SET(int fd, fd_set *set) {
    if (fd >= 0 && fd < FD_SETSIZE) set->fds_bits[fd / 64] |= (1ULL << (fd % 64));
}
static inline int FD_ISSET(int fd, fd_set *set) {
    return (fd >= 0 && fd < FD_SETSIZE) && (set->fds_bits[fd / 64] & (1ULL << (fd % 64)));
}

int do_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct kernel_timeval *timeout) {
    (void)exceptfds;

    tcb_t *cur = sched_current();
    if (!cur || nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;

    int ready = 0;
    fd_set r, w;
    if (readfds)  copy_from_user(&r, readfds, sizeof(fd_set));
    if (writefds) copy_from_user(&w, writefds, sizeof(fd_set));

    for (int fd = 0; fd < nfds; fd++) {
        struct ofd *o = cur->fd_table[fd];
        if (!o) continue;

        int can_read  = (readfds  && FD_ISSET(fd, &r))  && (o->pos < o->vn->size || o->nonblock);
        int can_write = (writefds && FD_ISSET(fd, &w)) && (o->access_mode != O_RDONLY);

        if (can_read || can_write) ready++;
    }

    if (ready == 0 && timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0)
        return 0;   /* timeout immediately */

    /* Простая реализация: если нет готовых — yield (реальная блокировка через wait_queue в P10 follow-up) */
    if (ready == 0) {
        sched_yield();
    }

    if (readfds)  copy_to_user(readfds, &r, sizeof(fd_set));
    if (writefds) copy_to_user(writefds, &w, sizeof(fd_set));

    return ready;
}