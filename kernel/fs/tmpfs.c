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

struct tmpfs_data {
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t atime;
    uint64_t ino;          /* shared inode identity (hard links) */
    int refs;              /* number of names pointing at this data */
};

struct tmpfs_file {
    int in_use;
    char name[VFS_PATH_MAX];
    struct tmpfs_data *d;   /* Q13: shared data; hard links share one struct */
    struct tmpfs_volume *vol;   /* Q12: owning volume (for nested dirs) */
    struct vnode vnode;
};

/* One mounted volume. */
struct tmpfs_volume {
    const char *mount_path;              /* for diagnostics only */
    const struct vfs_ops *ops;           /* this volume's ops table */
    struct vnode root;
    uint64_t next_ino;                   /* Q13: inode counter */
    struct tmpfs_file files[TMPFS_MAX_FILES];
};

static struct tmpfs_volume tmp_vol;
static struct tmpfs_volume opt_vol;
static struct tmpfs_volume shm_vol;   /* Q12: /dev/shm (POSIX shared memory) */

/* The VFS passes fs_data straight through from vfs_mount(); a NULL means an
 * older caller that predates volumes, which can only mean /tmp. */
static struct tmpfs_volume *vol_of(void *fs_data) {
    return fs_data ? (struct tmpfs_volume *)fs_data : &tmp_vol;
}

/* Q12 (POSIX2024_PLAN.md phase Q12): tmpfs grows real directory semantics.
 * Entries may now contain '/'; an entry whose vnode.type is VFS_TYPE_DIR is
 * a directory.  Invariants:
 *   - every entry's parent (prefix up to the last '/') is an existing DIR
 *     (or the volume root for root-level names);
 *   - a DIR may be rmdir'd only when no entry starts with "<name>/";
 *   - unlink refuses DIRs (EISDIR) and rmdir refuses files (ENOTDIR).
 * The file table stays flat -- names carry the full relative path -- which
 * keeps lookup O(n) over 64 slots and the code tiny. */
static int path_ok(const char *path) {
    if (!path || !*path) return 0;
    if (path[0] == '/') return 0;
    size_t l = strlen(path);
    if (l >= VFS_PATH_MAX - 1) return 0;
    if (path[l - 1] == '/') return 0;
    for (const char *p = path; *p; p++) {
        if (p[0] == '/' && p[1] == '/') return 0;
    }
    return 1;
}

static int is_dir_entry(const struct tmpfs_file *f) {
    return f && f->vnode.type == VFS_TYPE_DIR;
}

static struct tmpfs_file *find_file(struct tmpfs_volume *v, const char *path);

/* No strchr() in the kernel string library. */
static int has_slash(const char *s) {
    for (; *s; s++) {
        if (*s == '/') return 1;
    }
    return 0;
}

/* Parent of `path`: for root-level names the parent is the volume root
 * (out = NULL, return 1).  Otherwise the parent prefix must exist and be a
 * directory: return 0 if it does not exist, -1 if it is not a directory,
 * and set *out on success. */
static int parent_dir_of(struct tmpfs_volume *v, const char *path,
                         struct tmpfs_file **out) {
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }
    if (!slash) { *out = NULL; return 1; }   /* root-level */
    char parent[VFS_PATH_MAX];
    size_t len = (size_t)(slash - path);
    if (len >= sizeof(parent)) return -1;
    memcpy(parent, path, len);
    parent[len] = '\0';
    struct tmpfs_file *p = find_file(v, parent);
    if (!p) return 0;
    if (!is_dir_entry(p)) return -1;
    *out = p;
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
    vol_init(&shm_vol, "/dev/shm", &shmfs_ops);
    kprintf("[tmpfs] writable in-memory filesystem ready "
            "(3 volumes, %d files max each)\n", TMPFS_MAX_FILES);
}

void *tmpfs_volume_tmp(void) { return &tmp_vol; }
void *tmpfs_volume_opt(void) { return &opt_vol; }
void *tmpfs_volume_shm(void) { return &shm_vol; }

static struct tmpfs_file *find_file(struct tmpfs_volume *v, const char *path) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (v->files[i].in_use && strcmp(v->files[i].name, path) == 0) {
            return &v->files[i];
        }
    }
    return NULL;
}

/* Q13: allocate a fresh shared-data block (refs = 1, fresh inode id). */
static struct tmpfs_data *data_alloc(struct tmpfs_volume *v) {
    struct tmpfs_data *d = kmalloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->refs = 1;
    d->ino = ++v->next_ino;
    return d;
}

/* Q13: release one name's reference; frees the data when the last link
 * goes away (unlink of one hard link must not affect the others). */
static void data_release(struct tmpfs_data *d) {
    if (!d) return;
    if (--d->refs <= 0) {
        if (d->data) kfree(d->data);
        kfree(d);
    }
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
    if (!path_ok(path)) return NULL;
    struct tmpfs_file *existing = find_file(v, path);
    if (existing) return &existing->vnode;

    /* Q12: the parent directory must exist and be a directory.  (The VFS
     * layer already reports ENOENT for a missing parent before calling us;
     * reaching here with a missing or non-dir parent means the path is
     * malformed, and refusing with NULL -> EACCES is the documented
     * behaviour -- POSIX would say ENOENT/ENOTDIR here.) */
    struct tmpfs_file *pdir;
    int pr = parent_dir_of(v, path, &pdir);
    if (pr != 1) return NULL;

    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!v->files[i].in_use) {
            struct tmpfs_file *f = &v->files[i];
            struct tmpfs_data *d = data_alloc(v);
            if (!d) return NULL;
            memset(f, 0, sizeof(*f));
            f->in_use = 1;
            f->vol = v;
            f->d = d;
            d->mtime = d->ctime = d->atime = vfs_now();
            strncpy(f->name, path, VFS_PATH_MAX - 1);
            strncpy(f->vnode.name, path, VFS_PATH_MAX - 1);
            f->vnode.type = VFS_TYPE_FILE;
            f->vnode.size = 0;
            f->vnode.ops = v->ops;
            f->vnode.inode_id = d->ino;   /* Q13: hard links share it */
            f->vnode.fs_data = f;
            f->vnode.mtime = d->mtime;
            f->vnode.ctime = d->ctime;
            f->vnode.atime = d->atime;
            return &f->vnode;
        }
    }
    return NULL;
}

/* Q12: directory mutation -- mkdir/rmdir/rename, filling the holes the
 * flat tmpfs never had.  All three validate parent existence and type. */

static int tmpfs_mkdir(void *fs_data, const char *path) {
    struct tmpfs_volume *v = vol_of(fs_data);
    if (!path_ok(path)) return -EINVAL;
    if (find_file(v, path)) return -EEXIST;
    struct tmpfs_file *pdir;
    int pr = parent_dir_of(v, path, &pdir);
    if (pr == 0) return -ENOENT;
    if (pr < 0) return -ENOTDIR;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!v->files[i].in_use) {
            struct tmpfs_file *f = &v->files[i];
            memset(f, 0, sizeof(*f));
            f->in_use = 1;
            f->vol = v;
            strncpy(f->name, path, VFS_PATH_MAX - 1);
            strncpy(f->vnode.name, path, VFS_PATH_MAX - 1);
            f->vnode.type = VFS_TYPE_DIR;
            f->vnode.size = 0;
            f->vnode.ops = v->ops;
            f->vnode.fs_data = f;
            f->vnode.mode = 0777;   /* VFS applies umask afterwards */
            f->vnode.mtime = f->vnode.ctime = f->vnode.atime = vfs_now();
            return 0;
        }
    }
    return -ENOSPC;
}

static int tmpfs_rmdir(void *fs_data, const char *path) {
    struct tmpfs_volume *v = vol_of(fs_data);
    struct tmpfs_file *f = find_file(v, path);
    if (!f) return -ENOENT;
    if (!is_dir_entry(f)) return -ENOTDIR;
    size_t fl = strlen(f->name);
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!v->files[i].in_use || &v->files[i] == f) continue;
        if (strncmp(v->files[i].name, f->name, fl) == 0 &&
            v->files[i].name[fl] == '/')
            return -ENOTEMPTY;
    }
    data_release(f->d);   /* Q13: dirs have d == NULL; harmless */
    memset(f, 0, sizeof(*f));
    return 0;
}

static int ensure_capacity(struct tmpfs_data *d, uint64_t need) {
    if (need <= d->capacity) return 0;
    uint64_t cap = d->capacity ? d->capacity : 64;
    while (cap < need) cap *= 2;
    uint8_t *new_data = krealloc(d->data, cap);
    if (!new_data) return -1;
    if (cap > d->capacity) {
        memset(new_data + d->capacity, 0, cap - d->capacity);
    }
    d->data = new_data;
    d->capacity = cap;
    return 0;
}

static int64_t tmpfs_read(struct vnode *vn, uint64_t pos,
                          void *buf, uint64_t count) {
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    struct tmpfs_data *d = f ? f->d : NULL;
    if (!d || pos >= d->size) return 0;
    if (pos + count > d->size) count = d->size - pos;
    memcpy(buf, d->data + pos, count);
    d->atime = vfs_now();
    vn->atime = d->atime;
    return (int64_t)count;
}

static int64_t tmpfs_write(struct vnode *vn, uint64_t pos,
                           const void *buf, uint64_t count) {
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    struct tmpfs_data *d = f ? f->d : NULL;
    if (!d) return -EIO;
    uint64_t end = pos + count;
    if (end < pos) return -EINVAL;             /* size_t/offset overflow */
    if (ensure_capacity(d, end) != 0) return -ENOSPC;
    memcpy(d->data + pos, buf, count);
    if (end > d->size) {
        d->size = end;
        vn->size = end;
    }
    d->mtime = d->ctime = vfs_now();
    vn->mtime = d->mtime;
    vn->ctime = d->ctime;
    return (int64_t)count;
}

/* Q12: rename.  Files replace files; directories require an empty target
 * (or replace an empty directory); a directory rename rewrites the prefix
 * of every descendant entry, which is exactly what the flat table needs. */
static int tmpfs_rename(void *fs_data, const char *from, const char *to) {
    struct tmpfs_volume *v = vol_of(fs_data);
    if (!path_ok(to)) return -EINVAL;
    struct tmpfs_file *f = find_file(v, from);
    if (!f) return -ENOENT;
    if (strcmp(from, to) == 0) return 0;
    struct tmpfs_file *t = find_file(v, to);
    if (t) {
        if (is_dir_entry(t) && !is_dir_entry(f)) return -EISDIR;
        if (!is_dir_entry(t) && is_dir_entry(f)) return -ENOTDIR;
        if (is_dir_entry(t)) {
            size_t tl = strlen(t->name);
            for (int i = 0; i < TMPFS_MAX_FILES; i++) {
                if (!v->files[i].in_use || &v->files[i] == t) continue;
                if (strncmp(v->files[i].name, t->name, tl) == 0 &&
                    v->files[i].name[tl] == '/')
                    return -ENOTEMPTY;
            }
            memset(t, 0, sizeof(*t));   /* empty directory replaced */
        } else {
            data_release(t->d);         /* Q13: refcounted data */
            memset(t, 0, sizeof(*t));
        }
    }
    struct tmpfs_file *pdir;
    int pr = parent_dir_of(v, to, &pdir);
    if (pr == 0) return -ENOENT;
    if (pr < 0) return -ENOTDIR;

    if (is_dir_entry(f)) {
        size_t fl = strlen(from);
        for (int i = 0; i < TMPFS_MAX_FILES; i++) {
            if (!v->files[i].in_use || &v->files[i] == f) continue;
            if (strncmp(v->files[i].name, from, fl) != 0 ||
                v->files[i].name[fl] != '/')
                continue;
            size_t nl = strlen(to) + strlen(v->files[i].name + fl);
            if (nl >= VFS_PATH_MAX - 1) return -ENAMETOOLONG;
            char newname[VFS_PATH_MAX];
            memcpy(newname, to, strlen(to));
            strcpy(newname + strlen(to), v->files[i].name + fl);
            struct tmpfs_file *collision = find_file(v, newname);
            if (collision && collision != &v->files[i]) return -EEXIST;
            strncpy(v->files[i].name, newname, VFS_PATH_MAX - 1);
            strncpy(v->files[i].vnode.name, newname, VFS_PATH_MAX - 1);
        }
    }
    strncpy(f->name, to, VFS_PATH_MAX - 1);
    strncpy(f->vnode.name, to, VFS_PATH_MAX - 1);
    if (f->d) {   /* Q13: rename updates the shared-data times */
        f->d->mtime = f->d->ctime = vfs_now();
        f->vnode.mtime = f->d->mtime;
        f->vnode.ctime = f->d->ctime;
    }
    return 0;
}

/* readdir gets a vnode, not fs_data, so the volume is recovered from the
 * root vnode's identity or the entry's owning-volume back-pointer. */
static struct tmpfs_volume *vol_of_vnode(struct vnode *vn) {
    if (vn == &tmp_vol.root) return &tmp_vol;
    if (vn == &opt_vol.root) return &opt_vol;
    if (vn == &shm_vol.root) return &shm_vol;
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    if (f && f->vol) return f->vol;
    return &tmp_vol;
}

/* Q12: list the IMMEDIATE children of a directory, with basenames and real
 * entry types (files vs directories).  The flat table stores full paths, so
 * "child of X" means "name starts with X/ and the remainder has no further
 * slash". */
static int tmpfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct tmpfs_volume *v = vol_of_vnode(vn);
    if (vn->type != VFS_TYPE_DIR) return -1;
    const char *dir = "";
    size_t dl = 0;
    if (vn != &v->root) {
        struct tmpfs_file *self = (struct tmpfs_file *)vn->fs_data;
        if (!self) return -1;
        dir = self->name;
        dl = strlen(dir);
    }
    int n = 0;
    for (int i = 0; i < TMPFS_MAX_FILES && n < max; i++) {
        if (!v->files[i].in_use) continue;
        const char *e = v->files[i].name;
        if (dl) {
            if (strncmp(e, dir, dl) != 0 || e[dl] != '/') continue;
            e += dl + 1;
        } else if (has_slash(e)) {
            continue;
        }
        if (!*e || has_slash(e)) continue;   /* immediate children only */
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, e, VFS_PATH_MAX - 1);
        out[n].type = v->files[i].vnode.type;
        out[n].size = v->files[i].d ? v->files[i].d->size : 0;
        out[n].inode = v->files[i].vnode.inode_id;   /* Q13: shared inode */
        n++;
    }
    return n;
}

static int tmpfs_unlink(void *fs_data, const char *path) {
    struct tmpfs_volume *v = vol_of(fs_data);
    struct tmpfs_file *f = find_file(v, path);
    if (!f) return -ENOENT;
    if (is_dir_entry(f)) return -EISDIR;   /* Q12: POSIX unlink(2) on a dir */
    data_release(f->d);                    /* Q13: refcounted data */
    memset(f, 0, sizeof(*f));
    return 0;
}

static int tmpfs_truncate(struct vnode *vn, uint64_t new_size) {
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    struct tmpfs_data *d = f ? f->d : NULL;
    if (!d) return -EIO;
    if (new_size > d->capacity) {
        if (ensure_capacity(d, new_size) != 0) return -ENOSPC;
    }
    if (new_size < d->size) {
        /* shrinking: data above new_size becomes garbage; zero it. */
        memset(d->data + new_size, 0, d->size - new_size);
    } else if (new_size > d->size) {
        memset(d->data + d->size, 0, new_size - d->size);
    }
    d->size = new_size;
    vn->size = new_size;
    d->mtime = d->ctime = vfs_now();
    vn->mtime = d->mtime;
    vn->ctime = d->ctime;
    return 0;
}

/* Q13: hard link.  Shares the data block AND the inode id with the target
 * entry; the shared struct tmpfs_data is refcounted, so unlinking either
 * name keeps the other alive. */
static int tmpfs_link(void *fs_data, const char *old, const char *new) {
    struct tmpfs_volume *v = vol_of(fs_data);
    if (!path_ok(old) || !path_ok(new)) return -EINVAL;
    struct tmpfs_file *o = find_file(v, old);
    if (!o) return -ENOENT;
    if (o->vnode.type == VFS_TYPE_DIR) return -EPERM;   /* vfs_link pre-checks */
    if (find_file(v, new)) return -EEXIST;
    struct tmpfs_file *pdir;
    int pr = parent_dir_of(v, new, &pdir);
    if (pr == 0) return -ENOENT;
    if (pr < 0) return -ENOTDIR;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!v->files[i].in_use) {
            struct tmpfs_file *f = &v->files[i];
            memset(f, 0, sizeof(*f));
            f->in_use = 1;
            f->vol = v;
            f->d = o->d;
            f->d->refs++;
            strncpy(f->name, new, VFS_PATH_MAX - 1);
            strncpy(f->vnode.name, new, VFS_PATH_MAX - 1);
            f->vnode.type = VFS_TYPE_FILE;
            f->vnode.size = o->vnode.size;
            f->vnode.mode = o->vnode.mode;
            f->vnode.uid = o->vnode.uid;
            f->vnode.gid = o->vnode.gid;
            f->vnode.inode_id = o->d->ino;   /* same inode as the target */
            f->vnode.ops = v->ops;
            f->vnode.fs_data = f;
            f->vnode.mtime = o->vnode.mtime;
            f->vnode.ctime = o->vnode.ctime;
            f->vnode.atime = o->vnode.atime;
            return 0;
        }
    }
    return -ENOSPC;
}

/* Q13: utimensat/futimens on tmpfs — persist into the shared data block so
 * every hard link reports the same times. */
static int tmpfs_settimes(struct vnode *vn, uint64_t atime, uint64_t mtime) {
    struct tmpfs_file *f = (struct tmpfs_file *)vn->fs_data;
    struct tmpfs_data *d = f ? f->d : NULL;
    if (!d) return -EIO;
    if (atime != (uint64_t)-1) d->atime = atime;
    if (mtime != (uint64_t)-1) d->mtime = mtime;
    vn->atime = d->atime;
    vn->mtime = d->mtime;
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
    struct tmpfs_data *d = f ? f->d : NULL;
    if (!d) return -EIO;
    out->mtime = d->mtime;
    out->ctime = d->ctime;
    out->atime = d->atime;
    out->nlink = d->refs;   /* Q13: hard links are counted */
    return 0;
}

const struct vfs_ops tmpfs_ops = {
    .lookup   = tmpfs_lookup,
    .create   = tmpfs_create,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readdir  = tmpfs_readdir,
    .mkdir    = tmpfs_mkdir,   /* Q12 */
    .rmdir    = tmpfs_rmdir,   /* Q12 */
    .unlink   = tmpfs_unlink,
    .rename   = tmpfs_rename,  /* Q12 */
    .link     = tmpfs_link,    /* Q13 */
    .settimes = tmpfs_settimes,/* Q13 */
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
    .mkdir    = tmpfs_mkdir,   /* Q12 */
    .rmdir    = tmpfs_rmdir,   /* Q12 */
    .unlink   = tmpfs_unlink,
    .rename   = tmpfs_rename,  /* Q12 */
    .link     = tmpfs_link,    /* Q13 */
    .settimes = tmpfs_settimes,/* Q13 */
    .stat     = tmpfs_stat,
    .truncate = tmpfs_truncate,
};

/* Q12: /dev/shm volume (POSIX shared memory / named-semaphore home).  Same
 * code, distinct ops address, exactly like /opt above. */
const struct vfs_ops shmfs_ops = {
    .lookup   = tmpfs_lookup,
    .create   = tmpfs_create,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readdir  = tmpfs_readdir,
    .mkdir    = tmpfs_mkdir,
    .rmdir    = tmpfs_rmdir,
    .unlink   = tmpfs_unlink,
    .rename   = tmpfs_rename,
    .link     = tmpfs_link,    /* Q13 */
    .settimes = tmpfs_settimes,/* Q13 */
    .stat     = tmpfs_stat,
    .truncate = tmpfs_truncate,
};

static void vol_list(const struct tmpfs_volume *v) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (v->files[i].in_use) {
            uint64_t sz = v->files[i].d ? v->files[i].d->size : 0;
            kprintf("  %s/%s  (%llu bytes)\n", v->mount_path,
                    v->files[i].name, (unsigned long long)sz);
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
