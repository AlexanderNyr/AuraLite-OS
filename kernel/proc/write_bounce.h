#ifndef AURALITE_PROC_WRITE_BOUNCE_H
#define AURALITE_PROC_WRITE_BOUNCE_H

#include <stdint.h>
#include "kernel/lib/errno.h"
#include "kernel/fs/vfs.h"
#include "kernel/proc/usercopy.h"
#include "kernel/mm/kheap.h"

/* SYS_WRITE VFS path only. Console, reads, pwrite and writev are unchanged.
 * Compile with 256 for the control run; no global/per-CPU scratch and no
 * enlarged kernel stack. This does not add FD-table close/reuse locking. */
#ifndef SYSCALL_FILE_WRITE_CHUNK
#define SYSCALL_FILE_WRITE_CHUNK 4096
#endif
#if SYSCALL_FILE_WRITE_CHUNK < 256 || SYSCALL_FILE_WRITE_CHUNK > 16384
#error "SYSCALL_FILE_WRITE_CHUNK must be in 256..16384"
#endif

static int64_t syscall_vfs_write(int fd, const void *user_buf, uint64_t len) {
    if (!len) return 0;
    if (len > INT64_MAX) return -EINVAL;
    if (!validate_user_range(user_buf, len, 0)) return -EFAULT;

    char small[256];
    char *buffer = small;
    uint64_t capacity = sizeof(small);
    int regular_file = 0;
    if (len > capacity) {
        struct vnode *vn = vfs_get_vnode(fd);
        regular_file = vn && vn->type == VFS_TYPE_FILE && !vfs_fd_is_devfs(fd);
        /* Pipes/FIFOs and devices keep the old bounded transfer policy. */
        if (regular_file && SYSCALL_FILE_WRITE_CHUNK > sizeof(small)) {
            uint64_t wanted = len < SYSCALL_FILE_WRITE_CHUNK ? len : SYSCALL_FILE_WRITE_CHUNK;
            char *allocated = kmalloc(wanted);
            if (allocated) { buffer = allocated; capacity = wanted; }
            /* OOM is not a failed write: retry with the old stack buffer. */
        }
    }

    uint64_t done = 0;
    int64_t result;
    while (done < len) {
        uint64_t n = len - done;
        if (n > capacity) n = capacity;
        /* VFS sees only a fully copied kernel buffer, never user memory. */
        if (copy_from_user(buffer, (const uint8_t *)user_buf + done, n) != 0) {
            /* Correct regular-file progress reporting only; retain the
             * historical pipe/device fault return in this scoped change. */
            result = regular_file && done ? (int64_t)done : -EFAULT;
            goto out;
        }
        int64_t written = vfs_write(fd, buffer, n);
        if (written < 0) {
            result = done ? (int64_t)done : written;
            goto out;
        }
        uint64_t progress = written; /* negative values handled above */
        if (progress > n) { /* do not trust an impossible FS count */
            result = done ? (int64_t)done : -EIO;
            goto out;
        }
        done += progress;
        if (progress < n) break; /* includes zero: no busy loop */
    }
    result = (int64_t)done;
out:
    if (buffer != small) kfree(buffer);
    return result;
}
#endif
