/* vfs.c — virtual file system: mount table, path resolution, FD management.
 *
 * Path resolution: find the longest matching mount point, then call the FS's
 * lookup() with the remaining relative path. For example, "/dev/null" matches
 * the "/dev" mount and calls devfs_lookup("null").
 */

#include <stdint.h>
#include "kernel/fs/vfs.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static struct file      fd_table[VFS_MAX_FDS];

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
    memset(fd_table, 0, sizeof(fd_table));
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
    return -1;
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

/* Resolve a path to a vnode. */
static struct vnode *resolve_path(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return NULL;
    return mounts[m].ops->lookup(rel);
}

int vfs_open(const char *path) {
    struct vnode *vn = resolve_path(path);
    if (vn == NULL) {
        const char *rel = NULL;
        int m = find_mount(path, &rel);
        if (m >= 0 && mounts[m].ops->create) {
            vn = mounts[m].ops->create(rel);
        }
        if (vn == NULL) {
            return -1;
        }
    }

    /* Reserve fd 0/1/2 for stdin/stdout/stderr syscall semantics. */
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            fd_table[i].vn     = vn;
            fd_table[i].pos    = 0;
            fd_table[i].in_use = 1;
            return i;
        }
    }
    return -1;   /* too many open files */
}

int64_t vfs_read(int fd, void *buf, uint64_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    struct file *f = &fd_table[fd];
    int64_t n = f->vn->ops->read(f->vn, f->pos, buf, count);
    if (n > 0) {
        f->pos += (uint64_t)n;
    }
    return n;
}

int64_t vfs_write(int fd, const void *buf, uint64_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    struct file *f = &fd_table[fd];
    int64_t n = f->vn->ops->write(f->vn, f->pos, buf, count);
    if (n > 0) {
        f->pos += (uint64_t)n;
    }
    return n;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    fd_table[fd].in_use = 0;
    fd_table[fd].vn     = NULL;
    fd_table[fd].pos    = 0;
    return 0;
}

void vfs_list(const char *path) {
    extern void initrd_list(void);
    extern void tmpfs_list(void);
    extern void diskfs_list(void);
    if (strcmp(path, "/") == 0) {
        initrd_list();
    } else if (strcmp(path, "/tmp") == 0 || strcmp(path, "/tmp/") == 0) {
        tmpfs_list();
    } else if (strcmp(path, "/disk") == 0 || strcmp(path, "/disk/") == 0) {
        diskfs_list();
    }
}

/*
 * Phase 10 gate test.
 * 1. Open /dev/null, write data, confirm the write succeeds (data discarded).
 * 2. Open /dev/zero, read 4 bytes, confirm they are all zero.
 * 3. If the initrd has files, open the first one and read a few bytes.
 */
void vfs_self_test(void) {
    kprintf("[vfs] self-test: exercising /dev and initrd...\n");

    /* Test /dev/null: writing should succeed. */
    int fd = vfs_open("/dev/null");
    if (fd < 0) {
        kprintf("[vfs] FAIL: cannot open /dev/null\n");
        return;
    }
    const char *msg = "hello /dev/null";
    int64_t n = vfs_write(fd, msg, 15);
    if (n != 15) {
        kprintf("[vfs] FAIL: write to /dev/null returned %lld\n",
                (long long)n);
        vfs_close(fd);
        return;
    }
    vfs_close(fd);
    kprintf("[vfs]   /dev/null: write OK (15 bytes discarded)\n");

    /* Test /dev/zero: reading should return zero bytes. */
    fd = vfs_open("/dev/zero");
    if (fd < 0) {
        kprintf("[vfs] FAIL: cannot open /dev/zero\n");
        return;
    }
    uint8_t zbuf[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    n = vfs_read(fd, zbuf, 4);
    if (n != 4 || zbuf[0] != 0 || zbuf[1] != 0 ||
        zbuf[2] != 0 || zbuf[3] != 0) {
        kprintf("[vfs] FAIL: /dev/zero read = %lld, buf=%02x%02x%02x%02x\n",
                (long long)n, zbuf[0], zbuf[1], zbuf[2], zbuf[3]);
        vfs_close(fd);
        return;
    }
    vfs_close(fd);
    kprintf("[vfs]   /dev/zero: read OK (4 zero bytes)\n");

    /* Test writable tmpfs through the VFS descriptor path. */
    fd = vfs_open("/tmp/vfs.txt");
    if (fd < 0) {
        kprintf("[vfs] FAIL: cannot create /tmp/vfs.txt\n");
        return;
    }
    const char *tmsg = "vfs writable path";
    n = vfs_write(fd, tmsg, strlen(tmsg));
    if (n != (int64_t)strlen(tmsg)) {
        kprintf("[vfs] FAIL: tmpfs write returned %lld\n", (long long)n);
        vfs_close(fd);
        return;
    }
    vfs_close(fd);
    fd = vfs_open("/tmp/vfs.txt");
    char tbuf[32] = {0};
    n = vfs_read(fd, tbuf, sizeof(tbuf) - 1);
    vfs_close(fd);
    if (n != (int64_t)strlen(tmsg) || strcmp(tbuf, tmsg) != 0) {
        kprintf("[vfs] FAIL: tmpfs readback mismatch '%s'\n", tbuf);
        return;
    }
    kprintf("[vfs]   /tmp/vfs.txt: create/write/read OK\n");

    /* Test the initrd: try to open "/init" (or whatever the first file is). */
    fd = vfs_open("/init");
    if (fd >= 0) {
        char fbuf[32] = { 0 };
        n = vfs_read(fd, fbuf, sizeof(fbuf) - 1);
        kprintf("[vfs]   /init: opened, read %lld byte(s)\n", (long long)n);
        if (n > 0) {
            fbuf[n < (int64_t)sizeof(fbuf) - 1 ? n : (int64_t)sizeof(fbuf) - 1] = '\0';
            kprintf("[vfs]   content: \"%s\"\n", fbuf);
        }
        vfs_close(fd);
    } else {
        kprintf("[vfs]   /init: not found (initrd may be empty)\n");
    }

    kprintf("[vfs] PASS: VFS layer functional\n");
}
