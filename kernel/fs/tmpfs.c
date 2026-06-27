/* tmpfs.c — small writable in-memory filesystem for /tmp.
 *
 * This is intentionally simple: a fixed-size file table and kmalloc-backed file
 * contents. It provides real VFS read/write/create semantics for user programs
 * while persistent disk filesystems are built on top of block devices later.
 */

#include <stdint.h>
#include "kernel/fs/tmpfs.h"
#include "kernel/lib/errno.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define TMPFS_MAX_FILES 64

struct tmpfs_file {
    int in_use;
    char name[VFS_PATH_MAX];
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;
    struct vnode vnode;
};

static struct tmpfs_file files[TMPFS_MAX_FILES];

static int valid_name(const char *path) {
    if (!path || !*path) return 0;
    if (path[0] == '/') return 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') return 0; /* flat tmpfs for now */
    }
    return 1;
}

void tmpfs_init(void) {
    memset(files, 0, sizeof(files));
    kprintf("[tmpfs] writable in-memory filesystem ready (%d files max)\n",
            TMPFS_MAX_FILES);
}

static struct tmpfs_file *find_file(const char *path) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (files[i].in_use && strcmp(files[i].name, path) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

static struct vnode tmpfs_root_vnode;

static struct vnode *tmpfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (path[0] == '\0') {
        tmpfs_root_vnode.type = VFS_TYPE_DIR;
        tmpfs_root_vnode.size = 0;
        tmpfs_root_vnode.ops  = &tmpfs_ops;
        return &tmpfs_root_vnode;
    }
    struct tmpfs_file *f = find_file(path);
    return f ? &f->vnode : NULL;
}

static struct vnode *tmpfs_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!valid_name(path)) return NULL;
    struct tmpfs_file *existing = find_file(path);
    if (existing) return &existing->vnode;

    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!files[i].in_use) {
            struct tmpfs_file *f = &files[i];
            memset(f, 0, sizeof(*f));
            f->in_use = 1;
            strncpy(f->name, path, VFS_PATH_MAX - 1);
            strncpy(f->vnode.name, path, VFS_PATH_MAX - 1);
            f->vnode.type = VFS_TYPE_FILE;
            f->vnode.size = 0;
            f->vnode.ops = &tmpfs_ops;
            f->vnode.fs_data = f;
            return &f->vnode;
        }
    }
    return NULL;
}

static int ensure_capacity(struct tmpfs_file *f, uint64_t need) {
    if (need <= f->capacity) return 0;
    uint64_t cap = f->capacity ? f->capacity : 64;
    while (cap < need) cap *= 2;
    uint8_t *new_data = krealloc(f->data, cap);
    if (!new_data) return -1;
    if (cap > f->capacity) {
        memset(new_data + f->capacity, 0, cap - f->capacity);
    }
    f->data = new_data;
    f->capacity = cap;
    return 0;
}

static int64_t tmpfs_read(struct vnode *vn, uint64_t pos,
                          void *buf, uint64_t count) {
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    if (!f || pos >= f->size) return 0;
    if (pos + count > f->size) count = f->size - pos;
    memcpy(buf, f->data + pos, count);
    return (int64_t)count;
}

static int64_t tmpfs_write(struct vnode *vn, uint64_t pos,
                           const void *buf, uint64_t count) {
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    if (!f) return -EIO;
    uint64_t end = pos + count;
    if (end < pos) return -EINVAL;             /* size_t/offset overflow */
    if (ensure_capacity(f, end) != 0) return -ENOSPC;
    memcpy(f->data + pos, buf, count);
    if (end > f->size) {
        f->size = end;
        f->vnode.size = end;
    }
    return (int64_t)count;
}

static int tmpfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    (void)vn;
    int n = 0;
    for (int i = 0; i < TMPFS_MAX_FILES && n < max; i++) {
        if (!files[i].in_use) continue;
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, files[i].name, VFS_PATH_MAX - 1);
        out[n].type = VFS_TYPE_FILE;
        out[n].size = files[i].size;
        out[n].inode = (uint64_t)i;
        n++;
    }
    return n;
}

static int tmpfs_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    struct tmpfs_file *f = find_file(path);
    if (!f) return -ENOENT;
    if (f->data) kfree(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}

static int tmpfs_truncate(struct vnode *vn, uint64_t new_size) {
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    if (!f) return -EIO;
    if (new_size > f->capacity) {
        if (ensure_capacity(f, new_size) != 0) return -ENOSPC;
    }
    if (new_size < f->size) {
        /* shrinking: data above new_size becomes garbage; zero it. */
        memset(f->data + new_size, 0, f->size - new_size);
    } else if (new_size > f->size) {
        memset(f->data + f->size, 0, new_size - f->size);
    }
    f->size = new_size;
    f->vnode.size = new_size;
    return 0;
}

const struct vfs_ops tmpfs_ops = {
    .lookup   = tmpfs_lookup,
    .create   = tmpfs_create,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readdir  = tmpfs_readdir,
    .unlink   = tmpfs_unlink,
    .truncate = tmpfs_truncate,
};

void tmpfs_list(void) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (files[i].in_use) {
            kprintf("  /tmp/%s  (%llu bytes)\n",
                    files[i].name, (unsigned long long)files[i].size);
        }
    }
}

void tmpfs_self_test(void) {
    kprintf("[tmpfs] self-test: create/write/read /tmp/hello.txt...\n");
    struct vnode *vn = tmpfs_create(NULL, "hello.txt");
    if (!vn) {
        kprintf("[tmpfs] FAIL: create failed\n");
        return;
    }
    const char *msg = "hello writable fs";
    if (tmpfs_write(vn, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[tmpfs] FAIL: write failed\n");
        return;
    }
    char buf[32];
    memset(buf, 0, sizeof(buf));
    int64_t n = tmpfs_read(vn, 0, buf, sizeof(buf) - 1);
    if (n != (int64_t)strlen(msg) || strcmp(buf, msg) != 0) {
        kprintf("[tmpfs] FAIL: readback mismatch '%s'\n", buf);
        return;
    }
    kprintf("[tmpfs] PASS: read/write file support works\n");
}
