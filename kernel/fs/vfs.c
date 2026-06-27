/* vfs.c — virtual file system: mount table, path resolution, FD management.
 *
 * Path resolution: find the longest matching mount point, then call the FS's
 * lookup() with the remaining relative path.  For example, "/dev/null" matches
 * the "/dev" mount and calls devfs_lookup(fs_data, "null").
 */

#include <stdint.h>
#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"

/*
 * vfs_wrap_err() — normalise a filesystem op's return value to a negative
 * errno.  Underlying FS drivers (tmpfs, devfs, ...) still report failure with a
 * bare -1; until each grows specific errno returns (TODO.md), the VFS layer
 * substitutes a caller-supplied @fallback errno for that generic -1.  A return
 * that is already a specific negative errno (in the reserved band) is passed
 * through unchanged, and any non-negative result is returned as-is.
 *
 * @ret      filesystem op return value (>= 0 success, < 0 failure)
 * @fallback positive errno to use when @ret is the generic -1
 * Returns @ret on success, or a negative errno on failure.
 */
static int64_t vfs_wrap_err(int64_t ret, int fallback) {
    if (ret >= 0) return ret;
    if (ret == -1) return -(int64_t)fallback;
    if (errno_is_err((long)ret)) return ret;   /* already a specific -errno */
    return -(int64_t)fallback;
}

/* ---- Anonymous pipe backing -------------------------------------------- */
#define PIPE_BUF_SIZE 4096

struct pipe_ring {
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t head;          /* next write index */
    uint32_t tail;          /* next read  index */
    uint32_t used;
    int     readers;        /* number of FDs holding the read end open      */
    int     writers;        /* number of FDs holding the write end open     */
};

static int64_t pipe_read_op(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    (void)pos;
    struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
    if (!p) return -1;
    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;
    while (got == 0) {
        if (p->used == 0) {
            if (p->writers == 0) return 0;     /* EOF: nobody to write more */
            sched_yield();
            continue;
        }
        while (got < count && p->used > 0) {
            out[got++] = p->buf[p->tail];
            p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
            p->used--;
        }
    }
    return (int64_t)got;
}

static int64_t pipe_write_op(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    (void)pos;
    struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
    if (!p) return -1;
    if (p->readers == 0) return -1;            /* broken pipe */
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t put = 0;
    while (put < count) {
        if (p->used == PIPE_BUF_SIZE) {
            if (p->readers == 0) return -1;
            sched_yield();
            continue;
        }
        while (put < count && p->used < PIPE_BUF_SIZE) {
            p->buf[p->head] = in[put++];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
            p->used++;
        }
    }
    return (int64_t)put;
}

static const struct vfs_ops pipe_read_ops  = { .read = pipe_read_op };
static const struct vfs_ops pipe_write_ops = { .write = pipe_write_op };

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
/* Fallback table for early boot or unusual calls before sched_init().  Normal
 * threads/processes use tcb_t::fd_table, so fd numbers are process-local. */
static struct file      fallback_fd_table[VFS_MAX_FDS];

static struct file *current_fd_table(void) {
    tcb_t *cur = sched_current();
    if (cur) return cur->fd_table;
    return fallback_fd_table;
}

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
    memset(fallback_fd_table, 0, sizeof(fallback_fd_table));
}

int vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            strncpy(mounts[i].mount_path, path, VFS_PATH_MAX - 1);
            mounts[i].ops      = ops;
            mounts[i].fs_data  = fs_data;
            mounts[i].in_use   = 1;
            kprintf("[vfs] mounted '%s'\n", path);
            return 0;
        }
    }
    return -ENOSPC;   /* mount table full */
}

/* Find the longest matching mount for a path. */
static int find_mount(const char *path, const char **out_rel) {
    if (path[0] != '/') return -1;
    int best_mount = -1;
    size_t best_len = 0;
    size_t path_len = strlen(path);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        size_t mlen = strlen(mounts[i].mount_path);
        if (mlen > path_len) continue;
        if (memcmp(mounts[i].mount_path, path, mlen) == 0) {
            /* Avoid matching /tmpfile to /tmp. Root is a special prefix. */
            if (mlen > 1 && path[mlen] != '\0' && path[mlen] != '/') continue;
            if (mlen > best_len) {
                best_len = mlen;
                best_mount = i;
            }
        }
    }
    if (best_mount < 0) return -1;
    const char *rel = path + best_len;
    if (*rel == '/') rel++;
    if (out_rel) *out_rel = rel;
    return best_mount;
}

/* Resolve a path to a vnode (no creation). */
static struct vnode *resolve_path(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return NULL;
    return mounts[m].ops->lookup(mounts[m].fs_data, rel);
}

/*
 * vfs_open() — POSIX.1-2017 open(2).
 *
 * @flags carries the access mode (O_ACCMODE field) plus creation/status flags.
 * @mode is the permission bits for a newly created file (consulted only with
 * O_CREAT; AuraLite has no umask yet — applied verbatim, masked to 07777).
 *
 * Order of operations (POSIX.1-2017 open()):
 *   1. validate the access-mode field;
 *   2. resolve the final component;
 *   3. absent + !O_CREAT -> ENOENT;  absent + O_CREAT -> create;
 *   4. present + (O_CREAT|O_EXCL) -> EEXIST;
 *   5. directory opened for writing -> EISDIR; O_DIRECTORY on non-dir -> ENOTDIR;
 *   6. O_TRUNC last, only for a regular file opened writable.
 * On any failure no file is created or modified.
 */
int vfs_open(const char *path, int flags, int mode) {
    int acc = flags & O_ACCMODE;
    if (acc == 3) return -EINVAL;              /* reserved/invalid access mode */

    int writable = (acc == O_WRONLY || acc == O_RDWR);

    struct vnode *vn = resolve_path(path);
    int created = 0;

    if (vn == NULL) {
        if (!(flags & O_CREAT)) return -ENOENT;
        const char *rel = NULL;
        int m = find_mount(path, &rel);
        if (m < 0) return -ENOENT;
        if (!mounts[m].ops->create) return -EROFS;   /* fs cannot create */
        vn = mounts[m].ops->create(mounts[m].fs_data, rel);
        if (vn == NULL) return -EACCES;
        if (mode != 0) vn->mode = (uint32_t)(mode & 07777);
        created = 1;
    } else {
        /* File exists. */
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    }

    /* Type checks against the requested access. */
    if (vn->type == VFS_TYPE_DIR && writable) return -EISDIR;
    if ((flags & O_DIRECTORY) && vn->type != VFS_TYPE_DIR) return -ENOTDIR;

    /* O_TRUNC: only meaningful for a regular file opened writable.  POSIX
     * leaves O_TRUNC without write access undefined — we ignore it there. */
    if ((flags & O_TRUNC) && writable && vn->type == VFS_TYPE_FILE && !created) {
        if (vn->ops->truncate) {
            int64_t tr = vn->ops->truncate(vn, 0);
            if (tr < 0) return (int)vfs_wrap_err(tr, EIO);
        }
    }

    struct file *fd_table = current_fd_table();
    /* Reserve fd 0/1/2 for stdin/stdout/stderr syscall semantics. */
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            fd_table[i].vn          = vn;
            fd_table[i].pos         = (flags & O_APPEND) ? vn->size : 0;
            fd_table[i].in_use      = 1;
            fd_table[i].access_mode = acc;
            fd_table[i].append      = (flags & O_APPEND) ? 1 : 0;
            fd_table[i].nonblock    = (flags & O_NONBLOCK) ? 1 : 0;
            tcb_t *cur = sched_current();
            if (cur) cur->cloexec[i] = (flags & O_CLOEXEC) ? 1 : 0;
            return i;
        }
    }
    return -EMFILE;   /* per-process FD table is full */
}

int64_t vfs_read(int fd, void *buf, uint64_t count) {
    struct file *fd_table = current_fd_table();
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    struct file *f = &fd_table[fd];
    /* A fd opened O_WRONLY is not readable (POSIX: read on such fd -> EBADF). */
    if (f->access_mode == O_WRONLY) return -EBADF;
    if (!f->vn->ops->read) return -EINVAL;   /* object not readable */
    /* O_NONBLOCK: a read that would block returns -EAGAIN instead.  For a pipe
     * with no buffered data and writers still attached, that is a would-block. */
    if (f->nonblock && f->vn->ops == &pipe_read_ops && count > 0) {
        struct pipe_ring *p = (struct pipe_ring *)f->vn->fs_data;
        if (p && p->used == 0 && p->writers > 0) return -EAGAIN;
    }
    int64_t n = f->vn->ops->read(f->vn, f->pos, buf, count);
    if (n > 0) f->pos += (uint64_t)n;
    return vfs_wrap_err(n, EIO);
}

int64_t vfs_write(int fd, const void *buf, uint64_t count) {
    struct file *fd_table = current_fd_table();
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    struct file *f = &fd_table[fd];
    /* A fd opened O_RDONLY is not writable (POSIX: write on such fd -> EBADF). */
    if (f->access_mode == O_RDONLY) return -EBADF;
    if (!f->vn->ops->write) return -EINVAL;   /* object not writable */
    /* O_NONBLOCK: a write that would block returns -EAGAIN instead.  For a pipe
     * whose ring is full with readers still attached, that is a would-block. */
    if (f->nonblock && f->vn->ops == &pipe_write_ops && count > 0) {
        struct pipe_ring *p = (struct pipe_ring *)f->vn->fs_data;
        if (p && p->used == PIPE_BUF_SIZE && p->readers > 0) return -EAGAIN;
    }
    /* O_APPEND: reposition to EOF before each write.  The single-threaded
     * VFS makes the seek-to-EOF + write atomic here; once SMP/preemptive FS
     * access lands this needs a per-vnode write lock (TODO.md). */
    if (f->append) f->pos = f->vn->size;
    int64_t n = f->vn->ops->write(f->vn, f->pos, buf, count);
    if (n > 0) f->pos += (uint64_t)n;
    return vfs_wrap_err(n, EIO);
}

int64_t vfs_lseek(int fd, int64_t offset, int whence) {
    struct file *fd_table = current_fd_table();
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    struct file *f = &fd_table[fd];
    int64_t new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;
        case 1: new_pos = (int64_t)f->pos + offset; break;
        case 2: new_pos = (int64_t)f->vn->size + offset; break;
        default: return -EINVAL;
    }
    if (new_pos < 0) return -EINVAL;
    f->pos = (uint64_t)new_pos;
    return new_pos;
}

int vfs_close(int fd) {
    struct file *fd_table = current_fd_table();
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -EBADF;
    struct vnode *vn = fd_table[fd].vn;
    fd_table[fd].in_use = 0;
    fd_table[fd].vn     = NULL;
    fd_table[fd].pos    = 0;
    /* Per-process cloexec flag is also cleared. */
    tcb_t *cur = sched_current();
    if (cur) cur->cloexec[fd] = 0;

    /* Pipe lifetime: when the last reader/writer FD closes, we can free the
     * pipe ring + vnode.  We detect that by checking the ops pointer. */
    if (vn) {
        struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
        if (p && (vn->ops == &pipe_read_ops || vn->ops == &pipe_write_ops)) {
            if (vn->ops == &pipe_read_ops)  p->readers--;
            if (vn->ops == &pipe_write_ops) p->writers--;
            if (p->readers <= 0 && p->writers <= 0) {
                kfree(p);
                kfree(vn);
            }
        }
    }
    return 0;
}

/* ---- dup / dup2 / pipe / cloexec ---- */

static int alloc_fd_slot(struct file *table, int starting_from) {
    for (int i = starting_from; i < VFS_MAX_FDS; i++) {
        if (!table[i].in_use) return i;
    }
    return -1;
}

int vfs_dup(int oldfd) {
    struct file *t = current_fd_table();
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !t[oldfd].in_use) return -EBADF;
    int nfd = alloc_fd_slot(t, 3);
    if (nfd < 0) return -EMFILE;
    t[nfd] = t[oldfd];
    /* Cloexec is per-FD: dup() clears it on the new FD. */
    tcb_t *cur = sched_current();
    if (cur) cur->cloexec[nfd] = 0;

    /* If we duped a pipe end, bump the refcount so close() handles it. */
    struct vnode *vn = t[nfd].vn;
    if (vn) {
        struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
        if (p) {
            if (vn->ops == &pipe_read_ops)  p->readers++;
            if (vn->ops == &pipe_write_ops) p->writers++;
        }
    }
    return nfd;
}

int vfs_dup2(int oldfd, int newfd) {
    struct file *t = current_fd_table();
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !t[oldfd].in_use) return -EBADF;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    if (newfd < 3) return -EBADF;              /* protect stdin/out/err */
    if (t[newfd].in_use) vfs_close(newfd);
    t[newfd] = t[oldfd];
    tcb_t *cur = sched_current();
    if (cur) cur->cloexec[newfd] = 0;
    struct vnode *vn = t[newfd].vn;
    if (vn) {
        struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
        if (p) {
            if (vn->ops == &pipe_read_ops)  p->readers++;
            if (vn->ops == &pipe_write_ops) p->writers++;
        }
    }
    return newfd;
}

int vfs_pipe2(int out_fds[2], int flags) {
    if (!out_fds) return -EFAULT;
    /* pipe2 only accepts O_CLOEXEC | O_NONBLOCK. */
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    struct file *t = current_fd_table();
    int rfd = alloc_fd_slot(t, 3);
    if (rfd < 0) return -EMFILE;
    t[rfd].in_use = 1;                          /* reserve before second alloc */
    int wfd = alloc_fd_slot(t, 3);
    if (wfd < 0) { t[rfd].in_use = 0; return -EMFILE; }

    struct pipe_ring *p = kmalloc(sizeof(*p));
    if (!p) { t[rfd].in_use = 0; return -ENOMEM; }
    memset(p, 0, sizeof(*p));
    p->readers = 1; p->writers = 1;

    struct vnode *rvn = kmalloc(sizeof(*rvn));
    struct vnode *wvn = kmalloc(sizeof(*wvn));
    if (!rvn || !wvn) {
        if (rvn) kfree(rvn);
        if (wvn) kfree(wvn);
        kfree(p);
        t[rfd].in_use = 0;
        return -ENOMEM;
    }
    memset(rvn, 0, sizeof(*rvn));
    memset(wvn, 0, sizeof(*wvn));
    strncpy(rvn->name, "pipe-r", VFS_PATH_MAX - 1);
    strncpy(wvn->name, "pipe-w", VFS_PATH_MAX - 1);
    rvn->type = VFS_TYPE_CHARDEV; rvn->ops = &pipe_read_ops;  rvn->fs_data = p;
    wvn->type = VFS_TYPE_CHARDEV; wvn->ops = &pipe_write_ops; wvn->fs_data = p;

    int nb = (flags & O_NONBLOCK) ? 1 : 0;
    t[rfd].vn = rvn; t[rfd].pos = 0; t[rfd].in_use = 1;
    t[rfd].access_mode = O_RDONLY; t[rfd].append = 0; t[rfd].nonblock = nb;
    t[wfd].vn = wvn; t[wfd].pos = 0; t[wfd].in_use = 1;
    t[wfd].access_mode = O_WRONLY; t[wfd].append = 0; t[wfd].nonblock = nb;

    tcb_t *cur = sched_current();
    if (cur) {
        int ce = (flags & O_CLOEXEC) ? 1 : 0;
        cur->cloexec[rfd] = ce;
        cur->cloexec[wfd] = ce;
    }
    out_fds[0] = rfd;
    out_fds[1] = wfd;
    return 0;
}

int vfs_pipe(int out_fds[2]) {
    return vfs_pipe2(out_fds, 0);
}

int vfs_set_cloexec(int fd, int on) {
    tcb_t *cur = sched_current();
    if (!cur) return -EINVAL;
    if (fd < 0 || fd >= VFS_MAX_FDS) return -EBADF;
    cur->cloexec[fd] = on ? 1 : 0;
    return 0;
}

int vfs_get_cloexec(int fd) {
    tcb_t *cur = sched_current();
    if (!cur) return 0;
    if (fd < 0 || fd >= VFS_MAX_FDS) return 0;
    return cur->cloexec[fd];
}

/*
 * dup_lowest_from() — duplicate @oldfd into the lowest free slot >= @minfd
 * (F_DUPFD semantics).  @cloexec selects the new fd's FD_CLOEXEC bit.
 * Returns the new fd, or a negative errno (EBADF / EINVAL / EMFILE).
 */
static int dup_lowest_from(int oldfd, int minfd, int cloexec) {
    struct file *t = current_fd_table();
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !t[oldfd].in_use) return -EBADF;
    if (minfd < 0 || minfd >= VFS_MAX_FDS) return -EINVAL;   /* OPEN_MAX bound */
    int start = (minfd < 3) ? 3 : minfd;   /* never hand out 0/1/2 */
    int nfd = alloc_fd_slot(t, start);
    if (nfd < 0) return -EMFILE;            /* no slot >= minfd available */
    t[nfd] = t[oldfd];
    tcb_t *cur = sched_current();
    if (cur) cur->cloexec[nfd] = cloexec ? 1 : 0;
    /* Bump pipe refcounts if we duped a pipe end. */
    struct vnode *vn = t[nfd].vn;
    if (vn) {
        struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
        if (p) {
            if (vn->ops == &pipe_read_ops)  p->readers++;
            if (vn->ops == &pipe_write_ops) p->writers++;
        }
    }
    return nfd;
}

/*
 * vfs_fcntl() — POSIX fcntl(2) subset.  Returns a non-negative result or a
 * negative errno.  Keeps the FD_CLOEXEC namespace (F_GETFD/F_SETFD) strictly
 * separate from the file-status-flags namespace (F_GETFL/F_SETFL).
 */
int vfs_fcntl(int fd, int cmd, int arg) {
    struct file *t = current_fd_table();
    /* Commands whose @fd must be a valid open descriptor. */
    if (cmd != F_DUPFD && cmd != F_DUPFD_CLOEXEC) {
        if (fd < 0 || fd >= VFS_MAX_FDS || !t[fd].in_use) return -EBADF;
    }
    struct file *f = &t[fd];

    switch (cmd) {
    case F_DUPFD:
        return dup_lowest_from(fd, arg, 0);
    case F_DUPFD_CLOEXEC:
        return dup_lowest_from(fd, arg, 1);
    case F_GETFD:
        return vfs_get_cloexec(fd) ? FD_CLOEXEC : 0;
    case F_SETFD:
        return vfs_set_cloexec(fd, (arg & FD_CLOEXEC) ? 1 : 0);
    case F_GETFL: {
        /* access mode | live status flags (NOT FD_CLOEXEC, NOT creation flags) */
        int r = f->access_mode;
        if (f->append)   r |= O_APPEND;
        if (f->nonblock) r |= O_NONBLOCK;
        return r;
    }
    case F_SETFL:
        /* Only O_APPEND / O_NONBLOCK are changeable; access mode and creation
         * flags in @arg are silently ignored (POSIX requirement). */
        f->append   = (arg & O_APPEND)   ? 1 : 0;
        f->nonblock = (arg & O_NONBLOCK) ? 1 : 0;
        return 0;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        return -ENOSYS;   /* POSIX advisory record locking — P10 territory */
    default:
        return -EINVAL;   /* unknown command */
    }
}

void vfs_close_on_exec(void) {
    tcb_t *cur = sched_current();
    if (!cur) return;
    for (int fd = 3; fd < VFS_MAX_FDS; fd++) {
        if (cur->cloexec[fd] && cur->fd_table[fd].in_use) {
            vfs_close(fd);
        }
    }
}

/* ---- Path operations ---- */

int vfs_mkdir(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -ENOENT;
    if (!mounts[m].ops->mkdir) return -ENOSYS;
    return (int)vfs_wrap_err(mounts[m].ops->mkdir(mounts[m].fs_data, rel), EACCES);
}

int vfs_rmdir(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -ENOENT;
    if (!mounts[m].ops->rmdir) return -ENOSYS;
    return (int)vfs_wrap_err(mounts[m].ops->rmdir(mounts[m].fs_data, rel), ENOENT);
}

int vfs_unlink(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -ENOENT;
    if (!mounts[m].ops->unlink) return -ENOSYS;
    return (int)vfs_wrap_err(mounts[m].ops->unlink(mounts[m].fs_data, rel), ENOENT);
}

int vfs_rename(const char *from, const char *to) {
    const char *rel_from = NULL, *rel_to = NULL;
    int m_from = find_mount(from, &rel_from);
    int m_to   = find_mount(to,   &rel_to);
    if (m_from < 0 || m_to < 0) return -ENOENT;
    if (m_from != m_to) return -EXDEV;          /* cross-device link */
    if (!mounts[m_from].ops->rename) return -ENOSYS;
    return (int)vfs_wrap_err(
        mounts[m_from].ops->rename(mounts[m_from].fs_data, rel_from, rel_to),
        ENOENT);
}

int vfs_truncate(const char *path, uint64_t new_size) {
    struct vnode *vn = resolve_path(path);
    if (!vn) return -ENOENT;
    if (!vn->ops->truncate) return -EINVAL;
    return (int)vfs_wrap_err(vn->ops->truncate(vn, new_size), EIO);
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!out) return -EFAULT;
    struct vnode *vn = resolve_path(path);
    if (!vn) return -ENOENT;
    if (vn->ops->stat) return (int)vfs_wrap_err(vn->ops->stat(vn, out), EIO);
    /* Default stat: pull from vnode fields. */
    memset(out, 0, sizeof(*out));
    out->type  = vn->type;
    out->mode  = vn->mode ? vn->mode : (vn->type == VFS_TYPE_DIR ? 0755 : 0644);
    out->size  = vn->size;
    out->inode = vn->inode_id;
    out->nlink = 1;
    return 0;
}

int vfs_readdir(const char *path, struct vfs_dirent *out, int max) {
    if (!out) return -EFAULT;
    if (max <= 0) return -EINVAL;
    struct vnode *vn = resolve_path(path);
    if (!vn) return -ENOENT;
    if (vn->type != VFS_TYPE_DIR) return -ENOTDIR;
    if (!vn->ops->readdir) return -ENOSYS;
    return (int)vfs_wrap_err(vn->ops->readdir(vn, out, max), EIO);
}

void vfs_list(const char *path) {
    /* Try the generic readdir path first.  If the underlying fs supports it,
     * we get a uniform listing.  Otherwise fall back to fs-specific shims. */
    struct vfs_dirent *ents = kmalloc(64 * sizeof(struct vfs_dirent));
    if (!ents) return;
    int n = vfs_readdir(path, ents, 64);
    if (n >= 0) {
        for (int i = 0; i < n; i++) {
            const char *suffix = (ents[i].type == VFS_TYPE_DIR) ? "/" : "";
            if (ents[i].type == VFS_TYPE_DIR) {
                kprintf("  %s%s\n", ents[i].name, suffix);
            } else {
                kprintf("  %s  (%llu bytes)\n",
                        ents[i].name, (unsigned long long)ents[i].size);
            }
        }
        kfree(ents);
        return;
    }
    kfree(ents);

    /* Legacy per-fs print helpers (for filesystems without readdir). */
    extern void initrd_list(void);
    extern void tmpfs_list(void);
    extern void diskfs_list(void);
    extern void fat32_list(void);
    if (strcmp(path, "/") == 0) {
        initrd_list();
    } else if (strcmp(path, "/tmp") == 0 || strcmp(path, "/tmp/") == 0) {
        tmpfs_list();
    } else if (strcmp(path, "/disk") == 0 || strcmp(path, "/disk/") == 0) {
        diskfs_list();
    } else if (strcmp(path, "/fat") == 0 || strcmp(path, "/fat/") == 0) {
        fat32_list();
    }
}

void vfs_self_test(void) {
    kprintf("[vfs] self-test: exercising /dev and initrd...\n");

    int fd = vfs_open("/dev/null", O_RDWR, 0);
    if (fd < 0) { kprintf("[vfs] FAIL: cannot open /dev/null\n"); return; }
    const char *msg = "hello /dev/null";
    int64_t n = vfs_write(fd, msg, 15);
    if (n != 15) {
        kprintf("[vfs] FAIL: write to /dev/null returned %lld\n", (long long)n);
        vfs_close(fd); return;
    }
    vfs_close(fd);
    kprintf("[vfs]   /dev/null: write OK (15 bytes discarded)\n");

    fd = vfs_open("/dev/zero", O_RDONLY, 0);
    if (fd < 0) { kprintf("[vfs] FAIL: cannot open /dev/zero\n"); return; }
    uint8_t zbuf[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    n = vfs_read(fd, zbuf, 4);
    if (n != 4 || zbuf[0] || zbuf[1] || zbuf[2] || zbuf[3]) {
        kprintf("[vfs] FAIL: /dev/zero read = %lld, buf=%02x%02x%02x%02x\n",
                (long long)n, zbuf[0], zbuf[1], zbuf[2], zbuf[3]);
        vfs_close(fd); return;
    }
    vfs_close(fd);
    kprintf("[vfs]   /dev/zero: read OK (4 zero bytes)\n");

    fd = vfs_open("/tmp/vfs.txt", O_CREAT | O_RDWR, 0644);
    if (fd < 0) { kprintf("[vfs] FAIL: cannot create /tmp/vfs.txt\n"); return; }
    const char *tmsg = "vfs writable path";
    n = vfs_write(fd, tmsg, strlen(tmsg));
    if (n != (int64_t)strlen(tmsg)) {
        kprintf("[vfs] FAIL: tmpfs write returned %lld\n", (long long)n);
        vfs_close(fd); return;
    }
    vfs_close(fd);
    fd = vfs_open("/tmp/vfs.txt", O_RDONLY, 0);
    char tbuf[32] = {0};
    n = vfs_read(fd, tbuf, sizeof(tbuf) - 1);
    vfs_close(fd);
    if (n != (int64_t)strlen(tmsg) || strcmp(tbuf, tmsg) != 0) {
        kprintf("[vfs] FAIL: tmpfs readback mismatch '%s'\n", tbuf); return;
    }
    kprintf("[vfs]   /tmp/vfs.txt: create/write/read OK\n");

    fd = vfs_open("/init", O_RDONLY, 0);
    if (fd >= 0) {
        char fbuf[32] = { 0 };
        n = vfs_read(fd, fbuf, sizeof(fbuf) - 1);
        kprintf("[vfs]   /init: opened, read %lld byte(s)\n", (long long)n);
        vfs_close(fd);
    } else {
        kprintf("[vfs]   /init: not found (initrd may be empty)\n");
    }

    kprintf("[vfs] PASS: VFS layer functional\n");
}
