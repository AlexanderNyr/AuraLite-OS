#ifndef AURALITE_FS_DEVFS_H
#define AURALITE_FS_DEVFS_H

/*
 * DevFS — a small in-memory filesystem exposing character devices at /dev.
 * Currently provides:
 *   /dev/null  — reads return EOF, writes discard all data
 *   /dev/zero  — reads return zero bytes, writes discard all data
 */

/* Initialise devfs and register its devices. */
void devfs_init(void);

/* VFS operations for devfs. */
extern const struct vfs_ops devfs_ops;

#endif /* AURALITE_FS_DEVFS_H */
