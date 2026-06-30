#ifndef AURALITE_FS_VFS_H
#define AURALITE_FS_VFS_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/proc/wait_queue.h"

/*
 * Virtual File System.
 *
 * A vnode-based VFS that abstracts over multiple backing filesystems.  Each
 * filesystem registers a set of operations; the VFS dispatches by longest-
 * prefix mount match.
 *
 * Currently mounted:
 *   "/"      USTAR initrd                       (read-only)
 *   "/dev"   devfs                              (/dev/null, /dev/zero)
 *   "/tmp"   tmpfs                              (writable, in-memory)
 *   "/disk"  diskfs                             (tiny persistent, AHCI)
 *   "/fat"   fat32                              (full FAT32 with LFN + subdirs)
 *   "/ext2"  ext2                               (full ext2 with indirect blocks)
 *
 * Most ops are optional — read-only filesystems simply leave them NULL and
 * the VFS returns -ENOTSUP-equivalent (-1).
 */

#define VFS_MAX_MOUNTS   8
#define VFS_PATH_MAX     256
#define VFS_MAX_FDS      64
#define VFS_MAX_DIRENTS  256          /* per readdir() call */

#define VFS_TYPE_FILE     1
#define VFS_TYPE_DIR      2
#define VFS_TYPE_CHARDEV  3
#define VFS_TYPE_SYMLINK  4
#define VFS_TYPE_FIFO     5

/* open(2) flags — Linux/asm-generic ABI values (must match libc/include/fcntl.h).
 *
 * The access mode is a 2-bit enumerated field, NOT independent flags: because
 * O_RDONLY == 0, never test `flags & O_RDONLY`.  Extract it with O_ACCMODE and
 * compare against O_RDONLY/O_WRONLY/O_RDWR.  POSIX.1-2017 <fcntl.h>. */
#define O_RDONLY    0x0000      /* access mode: read only (value 0!) */
#define O_WRONLY    0x0001      /* access mode: write only */
#define O_RDWR      0x0002      /* access mode: read/write */
#define O_ACCMODE   0x0003      /* mask isolating the access-mode field */
#define O_CREAT     0x0040      /* creation: create if absent */
#define O_EXCL      0x0080      /* creation: with O_CREAT, fail if exists */
#define O_TRUNC     0x0200      /* creation: truncate regular file to 0 */
#define O_APPEND    0x0400      /* status:   append before each write */
#define O_NONBLOCK  0x0800      /* status:   non-blocking I/O */
#define O_DIRECTORY 0x10000     /* creation: fail if not a directory */
#define O_CLOEXEC   0x80000     /* creation: set FD_CLOEXEC on the new fd */

/* fcntl(2) commands — Linux/asm-generic ABI values. */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030   /* F_LINUX_SPECIFIC_BASE (1024) + 6 */

/* File-descriptor flag (F_GETFD/F_SETFD namespace; separate from O_* status). */
#define FD_CLOEXEC       1

/* Permission bits (POSIX subset).  Filesystems that don't track perms
 * report 0o755 for directories and 0o644 for files by convention. */
#define VFS_PERM_USR_R  0400
#define VFS_PERM_USR_W  0200
#define VFS_PERM_USR_X  0100

/* Forward declarations. */
struct vnode;
struct ofd;

/* A directory entry returned by readdir(). */
struct vfs_dirent {
    char     name[VFS_PATH_MAX];
    uint32_t type;          /* VFS_TYPE_FILE | VFS_TYPE_DIR | … */
    uint64_t size;          /* file size; 0 for directories */
    uint64_t inode;         /* filesystem-specific identifier */
};

/* Stat information for stat(). */
struct vfs_stat {
    uint32_t type;
    uint32_t mode;          /* perm bits, low 12 bits used */
    uint32_t uid;
    uint32_t gid;
    uint64_t size;          /* in bytes */
    uint64_t inode;         /* fs-specific inode/cluster id */
    uint32_t nlink;         /* hard link count (1 if unknown) */
    uint32_t blocks;        /* allocated blocks (fs unit) */
    uint64_t mtime;         /* modification time (Unix epoch, 0 if unknown) */
    uint64_t ctime;         /* creation time */
    uint64_t atime;         /* access time */
};

/* Filesystem operations.  All paths passed in are relative to the mount root,
 * without a leading slash (e.g. "subdir/file.txt").  Optional ops can be NULL. */
struct vfs_ops {
    /* Required: resolve a path to a vnode, or return NULL. */
    struct vnode *(*lookup)(void *fs_data, const char *path);

    /* Optional: create a regular file. */
    struct vnode *(*create)(void *fs_data, const char *path);

    /* Required for files: read/write. */
    int64_t (*read)(struct vnode *vn, uint64_t pos, void *buf, uint64_t count);
    int64_t (*write)(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count);

    /* Optional: list directory entries.  out[] is caller-provided.  Returns
     * number of entries written, or -1 on error. */
    int (*readdir)(struct vnode *vn, struct vfs_dirent *out, int max);

    /* Optional: directory mutation. */
    int (*mkdir)(void *fs_data, const char *path);
    int (*rmdir)(void *fs_data, const char *path);
    int (*unlink)(void *fs_data, const char *path);
    int (*rename)(void *fs_data, const char *from, const char *to);

    /* Optional: stat (default fills from vnode if NULL). */
    int (*stat)(struct vnode *vn, struct vfs_stat *out);

    /* Optional: truncate to given size (extends with zeros or shrinks). */
    int (*truncate)(struct vnode *vn, uint64_t new_size);

    /* Optional: sync any cached metadata. */
    int (*sync)(void *fs_data);

    /* Optional: device control (ioctl).  @arg is a kernel pointer to a buffer
     * the syscall layer has copied in (and copies back out).  Returns 0 or a
     * negative errno; absence implies the node is not an ioctl-capable device. */
    int (*ioctl)(struct vnode *vn, unsigned long cmd, void *arg);

    /* Optional P7: chmod and chown persistence. */
    int (*chmod)(struct vnode *vn, uint32_t mode);
    int (*chown)(struct vnode *vn, uint32_t uid, uint32_t gid);
};

/* A virtual inode. */
struct vnode {
    char     name[VFS_PATH_MAX];
    uint32_t type;          /* VFS_TYPE_* */
    uint32_t mode;          /* permission bits */
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    const struct vfs_ops *ops;
    void    *fs_data;       /* fs-private (e.g. inode struct pointer) */
    void    *mount_data;    /* mount-private (e.g. superblock) */
    uint64_t inode_id;      /* fs-specific id (cluster for FAT, inode # for ext2) */
    uint64_t mtime;         /* modification time (Unix epoch, 0 if unknown) */
    uint64_t ctime;         /* status/change or creation time, fs-dependent */
    uint64_t atime;         /* access time */
};

/* An open file handle.  File tables live in tcb_t, so fd numbers are
 * per-process; vfs.c keeps a fallback table for early boot before sched_init. */
/*
 * struct ofd — open file description (POSIX.1-2017 XBD §3.258).
 *
 * Holds the seek offset, file status flags (O_APPEND/O_NONBLOCK) and access
 * mode — all of which are SHARED across file descriptors created by dup(),
 * dup2(), fcntl(F_DUPFD) and fork().  A per-process FD table entry is just a
 * pointer to one of these; the per-fd FD_CLOEXEC flag lives in tcb_t::cloexec,
 * never here.
 *
 * refcount counts the number of FD-table slots (across all processes) pointing
 * at this OFD.  It is distinct from the underlying vnode's lifetime: the OFD is
 * freed when refcount reaches 0, and only then is the vnode/pipe released.
 */
struct ofd {
    struct vnode *vn;
    uint64_t      pos;          /* seek offset — shared via the OFD */
    int          access_mode;   /* O_RDONLY / O_WRONLY / O_RDWR (O_ACCMODE field) */
    int          append;        /* 1 if O_APPEND */
    int          nonblock;      /* 1 if O_NONBLOCK */
    int          refcount;      /* # of FD slots referencing this OFD; free at 0 */
    struct wait_queue read_wq;  /* wait queue for select/poll read readiness */
    struct wait_queue write_wq; /* wait queue for select/poll write readiness */
};

struct wait_queue *vfs_get_read_wq(struct ofd *o);
struct wait_queue *vfs_get_write_wq(struct ofd *o);

/* A mount point. */
struct vfs_mount {
    char     mount_path[VFS_PATH_MAX];
    const struct vfs_ops *ops;
    void    *fs_data;
    int      in_use;
};

/* ---- Lifecycle ---- */
void vfs_init(void);
int  vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data);

/* Timestamp helpers (seconds-resolution, realtime clock). */
uint64_t vfs_now(void);
void vfs_stamp_created(struct vnode *vn);
void vfs_stamp_accessed(struct vnode *vn);
void vfs_stamp_modified(struct vnode *vn);
void vfs_stamp_changed(struct vnode *vn);

/* ---- FD-based file I/O ---- */
int     vfs_open(const char *path, int flags, int mode);
int64_t vfs_read(int fd, void *buf, uint64_t count);
int64_t vfs_write(int fd, const void *buf, uint64_t count);
/* lseek whence values. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int64_t vfs_lseek(int fd, int64_t offset, int whence);  /* whence: SEEK_SET/CUR/END */

/* Positional I/O: read/write at @offset WITHOUT changing the shared OFD pos. */
int64_t vfs_pread(int fd, void *buf, uint64_t count, int64_t offset);
int64_t vfs_pwrite(int fd, const void *buf, uint64_t count, int64_t offset);

/* Scatter-gather I/O over a kernel-resident iovec array (already copied in). */
struct iovec {
    void  *iov_base;
    uint64_t iov_len;       /* size_t in userspace */
};
int64_t vfs_readv(int fd, const struct iovec *iov, int iovcnt);
int64_t vfs_writev(int fd, const struct iovec *iov, int iovcnt);

/* OFD-table maintenance used by fork()/exit()/execve() in the process layer. */
void    vfs_fork_inherit(struct ofd **dst, struct ofd **src, uint8_t *dst_cloexec,
                         const uint8_t *src_cloexec);
void    vfs_close_ofd(struct ofd *o, struct vnode *unused);
int     vfs_close(int fd);

/* dup(): allocate a new FD that refers to the same vnode/offset.  Returns
 * the new FD or -1.  Cloexec is cleared on the returned FD (matches POSIX). */
int     vfs_dup(int oldfd);
/* dup2(): like dup but force the new FD number.  Closes newfd first if open. */
int     vfs_dup2(int oldfd, int newfd);
/* pipe(): create a unidirectional in-memory pipe; out_fds[0]=read, out_fds[1]=write. */
int     vfs_pipe(int out_fds[2]);
/* pipe2(): like pipe() but applies O_CLOEXEC / O_NONBLOCK atomically. */
int     vfs_pipe2(int out_fds[2], int flags);
/* fcntl(2): F_GETFL/F_SETFL/F_DUPFD/F_DUPFD_CLOEXEC plus the F_GETFD/F_SETFD
 * flag commands.  Returns a non-negative result or a negative errno. */
int     vfs_fcntl(int fd, int cmd, int arg);

/* ioctl(2): route to the fd's vnode ->ioctl op.  @karg is a kernel buffer the
 * caller has copied the user argument into; on return it holds the result to
 * copy back.  Returns 0 or a negative errno (-ENOTTY if not ioctl-capable). */
int     vfs_ioctl(int fd, unsigned long cmd, void *karg);

/* close-on-exec flag management. */
int     vfs_set_cloexec(int fd, int on);
int     vfs_get_cloexec(int fd);
/* Close every FD with FD_CLOEXEC set.  Called from execve(). */
void    vfs_close_on_exec(void);

/* ---- Path operations (no FD needed) ---- */
int vfs_mkdir(const char *path, uint32_t mode);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *from, const char *to);
int vfs_truncate(const char *path, uint64_t new_size);
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_mkfifo(const char *path, uint32_t mode);

/* readdir: pass a path; entries are written into out[].  Returns count or -1. */
int vfs_readdir(const char *path, struct vfs_dirent *out, int max);

/* Pretty-print a directory listing (for `ls`). */
void vfs_list(const char *path);

/* P7: Permission checking & credential ops */
struct tcb;
int vfs_check_perm(struct vnode *vn, int access, struct tcb *tcb);
struct vnode *vfs_get_vnode(int fd);
int vfs_chmod(const char *path, uint32_t mode);
int vfs_fchmod(int fd, uint32_t mode);
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);
int vfs_fchown(int fd, uint32_t uid, uint32_t gid);
int vfs_access(const char *path, int mode);

/* In-memory symlink registry helpers (implemented in symlink.c). */
int vfs_symlink_lookup(const char *path, char *target, size_t target_len);
struct vnode *vfs_symlink_vnode(const char *path);
int vfs_unlink_symlink(const char *path);

/* Phase 10 gate test. */
void vfs_self_test(void);

#endif /* AURALITE_FS_VFS_H */
