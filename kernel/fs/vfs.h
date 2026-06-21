#ifndef AURALITE_FS_VFS_H
#define AURALITE_FS_VFS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Virtual File System.
 *
 * A minimal vnode-based VFS that abstracts over multiple backing filesystems.
 * Each filesystem registers a set of operations (lookup, read, write, readdir).
 * Files are opened via vfs_open() which returns a "file handle" carrying a
 * position and a pointer to the underlying vnode.
 *
 * For now the VFS supports:
 *   - initrd (USTAR) mounted at "/"          (read-only)
 *   - devfs mounted at "/dev"                (/dev/null, /dev/zero)
 */

#define VFS_MAX_MOUNTS   8
#define VFS_PATH_MAX     256
#define VFS_MAX_FDS      64

#define VFS_TYPE_FILE     1
#define VFS_TYPE_DIR      2
#define VFS_TYPE_CHARDEV  3

/* Forward declarations. */
struct vnode;
struct file;

/* Filesystem operations — implemented by each backing FS. */
struct vfs_ops {
    /* Resolve a path relative to this mount's root; returns the vnode or NULL. */
    struct vnode *(*lookup)(const char *path);
    /* Read up to `count` bytes from `vnode` at `pos` into `buf`.
     * Returns bytes read (0 = EOF), or -1 on error. */
    int64_t (*read)(struct vnode *vn, uint64_t pos, void *buf, uint64_t count);
    /* Write `count` bytes to `vnode` at `pos` from `buf`. Returns bytes written. */
    int64_t (*write)(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count);
};

/* A virtual inode — the unit a file handle points at. */
struct vnode {
    char     name[VFS_PATH_MAX];
    uint32_t type;          /* VFS_TYPE_* */
    uint64_t size;          /* file size in bytes (0 for devices) */
    const struct vfs_ops *ops;
    void     *fs_data;      /* filesystem-private data (e.g. initrd data ptr) */
};

/* An open file handle — tracks position within a vnode. */
struct file {
    struct vnode *vn;
    uint64_t      pos;
    int           in_use;
};

/* A mount point. */
struct vfs_mount {
    char     mount_path[VFS_PATH_MAX];
    const struct vfs_ops *ops;
    void    *fs_data;       /* filesystem-private mount data */
    int      in_use;
};

/* ---- Lifecycle ---- */

/* Initialise the VFS subsystem and its mount table. */
void vfs_init(void);

/* Mount a filesystem at `path` with the given ops and private data. */
int vfs_mount(const char *path, const struct vfs_ops *ops, void *fs_data);

/*
 * Open a file by path. Returns a file descriptor (>= 0) on success, or -1.
 * The file handle is allocated from a global pool (per-process tables arrive
 * with the PCB; for now a global FD table suffices).
 */
int vfs_open(const char *path);

/* Read from an open file descriptor. Returns bytes read or -1. */
int64_t vfs_read(int fd, void *buf, uint64_t count);

/* Write to an open file descriptor. Returns bytes written or -1. */
int64_t vfs_write(int fd, const void *buf, uint64_t count);

/* Close a file descriptor. Returns 0 on success, -1 on error. */
int vfs_close(int fd);

/* List the files in a directory path (debugging / VFS test). */
void vfs_list(const char *path);

/* Phase 10 gate test: open /dev/null, write to it, open /dev/zero, read zeros.
 * If an initrd file exists, also open and read it. */
void vfs_self_test(void);

#endif /* AURALITE_FS_VFS_H */
