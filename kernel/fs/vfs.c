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
            /* Interrupted by a signal with no data buffered -> -EINTR. */
            if (signal_interrupted()) return -EINTR;
            __asm__ volatile ("sti" ::: "memory");
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

/* Writing to a pipe whose read ends are all closed raises SIGPIPE on the
 * writer and fails with EPIPE (POSIX write()). */
static int64_t pipe_broken(void) {
    tcb_t *cur = sched_current();
    if (cur) signal_send(cur, SIGPIPE);
    return -EPIPE;
}

static int64_t pipe_write_op(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    (void)pos;
    struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
    if (!p) return -EIO;
    if (p->readers == 0) return pipe_broken();
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t put = 0;
    while (put < count) {
        if (p->used == PIPE_BUF_SIZE) {
            if (p->readers == 0) return put ? (int64_t)put : pipe_broken();
            /* Interrupted by a signal before completing -> partial / -EINTR. */
            if (signal_interrupted()) return put ? (int64_t)put : -EINTR;
            __asm__ volatile ("sti" ::: "memory");
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
static struct ofd *fallback_fd_table[VFS_MAX_FDS];
static uint8_t     fallback_cloexec[VFS_MAX_FDS];

static struct ofd **current_fd_table(void) {
    tcb_t *cur = sched_current();
    if (cur) return cur->fd_table;
    return fallback_fd_table;
}

/* Returns the cloexec[] array that pairs with current_fd_table(). */
static uint8_t *current_cloexec(void) {
    tcb_t *cur = sched_current();
    if (cur) return cur->cloexec;
    return fallback_cloexec;
}

/* ---- OFD lifecycle ---------------------------------------------------- */

/* Forward decls of the pipe ops so close/free can detect pipe ends. */
/* (pipe_read_ops / pipe_write_ops are defined above.) */

/* Release the vnode/pipe backing an OFD whose refcount has reached 0. */
static void ofd_release_backing(struct vnode *vn) {
    if (!vn) return;
    struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
    if (p && (vn->ops == &pipe_read_ops || vn->ops == &pipe_write_ops)) {
        /* Pipe reader/writer counts track live OFDs, not fds: decrement here,
         * on final OFD release, exactly once per OFD. */
        if (vn->ops == &pipe_read_ops)  p->readers--;
        if (vn->ops == &pipe_write_ops) p->writers--;
        if (p->readers <= 0 && p->writers <= 0) {
            kfree(p);
            kfree(vn);
        }
    }
    /* Non-pipe vnodes are owned by their filesystem; nothing to free here. */
}

/* Allocate a new OFD referring to @vn with refcount 1. */
static struct ofd *ofd_alloc(struct vnode *vn, int acc, int append, int nonblock) {
    struct ofd *o = kmalloc(sizeof(*o));
    if (!o) return NULL;
    o->vn          = vn;
    o->pos         = append ? vn->size : 0;
    o->access_mode = acc;
    o->append      = append;
    o->nonblock    = nonblock;
    o->refcount    = 1;
    return o;
}

/* Drop one reference; free the OFD (and release its backing) at 0. */
static void ofd_put(struct ofd *o) {
    if (!o) return;
    if (--o->refcount <= 0) {
        ofd_release_backing(o->vn);
        kfree(o);
    }
}

/* Public wrapper used by the process layer (exit path). */
void vfs_close_ofd(struct ofd *o, struct vnode *unused) {
    (void)unused;
    ofd_put(o);
}

/* Find the lowest free fd slot >= @start.  Returns -1 if none. */
static int alloc_fd_slot_ptr(struct ofd **t, int start) {
    for (int i = start; i < VFS_MAX_FDS; i++) {
        if (t[i] == NULL) return i;
    }
    return -1;
}

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
    memset(fallback_fd_table, 0, sizeof(fallback_fd_table));
    memset(fallback_cloexec, 0, sizeof(fallback_cloexec));
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

    struct ofd **fd_table = current_fd_table();
    /* Reserve fd 0/1/2 for stdin/stdout/stderr syscall semantics. */
    int slot = alloc_fd_slot_ptr(fd_table, 3);
    if (slot < 0) return -EMFILE;             /* per-process FD table is full */

    struct ofd *o = ofd_alloc(vn, acc, (flags & O_APPEND) ? 1 : 0,
                              (flags & O_NONBLOCK) ? 1 : 0);
    if (!o) return -ENOMEM;
    fd_table[slot] = o;
    current_cloexec()[slot] = (flags & O_CLOEXEC) ? 1 : 0;
    return slot;
}

/* Resolve a fd to its OFD, or NULL if the fd is not open. */
static struct ofd *fd_to_ofd(int fd) {
    struct ofd **t = current_fd_table();
    if (fd < 0 || fd >= VFS_MAX_FDS) return NULL;
    return t[fd];
}

/* True if @vn is non-seekable (pipe/FIFO/socket/chardev) -> lseek gives ESPIPE. */
static int vnode_is_pipe_like(const struct vnode *vn) {
    return vn && (vn->ops == &pipe_read_ops || vn->ops == &pipe_write_ops ||
                  vn->type == VFS_TYPE_CHARDEV);
}

int64_t vfs_read(int fd, void *buf, uint64_t count) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    /* A fd opened O_WRONLY is not readable (POSIX: read on such fd -> EBADF). */
    if (o->access_mode == O_WRONLY) return -EBADF;
    if (!o->vn->ops->read) return -EINVAL;   /* object not readable */
    /* O_NONBLOCK: a read that would block returns -EAGAIN instead.  For a pipe
     * with no buffered data and writers still attached, that is a would-block. */
    if (o->nonblock && o->vn->ops == &pipe_read_ops && count > 0) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        if (p && p->used == 0 && p->writers > 0) return -EAGAIN;
    }
    int64_t n = o->vn->ops->read(o->vn, o->pos, buf, count);
    if (n > 0) o->pos += (uint64_t)n;       /* shared OFD offset advances */
    return vfs_wrap_err(n, EIO);
}

int64_t vfs_write(int fd, const void *buf, uint64_t count) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    /* A fd opened O_RDONLY is not writable (POSIX: write on such fd -> EBADF). */
    if (o->access_mode == O_RDONLY) return -EBADF;
    if (!o->vn->ops->write) return -EINVAL;   /* object not writable */
    /* O_NONBLOCK: a write that would block returns -EAGAIN instead. */
    if (o->nonblock && o->vn->ops == &pipe_write_ops && count > 0) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        if (p && p->used == PIPE_BUF_SIZE && p->readers > 0) return -EAGAIN;
    }
    /* O_APPEND: reposition to EOF before each write.  The single-threaded
     * VFS makes the seek-to-EOF + write atomic here; once SMP/preemptive FS
     * access lands this needs a per-vnode write lock (TODO.md). */
    if (o->append) o->pos = o->vn->size;
    int64_t n = o->vn->ops->write(o->vn, o->pos, buf, count);
    if (n > 0) o->pos += (uint64_t)n;
    return vfs_wrap_err(n, EIO);
}

int64_t vfs_lseek(int fd, int64_t offset, int whence) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    if (vnode_is_pipe_like(o->vn)) return -ESPIPE;   /* non-seekable */
    int64_t new_pos;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = (int64_t)o->pos + offset; break;
        case SEEK_END: new_pos = (int64_t)o->vn->size + offset; break;
        default: return -EINVAL;
    }
    if (new_pos < 0) return -EINVAL;
    /* Seeking past EOF is allowed and does not extend the file. */
    o->pos = (uint64_t)new_pos;
    return new_pos;
}

int64_t vfs_pread(int fd, void *buf, uint64_t count, int64_t offset) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    if (o->access_mode == O_WRONLY) return -EBADF;
    if (vnode_is_pipe_like(o->vn)) return -ESPIPE;
    if (offset < 0) return -EINVAL;
    if (!o->vn->ops->read) return -EINVAL;
    /* Positioned read: do NOT touch the shared OFD offset. */
    int64_t n = o->vn->ops->read(o->vn, (uint64_t)offset, buf, count);
    return vfs_wrap_err(n, EIO);
}

int64_t vfs_pwrite(int fd, const void *buf, uint64_t count, int64_t offset) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    if (o->access_mode == O_RDONLY) return -EBADF;
    if (vnode_is_pipe_like(o->vn)) return -ESPIPE;
    if (offset < 0) return -EINVAL;
    if (!o->vn->ops->write) return -EINVAL;
    /* POSIX pwrite ignores O_APPEND and writes at @offset, leaving pos alone. */
    int64_t n = o->vn->ops->write(o->vn, (uint64_t)offset, buf, count);
    return vfs_wrap_err(n, EIO);
}

/* IOV_MAX for AuraLite: small, matches the kernel iovec staging cap. */
#define VFS_IOV_MAX 1024

int64_t vfs_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt <= 0 || iovcnt > VFS_IOV_MAX) return -EINVAL;
    /* Sum lengths with overflow check against SSIZE_MAX before any transfer. */
    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint64_t l = iov[i].iov_len;
        if (total + l < total || total + l > 0x7FFFFFFFFFFFFFFFULL) return -EINVAL;
        total += l;
    }
    int64_t done = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t n = vfs_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return (done > 0) ? done : n;      /* report error or partial */
        done += n;
        if ((uint64_t)n < iov[i].iov_len) break;       /* short read: stop */
    }
    return done;
}

int64_t vfs_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt <= 0 || iovcnt > VFS_IOV_MAX) return -EINVAL;
    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint64_t l = iov[i].iov_len;
        if (total + l < total || total + l > 0x7FFFFFFFFFFFFFFFULL) return -EINVAL;
        total += l;
    }
    int64_t done = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t n = vfs_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return (done > 0) ? done : n;
        done += n;
        if ((uint64_t)n < iov[i].iov_len) break;       /* short write: stop */
    }
    return done;
}

int vfs_close(int fd) {
    struct ofd **fd_table = current_fd_table();
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd] == NULL) return -EBADF;
    struct ofd *o = fd_table[fd];
    /* Clear the slot first so the fd number is immediately reusable, then drop
     * the OFD reference.  ofd_put() frees the OFD (and releases the pipe/vnode,
     * decrementing pipe reader/writer counts) only when refcount hits 0. */
    fd_table[fd] = NULL;
    current_cloexec()[fd] = 0;
    ofd_put(o);
    return 0;
}

/* ---- dup / dup2 / pipe / cloexec ---- */

int vfs_dup(int oldfd) {
    struct ofd **t = current_fd_table();
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || t[oldfd] == NULL) return -EBADF;
    int nfd = alloc_fd_slot_ptr(t, 3);
    if (nfd < 0) return -EMFILE;
    /* Share the same OFD (shared offset/flags); bump its refcount.  No pipe
     * reader/writer change: those track OFDs, and this is the same OFD. */
    t[oldfd]->refcount++;
    t[nfd] = t[oldfd];
    current_cloexec()[nfd] = 0;   /* FD_CLOEXEC is per-fd; dup() clears it */
    return nfd;
}

int vfs_dup2(int oldfd, int newfd) {
    struct ofd **t = current_fd_table();
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || t[oldfd] == NULL) return -EBADF;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -EBADF;
    /* dup2(fd, fd) with a valid fd: no close, no refcount change, returns fd. */
    if (oldfd == newfd) return newfd;
    if (newfd < 3) return -EBADF;              /* protect stdin/out/err */
    /* Bump the source OFD ref BEFORE dropping the old target (handles the case
     * where both already alias the same OFD). */
    t[oldfd]->refcount++;
    struct ofd *old = t[newfd];
    t[newfd] = t[oldfd];
    current_cloexec()[newfd] = 0;
    if (old) ofd_put(old);        /* close the old target after install */
    return newfd;
}

int vfs_pipe2(int out_fds[2], int flags) {
    if (!out_fds) return -EFAULT;
    /* pipe2 only accepts O_CLOEXEC | O_NONBLOCK. */
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    struct ofd **t = current_fd_table();
    int rfd = alloc_fd_slot_ptr(t, 3);
    if (rfd < 0) return -EMFILE;
    /* Reserve the read slot with a sentinel so the second alloc skips it. */
    t[rfd] = (struct ofd *)(uintptr_t)1;
    int wfd = alloc_fd_slot_ptr(t, 3);
    if (wfd < 0) { t[rfd] = NULL; return -EMFILE; }

    struct pipe_ring *p = kmalloc(sizeof(*p));
    if (!p) { t[rfd] = NULL; return -ENOMEM; }
    memset(p, 0, sizeof(*p));
    p->readers = 1; p->writers = 1;   /* one read-end OFD, one write-end OFD */

    struct vnode *rvn = kmalloc(sizeof(*rvn));
    struct vnode *wvn = kmalloc(sizeof(*wvn));
    int nb = (flags & O_NONBLOCK) ? 1 : 0;
    struct ofd *ro = NULL, *wo = NULL;
    if (rvn && wvn) {
        memset(rvn, 0, sizeof(*rvn));
        memset(wvn, 0, sizeof(*wvn));
        strncpy(rvn->name, "pipe-r", VFS_PATH_MAX - 1);
        strncpy(wvn->name, "pipe-w", VFS_PATH_MAX - 1);
        rvn->type = VFS_TYPE_CHARDEV; rvn->ops = &pipe_read_ops;  rvn->fs_data = p;
        wvn->type = VFS_TYPE_CHARDEV; wvn->ops = &pipe_write_ops; wvn->fs_data = p;
        ro = ofd_alloc(rvn, O_RDONLY, 0, nb);
        wo = ofd_alloc(wvn, O_WRONLY, 0, nb);
    }
    if (!rvn || !wvn || !ro || !wo) {
        if (ro) kfree(ro);
        if (wo) kfree(wo);
        if (rvn) kfree(rvn);
        if (wvn) kfree(wvn);
        kfree(p);
        t[rfd] = NULL;
        return -ENOMEM;
    }

    t[rfd] = ro;
    t[wfd] = wo;
    uint8_t *ce = current_cloexec();
    int cflag = (flags & O_CLOEXEC) ? 1 : 0;
    ce[rfd] = cflag;
    ce[wfd] = cflag;
    out_fds[0] = rfd;
    out_fds[1] = wfd;
    return 0;
}

int vfs_pipe(int out_fds[2]) {
    return vfs_pipe2(out_fds, 0);
}

int vfs_set_cloexec(int fd, int on) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -EBADF;
    if (current_fd_table()[fd] == NULL) return -EBADF;
    current_cloexec()[fd] = on ? 1 : 0;
    return 0;
}

int vfs_get_cloexec(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return 0;
    return current_cloexec()[fd];
}

/*
 * dup_lowest_from() — duplicate @oldfd into the lowest free slot >= @minfd
 * (F_DUPFD semantics): the new fd shares the same OFD.  @cloexec selects the
 * new fd's FD_CLOEXEC bit.  Returns the new fd or a negative errno.
 */
static int dup_lowest_from(int oldfd, int minfd, int cloexec) {
    struct ofd **t = current_fd_table();
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || t[oldfd] == NULL) return -EBADF;
    if (minfd < 0 || minfd >= VFS_MAX_FDS) return -EINVAL;   /* OPEN_MAX bound */
    int start = (minfd < 3) ? 3 : minfd;   /* never hand out 0/1/2 */
    int nfd = alloc_fd_slot_ptr(t, start);
    if (nfd < 0) return -EMFILE;            /* no slot >= minfd available */
    t[oldfd]->refcount++;
    t[nfd] = t[oldfd];
    current_cloexec()[nfd] = cloexec ? 1 : 0;
    return nfd;
}

/*
 * vfs_fcntl() — POSIX fcntl(2) subset.  Returns a non-negative result or a
 * negative errno.  Keeps the FD_CLOEXEC namespace (F_GETFD/F_SETFD) strictly
 * separate from the file-status-flags namespace (F_GETFL/F_SETFL).
 */
int vfs_fcntl(int fd, int cmd, int arg) {
    struct ofd **t = current_fd_table();
    /* Commands whose @fd must be a valid open descriptor. */
    if (fd < 0 || fd >= VFS_MAX_FDS || t[fd] == NULL) return -EBADF;
    struct ofd *o = t[fd];

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
        /* access mode | live status flags (NOT FD_CLOEXEC, NOT creation flags).
         * Reads the SHARED OFD, so dup'd fds report the same status flags. */
        int r = o->access_mode;
        if (o->append)   r |= O_APPEND;
        if (o->nonblock) r |= O_NONBLOCK;
        return r;
    }
    case F_SETFL:
        /* Only O_APPEND / O_NONBLOCK are changeable; access mode and creation
         * flags in @arg are silently ignored (POSIX requirement).  Because this
         * mutates the shared OFD, dup'd fds see the change too. */
        o->append   = (arg & O_APPEND)   ? 1 : 0;
        o->nonblock = (arg & O_NONBLOCK) ? 1 : 0;
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
        if (cur->cloexec[fd] && cur->fd_table[fd] != NULL) {
            vfs_close(fd);
        }
    }
}

/*
 * vfs_fork_inherit() — share the parent's OFDs with a forked child.
 *
 * The child's FD table entries point at the SAME OFD objects as the parent's
 * (so the seek offset and status flags are shared, per POSIX fork()); each
 * inherited OFD's refcount is incremented.  FD_CLOEXEC flags are copied.  Must
 * be called while building the child, before it becomes schedulable.
 */
void vfs_fork_inherit(struct ofd **dst, struct ofd **src, uint8_t *dst_cloexec,
                      const uint8_t *src_cloexec) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        dst[i] = src[i];
        dst_cloexec[i] = src_cloexec[i];
        if (src[i]) src[i]->refcount++;
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
