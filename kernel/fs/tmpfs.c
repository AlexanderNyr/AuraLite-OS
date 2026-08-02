/* tmpfs.c — small writable in-memory filesystem.
 *
 * This is intentionally simple: a fixed-size file table and kmalloc-backed file
 * contents. It provides real VFS read/write/create semantics for user programs
 * while persistent disk filesystems are built on top of block devices later.
 *
 * TWO VOLUMES (phase F1 of FSLAYOUT_PLAN.md)
 *
 * The filesystem used to be a single global file table, because there was a
 * single mount.  /opt needs its own storage: an install must not be able to
 * fill up, or be evicted by, ordinary scratch traffic in /tmp, and the two
 * directories have opposite intents.
 *
 * Each volume therefore owns a file table and its OWN ops table.  The ops
 * tables are functionally identical, and that is deliberate rather than
 * accidental duplication: vfs_vnode_path() identifies a vnode's mount by its
 * ops pointer, so two mounts sharing one ops table would be
 * indistinguishable, and fchmod() on a file in /opt would be judged as if it
 * were in /tmp.  Distinct addresses are what make the mounts tellable apart.
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
    uint64_t mtime;
    uint64_t ctime;
    uint64_t atime;
    struct vnode vnode;
};

/* One mounted volume. */
struct tmpfs_volume {
    const char *mount_path;              /* for diagnostics only */
    const struct vfs_ops *ops;           /* this volume's ops table */
    struct vnode root;
    struct tmpfs_file files[TMPFS_MAX_FILES];
};

static struct tmpfs_volume tmp_vol;
static struct tmpfs_volume opt_vol;

/* The VFS passes fs_data straight through from vfs_mount(); a NULL means an
 * older caller that predates volumes, which can only mean /tmp. */
static struct tmpfs_volume *vol_of(void *fs_data) {
    return fs_data ? (struct tmpfs_volume *)fs_data : &tmp_vol;
}

static int valid_name(const char *path) {
    if (!path || !*path) return 0;
    if (path[0] == '/') return 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') return 0; /* flat tmpfs for now */
    }
    return 1;
}

static void vol_init(struct tmpfs_volume *v, const char *mount_path,
                     const struct vfs_ops *ops) {
    memset(v, 0, sizeof(*v));
    v->mount_path = mount_path;
    v->ops        = ops;
    v->root.type  = VFS_TYPE_DIR;
    v->root.mode  = 0755;
    v->root.size  = 0;
    v->root.ops   = ops;
}

void tmpfs_init(void) {
    vol_init(&tmp_vol, "/tmp", &tmpfs_ops);
    vol_init(&opt_vol, "/opt", &optfs_ops);
    kprintf("[tmpfs] writable in-memory filesystem ready "
            "(2 volumes, %d files max each)\n", TMPFS_MAX_FILES);
}

void *tmpfs_volume_tmp(void) { return &tmp_vol; }
void *tmpfs_volume_opt(void) { return &opt_vol; }

static struct tmpfs_file *find_file(struct tmpfs_volume *v, const char *path) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (v->files[i].in_use && strcmp(v->files[i].name, path) == 0) {
            return &v->files[i];
        }
    }
    return NULL;
}

static struct vnode *tmpfs_lookup(void *fs_data, const char *path) {
    struct tmpfs_volume *v = vol_of(fs_data);
    if (path[0] == '\0') {
        return &v->root;
    }
    struct tmpfs_file *f = find_file(v, path);
    return f ? &f->vnode : NULL;
}

static struct vnode *tmpfs_create(void *fs_data, const char *path) {
    struct tmpfs_volume *v = vol_of(fs_data);
    if (!valid_name(path)) return NULL;
    struct tmpfs_file *existing = find_file(v, path);
    if (existing) return &existing->vnode;

    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!v->files[i].in_use) {
            struct tmpfs_file *f = &v->files[i];
            memset(f, 0, sizeof(*f));
            f->in_use = 1;
            strncpy(f->name, path, VFS_PATH_MAX - 1);
            strncpy(f->vnode.name, path, VFS_PATH_MAX - 1);
            f->vnode.type = VFS_TYPE_FILE;
            f->vnode.size = 0;
            f->vnode.ops = v->ops;
            f->mtime = f->ctime = f->atime = vfs_now();
            f->vnode.fs_data = f;
            f->vnode.mtime = f->mtime;
            f->vnode.ctime = f->ctime;
            f->vnode.atime = f->atime;
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
    f->atime = vfs_now();
    vn->atime = f->atime;
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
    f->mtime = f->ctime = vfs_now();
    f->vnode.mtime = f->mtime;
    f->vnode.ctime = f->ctime;
    return (int64_t)count;
}

/* readdir gets a vnode, not fs_data, so the volume is recovered from the
 * root vnode's identity. */
static struct tmpfs_volume *vol_of_vnode(struct vnode *vn) {
    if (vn == &tmp_vol.root) return &tmp_vol;
    if (vn == &opt_vol.root) return &opt_vol;
    return &tmp_vol;
}

static int tmpfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct tmpfs_volume *v = vol_of_vnode(vn);
    int n = 0;
    for (int i = 0; i < TMPFS_MAX_FILES && n < max; i++) {
        if (!v->files[i].in_use) continue;
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, v->files[i].name, VFS_PATH_MAX - 1);
        out[n].type = VFS_TYPE_FILE;
        out[n].size = v->files[i].size;
        out[n].inode = (uint64_t)i;
        n++;
    }
    return n;
}

static int tmpfs_unlink(void *fs_data, const char *path) {
    struct tmpfs_volume *v = vol_of(fs_data);
    struct tmpfs_file *f = find_file(v, path);
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
    f->mtime = f->ctime = vfs_now();
    f->vnode.mtime = f->mtime;
    f->vnode.ctime = f->ctime;
    return 0;
}

static int tmpfs_stat(struct vnode *vn, struct vfs_stat *out) {
    if (!vn || !out) return -EIO;
    memset(out, 0, sizeof(*out));
    out->type = vn->type;
    out->mode = vn->mode ? vn->mode : (vn->type == VFS_TYPE_DIR ? 0755 : 0644);
    out->uid = vn->uid;
    out->gid = vn->gid;
    out->size = vn->size;
    out->inode = vn->inode_id;
    out->nlink = 1;
    if (vn->type == VFS_TYPE_DIR) {
        out->mtime = vn->mtime;
        out->ctime = vn->ctime;
        out->atime = vn->atime;
        return 0;
    }
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    if (!f) return -EIO;
    out->mtime = f->mtime;
    out->ctime = f->ctime;
    out->atime = f->atime;
    return 0;
}

const struct vfs_ops tmpfs_ops = {
    .lookup   = tmpfs_lookup,
    .create   = tmpfs_create,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readdir  = tmpfs_readdir,
    .unlink   = tmpfs_unlink,
    .stat     = tmpfs_stat,
    .truncate = tmpfs_truncate,
};

/* Identical by design — see the header comment.  The distinct address is the
 * point: it is how vfs_vnode_path() tells /opt from /tmp. */
const struct vfs_ops optfs_ops = {
    .lookup   = tmpfs_lookup,
    .create   = tmpfs_create,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readdir  = tmpfs_readdir,
    .unlink   = tmpfs_unlink,
    .stat     = tmpfs_stat,
    .truncate = tmpfs_truncate,
};

static void vol_list(const struct tmpfs_volume *v) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (v->files[i].in_use) {
            kprintf("  %s/%s  (%llu bytes)\n", v->mount_path,
                    v->files[i].name, (unsigned long long)v->files[i].size);
        }
    }
}

void tmpfs_list(void) { vol_list(&tmp_vol); }
void optfs_list(void) { vol_list(&opt_vol); }

void tmpfs_self_test(void) {
    kprintf("[tmpfs] self-test: create/write/read /tmp/hello.txt...\n");
    struct vnode *vn = tmpfs_create(&tmp_vol, "hello.txt");
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
