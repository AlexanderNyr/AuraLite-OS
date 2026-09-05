#ifndef AURALITE_FS_DEVFS_H
#define AURALITE_FS_DEVFS_H

#include <stdint.h>

struct vnode;

/*
 * DevFS — a small in-memory filesystem exposing character devices at /dev.
 *
 * RESIDUE2 T3 (RES-07): devfs.c is the PORTABLE core, compiled unchanged
 * on all four architectures; it provides:
 *   /dev/null  — reads return EOF, writes discard all data
 *   /dev/zero  — reads return zero bytes, writes discard all data
 *
 * Platform devices attach through devfs_register_ext().  On x86_64,
 * kernel/fs/devfs_ext.c (devfs_ext_init, wired from kernel.c) adds:
 *   /dev/tty0  — the system console terminal
 *   /dev/audio — the PC speaker / audio buffer sink
 * The ports never register them, so their /dev is null + zero.
 */

/* Initialise devfs and register the portable devices. */
void devfs_init(void);

/* Register a platform character device backed by the given handlers
 * (any handler may be NULL: reads then EOF, writes then discard,
 * ioctl then -ENOTTY).  Returns 0 on success, -1 when the table is
 * full.  Must be called after devfs_init(). */
int devfs_register_ext(const char *name,
                       int64_t (*read_fn)(struct vnode *, uint64_t, void *,
                                          uint64_t),
                       int64_t (*write_fn)(struct vnode *, uint64_t,
                                           const void *, uint64_t),
                       int (*ioctl_fn)(struct vnode *, unsigned long,
                                       void *));

/* x86_64-only: register tty0 + audio (implemented in devfs_ext.c). */
void devfs_ext_init(void);

/* VFS operations for devfs. */
extern const struct vfs_ops devfs_ops;

#endif /* AURALITE_FS_DEVFS_H */
