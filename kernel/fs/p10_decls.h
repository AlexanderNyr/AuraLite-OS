/* P10 function declarations */
#ifndef P10_DECLS_H
#define P10_DECLS_H

#include "kernel/fs/vfs.h"
#include <stddef.h>

int do_getcwd(char *buf, size_t size);
int do_chdir(const char *path);
int vfs_readlink(const char *path, char *buf, size_t bufsiz);
int vfs_fstat(int fd, struct vfs_stat *st);
int vfs_lstat(const char *path, struct vfs_stat *st);
int vfs_symlink(const char *target, const char *linkpath);

#endif
