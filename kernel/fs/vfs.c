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
#include "kernel/mm/slab.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/time.h"
#include "kernel/limine_requests.h"

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
    struct wait_queue read_wq;
    struct wait_queue write_wq;
    spinlock_t lock;
};

static int64_t pipe_read_op(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    (void)pos;
    struct pipe_ring *p = (struct pipe_ring *)vn->fs_data;
    if (!p) return -1;
    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;
    spinlock_acquire(&p->lock);
    while (got == 0) {
        if (p->used == 0) {
            if (p->writers == 0) { spinlock_release(&p->lock); return 0; } /* EOF */
            /* Interrupted by a signal with no data buffered -> -EINTR. */
            if (signal_interrupted()) { spinlock_release(&p->lock); return -EINTR; }
            __asm__ volatile ("sti" ::: "memory");
            wq_wait(&p->read_wq, &p->lock);
            continue;
        }
        while (got < count && p->used > 0) {
            out[got++] = p->buf[p->tail];
            p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
            p->used--;
        }
    }
    wq_wake_all(&p->write_wq);
    spinlock_release(&p->lock);
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
    spinlock_acquire(&p->lock);
    if (p->readers == 0) { spinlock_release(&p->lock); return pipe_broken(); }
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t put = 0;
    while (put < count) {
        if (p->used == PIPE_BUF_SIZE) {
            if (p->readers == 0) { spinlock_release(&p->lock); return put ? (int64_t)put : pipe_broken(); }
            /* Interrupted by a signal before completing -> partial / -EINTR. */
            if (signal_interrupted()) { spinlock_release(&p->lock); return put ? (int64_t)put : -EINTR; }
            __asm__ volatile ("sti" ::: "memory");
            wq_wait(&p->write_wq, &p->lock);
            continue;
        }
        while (put < count && p->used < PIPE_BUF_SIZE) {
            p->buf[p->head] = in[put++];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
            p->used++;
        }
    }
    wq_wake_all(&p->read_wq);
    spinlock_release(&p->lock);
    return (int64_t)put;
}

static const struct vfs_ops pipe_read_ops  = { .read = pipe_read_op };
static const struct vfs_ops pipe_write_ops = { .write = pipe_write_op };

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
/* Fallback table for early boot or unusual calls before sched_init().  Normal
 * threads/processes use tcb_t::fd_table, so fd numbers are process-local. */
static struct ofd *fallback_fd_table[VFS_MAX_FDS];
static uint8_t     fallback_cloexec[VFS_MAX_FDS];

#define VFS_MAX_NAMED_FIFOS 16
#define VFS_SYMLINK_MAX_FOLLOW 8

struct named_fifo {
    int in_use;
    char path[VFS_PATH_MAX];
    struct pipe_ring *ring;
    struct vnode *vn;
};

static struct named_fifo named_fifos[VFS_MAX_NAMED_FIFOS];

static int64_t fifo_read_op(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    return pipe_read_op(vn, pos, buf, count);
}

static int64_t fifo_write_op(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    return pipe_write_op(vn, pos, buf, count);
}

static const struct vfs_ops fifo_ops = {
    .read = fifo_read_op,
    .write = fifo_write_op,
};

uint64_t vfs_now(void) {
    time_t now = kernel_time(NULL);
    return now > 0 ? (uint64_t)now : 1;
}

void vfs_stamp_created(struct vnode *vn) {
    if (!vn) return;
    uint64_t now = vfs_now();
    vn->mtime = now;
    vn->ctime = now;
    vn->atime = now;
}

void vfs_stamp_accessed(struct vnode *vn) {
    if (!vn) return;
    vn->atime = vfs_now();
}

void vfs_stamp_modified(struct vnode *vn) {
    if (!vn) return;
    uint64_t now = vfs_now();
    vn->mtime = now;
    vn->ctime = now;
}

void vfs_stamp_changed(struct vnode *vn) {
    if (!vn) return;
    vn->ctime = vfs_now();
}

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
        spinlock_acquire(&p->lock);
        if (vn->ops == &pipe_read_ops)  { p->readers--; wq_wake_all(&p->write_wq); }
        if (vn->ops == &pipe_write_ops) { p->writers--; wq_wake_all(&p->read_wq); }
        int kill = (p->readers <= 0 && p->writers <= 0);
        spinlock_release(&p->lock);
        if (kill) {
            kfree(p);
            slab_free(vnode_cache, vn);
        }
    }
    /* Non-pipe vnodes are owned by their filesystem; nothing to free here. */
}

/* Allocate a new OFD referring to @vn with refcount 1. */
static struct ofd *ofd_alloc(struct vnode *vn, int acc, int append, int nonblock) {
    struct ofd *o = slab_alloc(ofd_cache);
    if (!o) return NULL;
    o->vn          = vn;
    o->pos         = append ? vn->size : 0;
    o->access_mode = acc;
    o->append      = append;
    o->nonblock    = nonblock;
    o->refcount    = 1;
    wq_init(&o->read_wq);
    wq_init(&o->write_wq);
    return o;
}

struct wait_queue *vfs_get_read_wq(struct ofd *o) {
    if (!o) return NULL;
    if (o->vn && (o->vn->ops == &pipe_read_ops || o->vn->ops == &pipe_write_ops ||
                  o->vn->ops == &fifo_ops)) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        if (p) return &p->read_wq;
    }
    return &o->read_wq;
}

struct wait_queue *vfs_get_write_wq(struct ofd *o) {
    if (!o) return NULL;
    if (o->vn && (o->vn->ops == &pipe_read_ops || o->vn->ops == &pipe_write_ops ||
                  o->vn->ops == &fifo_ops)) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        if (p) return &p->write_wq;
    }
    return &o->write_wq;
}

/* ---- select()/poll() readiness helpers (BUG-28) ------------------------ */

int vfs_ofd_is_readable(struct ofd *o) {
    if (!o || !o->vn) return 0;
    /* Pipes and FIFOs: readiness is determined by the ring buffer byte count. */
    if (o->vn->ops == &pipe_read_ops || o->vn->ops == &fifo_ops) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        return p && p->used > 0;
    }
    /* Regular files and char devices: position < size, or non-blocking. */
    return o->pos < o->vn->size || o->nonblock;
}

int vfs_ofd_is_writable(struct ofd *o) {
    if (!o || !o->vn) return 0;
    if (o->access_mode == O_RDONLY) return 0;
    /* Pipe write end / FIFO: ready if space is available in the ring buffer. */
    if (o->vn->ops == &pipe_write_ops || o->vn->ops == &fifo_ops) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        return p && p->used < PIPE_BUF_SIZE;
    }
    /* Everything else is considered writable if opened for writing. */
    return 1;
}

/* Drop one reference; free the OFD (and release its backing) at 0. */
static void ofd_put(struct ofd *o) {
    if (!o) return;
    if (__sync_sub_and_fetch(&o->refcount, 1) <= 0) {
        ofd_release_backing(o->vn);
        slab_free(ofd_cache, o);
    }
}

/* Public wrappers used by process and VM layers. */
void vfs_ofd_get(struct ofd *o) {
    if (!o) return;
    /* Existing OFD refcounts are plain integers throughout vfs.c; use an
     * atomic add here so VMA references remain safe if mmap/close race on
     * different CPUs in future SMP configurations. */
    __sync_add_and_fetch(&o->refcount, 1);
}

void vfs_ofd_put(struct ofd *o) {
    ofd_put(o);
}

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
    memset(named_fifos, 0, sizeof(named_fifos));
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
static struct vnode *resolve_path(const char *path);

static struct vnode *resolve_parent_vnode(const char *path) {
    if (!path || !*path) return NULL;
    char parent_path[VFS_PATH_MAX];
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (!last_slash || last_slash == path) {
        return resolve_path("/");
    }
    size_t len = (size_t)(last_slash - path);
    if (len >= sizeof(parent_path)) len = sizeof(parent_path) - 1;
    memcpy(parent_path, path, len);
    parent_path[len] = '\0';
    return resolve_path(parent_path);
}

int vfs_check_perm(struct vnode *vn, int access, struct tcb *tcb_ptr) {
    if (!vn) return -ENOENT;
    if (access == 0) return 0; /* F_OK */
    tcb_t *tcb = (tcb_t *)tcb_ptr;
    if (!tcb || tcb->euid == 0) return 0; /* Root always passes */

    uint16_t mode = vn->mode & 0777u;
    int granted = 0;
    if (tcb->euid == vn->uid) {
        if (mode & 0400u) granted |= 4;
        if (mode & 0200u) granted |= 2;
        if (mode & 0100u) granted |= 1;
    } else {
        int in_group = (tcb->egid == vn->gid);
        if (!in_group) {
            for (int i = 0; i < tcb->ngroups; i++) {
                if (tcb->supplementary_gids[i] == vn->gid) {
                    in_group = 1;
                    break;
                }
            }
        }
        if (in_group) {
            if (mode & 0040u) granted |= 4;
            if (mode & 0020u) granted |= 2;
            if (mode & 0010u) granted |= 1;
        } else {
            if (mode & 0004u) granted |= 4;
            if (mode & 0002u) granted |= 2;
            if (mode & 0001u) granted |= 1;
        }
    }
    if ((granted & access) == access) return 0;
    return -EACCES;
}

struct vnode *vfs_get_vnode(int fd) {
    struct ofd **t = current_fd_table();
    if (fd < 0 || fd >= VFS_MAX_FDS || !t[fd]) return NULL;
    return t[fd]->vn;
}

int vfs_chmod(const char *path, uint32_t mode) {
    struct vnode *vn = resolve_path(path);
    if (!vn) return -ENOENT;
    tcb_t *cur = sched_current();
    if (cur && cur->euid != 0 && cur->euid != vn->uid) return -EPERM;
    vn->mode = (vn->mode & ~07777u) | (mode & 07777u);
    if (vn->ops && vn->ops->chmod) return (int)vfs_wrap_err(vn->ops->chmod(vn, vn->mode), EIO);
    return 0;
}

int vfs_fchmod(int fd, uint32_t mode) {
    struct vnode *vn = vfs_get_vnode(fd);
    if (!vn) return -EBADF;
    tcb_t *cur = sched_current();
    if (cur && cur->euid != 0 && cur->euid != vn->uid) return -EPERM;
    vn->mode = (vn->mode & ~07777u) | (mode & 07777u);
    if (vn->ops && vn->ops->chmod) return (int)vfs_wrap_err(vn->ops->chmod(vn, vn->mode), EIO);
    return 0;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    struct vnode *vn = resolve_path(path);
    if (!vn) return -ENOENT;
    tcb_t *cur = sched_current();
    if (cur && cur->euid != 0) {
        if (uid != (uint32_t)-1 && uid != cur->euid) return -EPERM;
        if (cur->euid != vn->uid) return -EPERM;
    }
    if (uid != (uint32_t)-1) vn->uid = uid;
    if (gid != (uint32_t)-1) vn->gid = gid;
    if (vn->ops && vn->ops->chown) return (int)vfs_wrap_err(vn->ops->chown(vn, uid, gid), EIO);
    return 0;
}

int vfs_fchown(int fd, uint32_t uid, uint32_t gid) {
    struct vnode *vn = vfs_get_vnode(fd);
    if (!vn) return -EBADF;
    tcb_t *cur = sched_current();
    if (cur && cur->euid != 0) {
        if (uid != (uint32_t)-1 && uid != cur->euid) return -EPERM;
        if (cur->euid != vn->uid) return -EPERM;
    }
    if (uid != (uint32_t)-1) vn->uid = uid;
    if (gid != (uint32_t)-1) vn->gid = gid;
    if (vn->ops && vn->ops->chown) return (int)vfs_wrap_err(vn->ops->chown(vn, uid, gid), EIO);
    return 0;
}

int vfs_access(const char *path, int mode) {
    struct vnode *vn = resolve_path(path);
    if (!vn) return -ENOENT;
    tcb_t *cur = sched_current();
    uint32_t saved_euid = cur ? cur->euid : 0;
    uint32_t saved_egid = cur ? cur->egid : 0;
    if (cur) { cur->euid = cur->uid; cur->egid = cur->gid; }
    int err = vfs_check_perm(vn, mode, cur);
    if (cur) { cur->euid = saved_euid; cur->egid = saved_egid; }
    return err;
}

static struct vnode *named_fifo_lookup(const char *path) {
    for (int i = 0; i < VFS_MAX_NAMED_FIFOS; i++) {
        if (named_fifos[i].in_use && strcmp(named_fifos[i].path, path) == 0) {
            return named_fifos[i].vn;
        }
    }
    return NULL;
}

static void symlink_target_to_absolute(const char *linkpath, const char *target,
                                       char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!target || !*target) return;
    if (target[0] == '/') {
        strncpy(out, target, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    size_t parent_len = 1;
    const char *last_slash = NULL;
    for (const char *p = linkpath; p && *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (last_slash && last_slash != linkpath) parent_len = (size_t)(last_slash - linkpath);

    if (parent_len >= out_len) parent_len = out_len - 1;
    memcpy(out, linkpath, parent_len);
    out[parent_len] = '\0';
    size_t used = strlen(out);
    if (parent_len > 1 && out[parent_len - 1] != '/' && used + 1 < out_len) {
        out[used++] = '/';
        out[used] = '\0';
    }
    size_t remain = out_len - used - 1;
    size_t tlen = strlen(target);
    if (tlen > remain) tlen = remain;
    memcpy(out + used, target, tlen);
    out[used + tlen] = '\0';
}

static struct vnode *resolve_path_follow(const char *path, int depth) {
    if (depth > VFS_SYMLINK_MAX_FOLLOW) return NULL;

    char target[VFS_PATH_MAX];
    if (vfs_symlink_lookup(path, target, sizeof(target)) == 0) {
        char next[VFS_PATH_MAX];
        symlink_target_to_absolute(path, target, next, sizeof(next));
        return resolve_path_follow(next, depth + 1);
    }

    struct vnode *fifo = named_fifo_lookup(path);
    if (fifo) return fifo;

    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return NULL;
    return mounts[m].ops->lookup(mounts[m].fs_data, rel);
}

static struct vnode *resolve_path(const char *path) {
    return resolve_path_follow(path, 0);
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
        struct vnode *pvn = resolve_parent_vnode(path);
        int err = vfs_check_perm(pvn, 2 /* W_OK */, sched_current());
        if (err != 0) return err;

        const char *rel = NULL;
        int m = find_mount(path, &rel);
        if (m < 0) return -ENOENT;
        if (!mounts[m].ops->create) return -EROFS;   /* fs cannot create */
        vn = mounts[m].ops->create(mounts[m].fs_data, rel);
        if (vn == NULL) return -EACCES;

        tcb_t *cur = sched_current();
        uint16_t umask = cur ? cur->umask : 0022;
        uint32_t masked_mode = (mode != 0 ? mode : 0666) & 07777u & ~umask;
        vn->mode = masked_mode ? masked_mode : (0644u & ~umask);
        if (cur) { vn->uid = cur->euid; vn->gid = cur->egid; }
        vfs_stamp_created(vn);
        if (vn->ops && vn->ops->chmod) vn->ops->chmod(vn, vn->mode);
        if (vn->ops && vn->ops->chown) vn->ops->chown(vn, vn->uid, vn->gid);

        created = 1;
    } else {
        /* File exists. */
        if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;

        int req_acc = 0;
        if (acc == O_RDONLY) req_acc = 4;
        else if (acc == O_WRONLY) req_acc = 2;
        else if (acc == O_RDWR) req_acc = 4 | 2;
        if (flags & O_TRUNC) req_acc |= 2;
        int err = vfs_check_perm(vn, req_acc, sched_current());
        if (err != 0) return err;
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
    int slot = alloc_fd_slot_ptr(fd_table, 0);
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
                  vn->type == VFS_TYPE_FIFO || vn->type == VFS_TYPE_CHARDEV);
}

int64_t vfs_read(int fd, void *buf, uint64_t count) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    /* A fd opened O_WRONLY is not readable (POSIX: read on such fd -> EBADF). */
    if (o->access_mode == O_WRONLY) return -EBADF;
    if (!o->vn->ops->read) return -EINVAL;   /* object not readable */
    /* O_NONBLOCK: a read that would block returns -EAGAIN instead.  For a pipe
     * with no buffered data and writers still attached, that is a would-block. */
    if (o->nonblock && (o->vn->ops == &pipe_read_ops || o->vn->ops == &fifo_ops) && count > 0) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        if (p && p->used == 0 && p->writers > 0) return -EAGAIN;
    }
    int64_t n = o->vn->ops->read(o->vn, o->pos, buf, count);
    if (n > 0) {
        o->pos += (uint64_t)n;       /* shared OFD offset advances */
        vfs_stamp_accessed(o->vn);
        wq_wake_all(&o->write_wq);
    }
    return vfs_wrap_err(n, EIO);
}

int64_t vfs_write(int fd, const void *buf, uint64_t count) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    /* A fd opened O_RDONLY is not writable (POSIX: write on such fd -> EBADF). */
    if (o->access_mode == O_RDONLY) return -EBADF;
    if (!o->vn->ops->write) return -EINVAL;   /* object not writable */
    /* O_NONBLOCK: a write that would block returns -EAGAIN instead. */
    if (o->nonblock && (o->vn->ops == &pipe_write_ops || o->vn->ops == &fifo_ops) && count > 0) {
        struct pipe_ring *p = (struct pipe_ring *)o->vn->fs_data;
        if (p && p->used == PIPE_BUF_SIZE && p->readers > 0) return -EAGAIN;
    }
    /* O_APPEND: reposition to EOF before each write.  The single-threaded
     * VFS makes the seek-to-EOF + write atomic here; once SMP/preemptive FS
     * access lands this needs a per-vnode write lock (TODO.md). */
    if (o->append) o->pos = o->vn->size;
    int64_t n = o->vn->ops->write(o->vn, o->pos, buf, count);
    if (n > 0) {
        o->pos += (uint64_t)n;
        vfs_stamp_modified(o->vn);
        wq_wake_all(&o->read_wq);
    }
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
    if (n > 0) vfs_stamp_accessed(o->vn);
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
    if (n > 0) vfs_stamp_modified(o->vn);
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
    int nfd = alloc_fd_slot_ptr(t, 0);
    if (nfd < 0) return -EMFILE;
    vfs_ofd_get(t[oldfd]);
    t[nfd] = t[oldfd];
    current_cloexec()[nfd] = 0;   /* FD_CLOEXEC is per-fd; dup() clears it */
    return nfd;
}

int vfs_dup2(int oldfd, int newfd) {
    struct ofd **t = current_fd_table();
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || t[oldfd] == NULL) return -EBADF;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    vfs_ofd_get(t[oldfd]);
    struct ofd *old = t[newfd];
    t[newfd] = t[oldfd];
    current_cloexec()[newfd] = 0;
    if (old) ofd_put(old);
    return newfd;
}

int vfs_pipe2(int out_fds[2], int flags) {
    if (!out_fds) return -EFAULT;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    struct ofd **t = current_fd_table();
    int rfd = alloc_fd_slot_ptr(t, 0);
    if (rfd < 0) return -EMFILE;
    t[rfd] = (struct ofd *)(uintptr_t)1;
    int wfd = alloc_fd_slot_ptr(t, 0);
    if (wfd < 0) { t[rfd] = NULL; return -EMFILE; }

    struct pipe_ring *p = kmalloc(sizeof(*p));
    if (!p) { t[rfd] = NULL; return -ENOMEM; }
    memset(p, 0, sizeof(*p));
    p->readers = 1; p->writers = 1;   /* one read-end OFD, one write-end OFD */
    wq_init(&p->read_wq);
    wq_init(&p->write_wq);
    spinlock_init(&p->lock);

    struct vnode *rvn = slab_alloc(vnode_cache);
    struct vnode *wvn = slab_alloc(vnode_cache);
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
        if (ro) slab_free(ofd_cache, ro);
        if (wo) slab_free(ofd_cache, wo);
        if (rvn) slab_free(vnode_cache, rvn);
        if (wvn) slab_free(vnode_cache, wvn);
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
    int start = minfd;
    int nfd = alloc_fd_slot_ptr(t, start);
    if (nfd < 0) return -EMFILE;            /* no slot >= minfd available */
    vfs_ofd_get(t[oldfd]);
    t[nfd] = t[oldfd];
    current_cloexec()[nfd] = cloexec ? 1 : 0;
    return nfd;
}

/*
 * vfs_fcntl() — POSIX fcntl(2) subset.  Returns a non-negative result or a
 * negative errno.  Keeps the FD_CLOEXEC namespace (F_GETFD/F_SETFD) strictly
 * separate from the file-status-flags namespace (F_GETFL/F_SETFL).
 */
int vfs_ioctl(int fd, unsigned long cmd, void *karg) {
    struct ofd *o = fd_to_ofd(fd);
    if (!o) return -EBADF;
    if (!o->vn || !o->vn->ops->ioctl) return -ENOTTY;   /* not a device */
    return o->vn->ops->ioctl(o->vn, cmd, karg);
}

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
        if (src[i]) vfs_ofd_get(src[i]);
    }
}

/*
 * vfs_read_at_phys() — read bytes from @o at @offset directly into a physical frame.
 * Used by the demand paging fault handler to fill new pages from files.
 * Returns bytes read (>= 0) or a negative errno.
 */
int64_t vfs_read_at_phys(struct ofd *o, uint64_t offset, uint64_t phys, uint64_t count) {
    if (!o || !o->vn) return -EBADF;
    if (!o->vn->ops->read) return -EINVAL;
    
    uint64_t hhdm = limine_get_hhdm_offset();
    void *dst = (void *)(uintptr_t)(hhdm + phys);
    
    /* Call the vnode's read operation. Since we are in kernel context, 
     * we don't need to worry about user-range validation. */
    int64_t n = o->vn->ops->read(o->vn, offset, dst, count);
    return n;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    struct vnode *pvn = resolve_parent_vnode(path);
    int err = vfs_check_perm(pvn, 2 /* W_OK */, sched_current());
    if (err != 0) return err;

    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -ENOENT;
    if (!mounts[m].ops->mkdir) return -ENOSYS;
    int r = (int)vfs_wrap_err(mounts[m].ops->mkdir(mounts[m].fs_data, rel), EACCES);
    if (r == 0) {
        struct vnode *vn = resolve_path(path);
        if (vn) {
            tcb_t *cur = sched_current();
            uint16_t umask = cur ? cur->umask : 0022;
            vn->mode = (mode != 0 ? mode : 0777u) & 07777u & ~umask;
            if (cur) { vn->uid = cur->euid; vn->gid = cur->egid; }
            vfs_stamp_created(vn);
            if (vn->ops && vn->ops->chmod) vn->ops->chmod(vn, vn->mode);
            if (vn->ops && vn->ops->chown) vn->ops->chown(vn, vn->uid, vn->gid);
        }
    }
    return r;
}

int vfs_rmdir(const char *path) {
    struct vnode *pvn = resolve_parent_vnode(path);
    int err = vfs_check_perm(pvn, 2 /* W_OK */, sched_current());
    if (err != 0) return err;

    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -ENOENT;
    if (!mounts[m].ops->rmdir) return -ENOSYS;
    return (int)vfs_wrap_err(mounts[m].ops->rmdir(mounts[m].fs_data, rel), ENOENT);
}

int vfs_unlink(const char *path) {
    struct vnode *pvn = resolve_parent_vnode(path);
    int err = vfs_check_perm(pvn, 2 /* W_OK */, sched_current());
    if (err != 0) return err;

    if (vfs_unlink_symlink(path) == 0) return 0;
    for (int i = 0; i < VFS_MAX_NAMED_FIFOS; i++) {
        if (named_fifos[i].in_use && strcmp(named_fifos[i].path, path) == 0) {
            kfree(named_fifos[i].ring);
            slab_free(vnode_cache, named_fifos[i].vn);
            memset(&named_fifos[i], 0, sizeof(named_fifos[i]));
            return 0;
        }
    }

    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -ENOENT;
    if (!mounts[m].ops->unlink) return -ENOSYS;
    return (int)vfs_wrap_err(mounts[m].ops->unlink(mounts[m].fs_data, rel), ENOENT);
}

int vfs_rename(const char *from, const char *to) {
    struct vnode *pvn1 = resolve_parent_vnode(from);
    int err = vfs_check_perm(pvn1, 2 /* W_OK */, sched_current());
    if (err != 0) return err;
    struct vnode *pvn2 = resolve_parent_vnode(to);
    err = vfs_check_perm(pvn2, 2 /* W_OK */, sched_current());
    if (err != 0) return err;

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
    int r = (int)vfs_wrap_err(vn->ops->truncate(vn, new_size), EIO);
    if (r == 0) vfs_stamp_modified(vn);
    return r;
}

int vfs_mkfifo(const char *path, uint32_t mode) {
    if (!path || path[0] != '/') return -EINVAL;
    if (resolve_path(path) || vfs_symlink_vnode(path)) return -EEXIST;

    struct vnode *pvn = resolve_parent_vnode(path);
    int err = vfs_check_perm(pvn, 2 /* W_OK */, sched_current());
    if (err != 0) return err;

    int slot = -1;
    for (int i = 0; i < VFS_MAX_NAMED_FIFOS; i++) {
        if (!named_fifos[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -ENOSPC;

    struct pipe_ring *ring = kmalloc(sizeof(*ring));
    struct vnode *vn = slab_alloc(vnode_cache);
    if (!ring || !vn) {
        if (ring) kfree(ring);
        if (vn) slab_free(vnode_cache, vn);
        return -ENOMEM;
    }
    memset(ring, 0, sizeof(*ring));
    /* Baseline named FIFOs keep the ring alive while the node exists.  The
     * counters are deliberately non-zero so single-process tests can open the
     * FIFO O_RDWR without an external peer and still exercise wait queues. */
    ring->readers = 1;
    ring->writers = 1;
    wq_init(&ring->read_wq);
    wq_init(&ring->write_wq);
    spinlock_init(&ring->lock);

    memset(vn, 0, sizeof(*vn));
    strncpy(vn->name, path, VFS_PATH_MAX - 1);
    vn->type = VFS_TYPE_FIFO;
    vn->mode = mode & 07777u;
    vn->ops = &fifo_ops;
    vn->fs_data = ring;
    vfs_stamp_created(vn);
    tcb_t *cur = sched_current();
    if (cur) {
        uint16_t umask = cur->umask;
        vn->mode = (mode != 0 ? mode : 0666u) & 07777u & ~umask;
        vn->uid = cur->euid;
        vn->gid = cur->egid;
    }

    named_fifos[slot].in_use = 1;
    strncpy(named_fifos[slot].path, path, VFS_PATH_MAX - 1);
    named_fifos[slot].ring = ring;
    named_fifos[slot].vn = vn;
    return 0;
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
    out->uid   = vn->uid;
    out->gid   = vn->gid;
    out->size  = vn->size;
    out->inode = vn->inode_id;
    out->nlink = 1;
    out->mtime = vn->mtime;
    out->ctime = vn->ctime;
    out->atime = vn->atime;
    return 0;
}

int vfs_readdir(const char *path, struct vfs_dirent *out, int max) {
    if (!out) return -EFAULT;
    if (max <= 0) return -EINVAL;
    struct vnode *vn = resolve_path(path);
    if (!vn) return -ENOENT;
    int err = vfs_check_perm(vn, 4 /* R_OK */, sched_current());
    if (err != 0) return err;
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
