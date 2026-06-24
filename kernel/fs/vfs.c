/* vfs.c — virtual file system: mount table, path resolution, FD management.
 *
 * Path resolution: find the longest matching mount point, then call the FS's
 * lookup() with the remaining relative path.  For example, "/dev/null" matches
 * the "/dev" mount and calls devfs_lookup(fs_data, "null").
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

/* Resolve a path to a vnode (no creation). */
static struct vnode *resolve_path(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return NULL;
    return mounts[m].ops->lookup(mounts[m].fs_data, rel);
}

int vfs_open(const char *path) {
    struct vnode *vn = resolve_path(path);
    if (vn == NULL) {
        const char *rel = NULL;
        int m = find_mount(path, &rel);
        if (m >= 0 && mounts[m].ops->create) {
            vn = mounts[m].ops->create(mounts[m].fs_data, rel);
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
    return -1;
}

int64_t vfs_read(int fd, void *buf, uint64_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;
    struct file *f = &fd_table[fd];
    if (!f->vn->ops->read) return -1;
    int64_t n = f->vn->ops->read(f->vn, f->pos, buf, count);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

int64_t vfs_write(int fd, const void *buf, uint64_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;
    struct file *f = &fd_table[fd];
    if (!f->vn->ops->write) return -1;
    int64_t n = f->vn->ops->write(f->vn, f->pos, buf, count);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

int64_t vfs_lseek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;
    struct file *f = &fd_table[fd];
    int64_t new_pos;
    switch (whence) {
        case 0: new_pos = offset; break;
        case 1: new_pos = (int64_t)f->pos + offset; break;
        case 2: new_pos = (int64_t)f->vn->size + offset; break;
        default: return -1;
    }
    if (new_pos < 0) return -1;
    f->pos = (uint64_t)new_pos;
    return new_pos;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;
    fd_table[fd].in_use = 0;
    fd_table[fd].vn     = NULL;
    fd_table[fd].pos    = 0;
    return 0;
}

/* ---- Path operations ---- */

int vfs_mkdir(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -1;
    if (!mounts[m].ops->mkdir) return -1;
    return mounts[m].ops->mkdir(mounts[m].fs_data, rel);
}

int vfs_rmdir(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -1;
    if (!mounts[m].ops->rmdir) return -1;
    return mounts[m].ops->rmdir(mounts[m].fs_data, rel);
}

int vfs_unlink(const char *path) {
    const char *rel = NULL;
    int m = find_mount(path, &rel);
    if (m < 0) return -1;
    if (!mounts[m].ops->unlink) return -1;
    return mounts[m].ops->unlink(mounts[m].fs_data, rel);
}

int vfs_rename(const char *from, const char *to) {
    const char *rel_from = NULL, *rel_to = NULL;
    int m_from = find_mount(from, &rel_from);
    int m_to   = find_mount(to,   &rel_to);
    if (m_from < 0 || m_to < 0 || m_from != m_to) return -1;
    if (!mounts[m_from].ops->rename) return -1;
    return mounts[m_from].ops->rename(mounts[m_from].fs_data, rel_from, rel_to);
}

int vfs_truncate(const char *path, uint64_t new_size) {
    struct vnode *vn = resolve_path(path);
    if (!vn || !vn->ops->truncate) return -1;
    return vn->ops->truncate(vn, new_size);
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!out) return -1;
    struct vnode *vn = resolve_path(path);
    if (!vn) return -1;
    if (vn->ops->stat) return vn->ops->stat(vn, out);
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
    if (!out || max <= 0) return -1;
    struct vnode *vn = resolve_path(path);
    if (!vn) return -1;
    if (vn->type != VFS_TYPE_DIR) return -1;
    if (!vn->ops->readdir) return -1;
    return vn->ops->readdir(vn, out, max);
}

void vfs_list(const char *path) {
    /* Try the generic readdir path first.  If the underlying fs supports it,
     * we get a uniform listing.  Otherwise fall back to fs-specific shims. */
    struct vfs_dirent ents[64];
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
        return;
    }

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

    int fd = vfs_open("/dev/null");
    if (fd < 0) { kprintf("[vfs] FAIL: cannot open /dev/null\n"); return; }
    const char *msg = "hello /dev/null";
    int64_t n = vfs_write(fd, msg, 15);
    if (n != 15) {
        kprintf("[vfs] FAIL: write to /dev/null returned %lld\n", (long long)n);
        vfs_close(fd); return;
    }
    vfs_close(fd);
    kprintf("[vfs]   /dev/null: write OK (15 bytes discarded)\n");

    fd = vfs_open("/dev/zero");
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

    fd = vfs_open("/tmp/vfs.txt");
    if (fd < 0) { kprintf("[vfs] FAIL: cannot create /tmp/vfs.txt\n"); return; }
    const char *tmsg = "vfs writable path";
    n = vfs_write(fd, tmsg, strlen(tmsg));
    if (n != (int64_t)strlen(tmsg)) {
        kprintf("[vfs] FAIL: tmpfs write returned %lld\n", (long long)n);
        vfs_close(fd); return;
    }
    vfs_close(fd);
    fd = vfs_open("/tmp/vfs.txt");
    char tbuf[32] = {0};
    n = vfs_read(fd, tbuf, sizeof(tbuf) - 1);
    vfs_close(fd);
    if (n != (int64_t)strlen(tmsg) || strcmp(tbuf, tmsg) != 0) {
        kprintf("[vfs] FAIL: tmpfs readback mismatch '%s'\n", tbuf); return;
    }
    kprintf("[vfs]   /tmp/vfs.txt: create/write/read OK\n");

    fd = vfs_open("/init");
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
