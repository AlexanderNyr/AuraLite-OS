#ifndef AURALITE_LIBC_SYS_UIO_H
#define AURALITE_LIBC_SYS_UIO_H

/*
 * sys/uio.h — scatter-gather I/O (POSIX.1-2017).
 *
 * The iovec layout (void *base; size_t len) must match the kernel's
 * `struct iovec` in kernel/fs/vfs.h.
 */

#include <stddef.h>
#include <sys/types.h>   /* ssize_t */

#define IOV_MAX 1024

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#endif /* AURALITE_LIBC_SYS_UIO_H */
