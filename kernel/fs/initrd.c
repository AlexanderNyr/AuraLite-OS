/* initrd.c — USTAR (POSIX tar) initrd parser.
 *
 * Walks the 512-byte tar headers to build a simple in-memory file table, then
 * exposes it through the VFS ops (lookup, read). Files are read-only.
 *
 * DIRECTORIES (phase F0 of FSLAYOUT_PLAN.md)
 *
 * The file table is flat: every entry stores its full path relative to the
 * mount root, e.g. "apps/probe".  That was already enough for open() — the
 * VFS hands the whole relative path to initrd_lookup(), which compares it
 * against the stored names — but it was NOT enough for readdir(), which used
 * to return every entry from every read.  Reading "/" therefore listed
 * "apps/probe" as if it were a file in the root, and "/apps" did not resolve
 * at all.
 *
 * Rather than building a tree, the directory view is *derived* from the flat
 * table: at parse time every path prefix ending in '/' is registered as a
 * directory, and readdir() enumerates the immediate children of a prefix,
 * collapsing anything deeper into a single directory entry.  A tree would be
 * the right answer for a writable filesystem; for a read-only image that is
 * parsed once and never mutated, derivation costs one extra pass and no
 * invalidation logic.
 *
 * HARD LINKS (phase F3)
 *
 * F3 moves every program into /bin, /apps, /demos or /tests while keeping a
 * root-level alias for each, so nothing breaks during the move.  Copying the
 * binaries would double a 5 MB image.  USTAR already has the answer: tar
 * emits a type-'1' entry whose linkname names an earlier entry, carrying no
 * data at all.  The parser resolves those to the same data offset, so an
 * alias costs one 512-byte header.
 *
 * Type-'5' (directory) entries are also honoured now.  They are not required
 * — a directory is still derived from any path that implies one — but an
 * empty directory has no file to imply it, and ignoring the entry tar
 * provides in order to re-derive the same fact is a strange way to spend a
 * parse.
 */

#include <stdint.h>
#include "kernel/fs/initrd.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"

/* 41 entries are packed today.  The runtime reorganisation in FSLAYOUT_PLAN
 * phase F3 keeps compatibility aliases for a while, which roughly doubles the
 * count, so the old ceiling of 64 would have been hit mid-plan. */
/* SELFHOST SH5d: 512 -> 1024 files, 32 -> 128 directories.  The image now
 * carries the complete x86_64 kernel source closure (kernel/, drivers/,
 * boot/, w32/ and generated-tool sources) so tcc can build the next kernel
 * inside AuraLite.  Keep these fixed, auditable bounds rather than silently
 * dropping the tail of a USTAR archive: 1024 file records cost about 272 KiB
 * of BSS and 128 directory records about 32 KiB, modest beside the kernel's
 * existing multi-megabyte static tables. */
#define INITRD_MAX_FILES 1024
#define INITRD_MAX_DIRS  128

struct initrd_file {
    char     name[VFS_PATH_MAX];
    uint64_t size;
    uint64_t data_offset;   /* offset from initrd base to file data */
};

/* A derived directory.  `path` is relative to the mount root with no leading
 * or trailing slash; the root directory is the empty string. */
struct initrd_dir {
    char name[VFS_PATH_MAX];
};

struct initrd_state {
    uint64_t base;                 /* virtual address of the initrd */
    uint64_t total_size;
    struct initrd_file files[INITRD_MAX_FILES];
    int file_count;
    struct initrd_dir dirs[INITRD_MAX_DIRS];
    int dir_count;
};

static struct initrd_state initrd;
static struct vnode       *initrd_vnodes;   /* pool of vnodes, one per file */
static struct vnode        initrd_dir_vnodes[INITRD_MAX_DIRS];

/* ---- USTAR parsing ---- */

/* Parse an octal string (up to `len` chars) to a uint64. */
static uint64_t parse_octal(const char *s, int len) {
    uint64_t v = 0;
    for (int i = 0; i < len && s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '7') {
            v = v * 8 + (uint64_t)(s[i] - '0');
        }
    }
    return v;
}

/* Register `path` (relative, no trailing slash) as a directory if it is not
 * already known.  The root ("") is registered by initrd_init() up front so
 * that slot 0 is always the root. */
static void initrd_add_dir(const char *path, size_t len) {
    for (int i = 0; i < initrd.dir_count; i++) {
        if (strlen(initrd.dirs[i].name) == len &&
            memcmp(initrd.dirs[i].name, path, len) == 0) {
            return;
        }
    }
    if (initrd.dir_count >= INITRD_MAX_DIRS) {
        kprintf("[initrd] WARNING: directory table full (%d), "
                "ignoring a directory — increase INITRD_MAX_DIRS\n",
                INITRD_MAX_DIRS);
        return;
    }
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(initrd.dirs[initrd.dir_count].name, path, len);
    initrd.dirs[initrd.dir_count].name[len] = '\0';
    initrd.dir_count++;
}

/* Walk a file's path and register every directory prefix it implies.
 * "apps/gl/probe" registers "apps" and "apps/gl". */
static void initrd_register_prefixes(const char *name) {
    for (size_t i = 0; name[i]; i++) {
        if (name[i] == '/' && i > 0) {
            initrd_add_dir(name, i);
        }
    }
}

int initrd_init(uint64_t address, uint64_t size) {
    initrd.base       = address;
    initrd.total_size = size;
    initrd.file_count = 0;
    initrd.dir_count  = 0;
    /* Slot 0 is the root directory, always present even in an empty image. */
    initrd_add_dir("", 0);

    const uint8_t *p = (const uint8_t *)address;
    uint64_t offset = 0;

    while (offset + 512 <= size) {
        const char *hdr = (const char *)(p + offset);

        /* Two consecutive zero blocks mark end of archive. */
        if (hdr[0] == '\0') {
            break;
        }

        /* Verify the ustar magic at offset 257. */
        if (memcmp(hdr + 257, "ustar", 5) != 0) {
            break;
        }

        /* Type flag at offset 156: '0' or '\0' = regular file, '1' = hard
         * link (linkname at offset 157), '5' = directory. */
        char typeflag = hdr[156];

        /* Parse the file size (octal at offset 124, 12 bytes). */
        uint64_t fsize = parse_octal(hdr + 124, 12);

        /* tar stores paths with a leading "./" prefix; strip it. */
        const char *name = hdr;
        if (name[0] == '.' && name[1] == '/') name += 2;

        if (typeflag == '5') {
            /* An explicit directory.  Its name carries a trailing slash. */
            size_t dlen = strlen(name);
            while (dlen > 0 && name[dlen - 1] == '/') dlen--;
            if (dlen > 0) initrd_add_dir(name, dlen);
        } else if (typeflag == '1') {
            /* A hard link: no data of its own, so it must borrow the target's.
             * The target always appears earlier in the archive — tar cannot
             * emit a link to something it has not written yet — so a single
             * backward search over the table is enough, and there is no
             * forward-reference case to get wrong. */
            const char *target = hdr + 157;
            if (target[0] == '.' && target[1] == '/') target += 2;

            struct initrd_file *src = NULL;
            for (int i = 0; i < initrd.file_count; i++) {
                if (strcmp(initrd.files[i].name, target) == 0) {
                    src = &initrd.files[i];
                    break;
                }
            }
            if (!src) {
                kprintf("[initrd] WARNING: hard link '%s' -> '%s': "
                        "target not found, skipping\n", name, target);
            } else if (initrd.file_count < INITRD_MAX_FILES) {
                struct initrd_file *f = &initrd.files[initrd.file_count];
                strncpy(f->name, name, VFS_PATH_MAX - 1);
                f->name[VFS_PATH_MAX - 1] = '\0';
                f->size        = src->size;
                f->data_offset = src->data_offset;
                initrd.file_count++;
                initrd_register_prefixes(f->name);
            } else {
                kprintf("[initrd] WARNING: file table full (%d), "
                        "skipping link '%s'\n", INITRD_MAX_FILES, name);
            }
        } else if (typeflag == '0' || typeflag == '\0') {
            if (initrd.file_count < INITRD_MAX_FILES) {
                struct initrd_file *f = &initrd.files[initrd.file_count];
                strncpy(f->name, name, VFS_PATH_MAX - 1);
                f->name[VFS_PATH_MAX - 1] = '\0';
                f->size        = fsize;
                f->data_offset = offset + 512;   /* data follows the header */
                initrd.file_count++;
                initrd_register_prefixes(f->name);
            } else {
                kprintf("[initrd] WARNING: file table full (%d), "
                        "skipping '%s' — increase INITRD_MAX_FILES\n",
                        INITRD_MAX_FILES, name);
            }
        }

        /* Advance past header + data (padded up to 512). */
        uint64_t data_blocks = (fsize + 511) / 512;
        offset += 512 + data_blocks * 512;
    }

    /* Allocate a pool of vnodes for the files we found. */
    if (initrd.file_count > 0) {
        initrd_vnodes = kmalloc(sizeof(struct vnode) * initrd.file_count);
        if (initrd_vnodes == NULL) {
            /* Fail the mount: a NULL pool would fault the first file
             * lookup.  Report an empty image so the ops stay harmless
             * even if a caller mounts us anyway. */
            kprintf("[initrd] ERROR: vnode pool for %d file(s) failed to "
                    "allocate; initrd not available\n", initrd.file_count);
            initrd.file_count = 0;
            initrd.dir_count  = 1;   /* keep only the root directory */
            return -1;
        }
        memset(initrd_vnodes, 0, sizeof(struct vnode) * initrd.file_count);
        for (int i = 0; i < initrd.file_count; i++) {
            initrd_vnodes[i].name[0] = '\0';
            initrd_vnodes[i].type    = VFS_TYPE_FILE;
            initrd_vnodes[i].size    = initrd.files[i].size;
            initrd_vnodes[i].ops     = &initrd_ops;
            initrd_vnodes[i].fs_data = (void *)&initrd.files[i];
        }
    }

    /* Directory vnodes are static: there are few of them, they never change,
     * and they must exist before the first lookup. */
    for (int i = 0; i < initrd.dir_count; i++) {
        memset(&initrd_dir_vnodes[i], 0, sizeof(initrd_dir_vnodes[i]));
        initrd_dir_vnodes[i].type    = VFS_TYPE_DIR;
        initrd_dir_vnodes[i].mode    = 0555;   /* read-only image */
        initrd_dir_vnodes[i].size    = 0;
        initrd_dir_vnodes[i].ops     = &initrd_ops;
        initrd_dir_vnodes[i].fs_data = (void *)&initrd.dirs[i];
        strncpy(initrd_dir_vnodes[i].name, initrd.dirs[i].name,
                VFS_PATH_MAX - 1);
    }

    kprintf("[initrd] parsed %d file(s) in %d director%s, %llu bytes\n",
            initrd.file_count, initrd.dir_count,
            initrd.dir_count == 1 ? "y" : "ies",
            (unsigned long long)initrd.total_size);
    return 0;
}

/* ---- VFS ops ---- */

/* Strip any trailing slashes from a relative path.  The VFS already removes
 * the mount prefix and one leading slash, but "/apps/" arrives here as
 * "apps/" and must resolve to the same directory as "apps". */
static size_t trimmed_len(const char *path) {
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    return len;
}

static struct vnode *initrd_lookup(void *fs_data, const char *path) {
    (void)fs_data;

    size_t plen = trimmed_len(path);

    /* Directories first: the empty path is the root, and it is dirs[0]. */
    for (int i = 0; i < initrd.dir_count; i++) {
        size_t dlen = strlen(initrd.dirs[i].name);
        if (dlen == plen && memcmp(initrd.dirs[i].name, path, plen) == 0) {
            return &initrd_dir_vnodes[i];
        }
    }

    /* A trailing slash asserts "this is a directory"; do not let it match a
     * regular file. */
    if (plen != strlen(path)) return NULL;

    for (int i = 0; i < initrd.file_count; i++) {
        if (strcmp(path, initrd.files[i].name) == 0) {
            strncpy(initrd_vnodes[i].name, initrd.files[i].name,
                    VFS_PATH_MAX - 1);
            return &initrd_vnodes[i];
        }
    }
    return NULL;
}

/* True if `name` lives directly inside directory `dir` (which is "" for the
 * root).  Writes the child component into `child` and reports through
 * `is_dir` whether the child is itself a directory (i.e. the name continues
 * past the component). */
static int child_of(const char *dir, const char *name,
                    char *child, size_t child_len, int *is_dir) {
    size_t dlen = strlen(dir);
    if (dlen > 0) {
        if (strncmp(name, dir, dlen) != 0) return 0;
        if (name[dlen] != '/') return 0;
        name += dlen + 1;
    }
    if (name[0] == '\0') return 0;

    size_t i = 0;
    while (name[i] && name[i] != '/') i++;
    *is_dir = (name[i] == '/');
    if (i >= child_len) i = child_len - 1;
    memcpy(child, name, i);
    child[i] = '\0';
    return 1;
}

static int initrd_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    /* fs_data is a struct initrd_dir* for directory vnodes.  A file vnode
     * never reaches here — the VFS rejects readdir on a non-directory. */
    const struct initrd_dir *dir = (const struct initrd_dir *)vn->fs_data;
    const char *base = dir ? dir->name : "";

    int n = 0;
    char child[VFS_PATH_MAX];
    int is_dir = 0;

    /* child_of() bounds the component to sizeof(child) and NUL-terminates it,
     * so a length-limited memcpy is both correct and free of the truncation
     * ambiguity strncpy() raises here. */
    #define EMIT_NAME(dst, src) do {                        \
        size_t l_ = strlen(src);                            \
        if (l_ > VFS_PATH_MAX - 1) l_ = VFS_PATH_MAX - 1;   \
        memcpy((dst), (src), l_);                           \
        (dst)[l_] = '\0';                                   \
    } while (0)

    /* Immediate subdirectories.  Derived directories are already unique, so
     * no de-duplication is needed for this half. */
    for (int i = 0; i < initrd.dir_count && n < max; i++) {
        if (i == 0 && initrd.dirs[i].name[0] == '\0') continue;  /* root */
        if (!child_of(base, initrd.dirs[i].name, child, sizeof(child), &is_dir))
            continue;
        if (is_dir) continue;   /* a grandchild; it belongs to its own parent */
        memset(&out[n], 0, sizeof(out[n]));
        EMIT_NAME(out[n].name, child);
        out[n].type  = VFS_TYPE_DIR;
        out[n].size  = 0;
        out[n].inode = (uint64_t)(0x40000000u + (unsigned)i);
        n++;
    }

    /* Immediate files. */
    for (int i = 0; i < initrd.file_count && n < max; i++) {
        if (!child_of(base, initrd.files[i].name, child, sizeof(child), &is_dir))
            continue;
        if (is_dir) continue;   /* lives in a subdirectory, listed above */
        memset(&out[n], 0, sizeof(out[n]));
        EMIT_NAME(out[n].name, child);
        out[n].type  = VFS_TYPE_FILE;
        out[n].size  = initrd.files[i].size;
        out[n].inode = (uint64_t)i;
        n++;
    }
    #undef EMIT_NAME
    return n;
}

static int64_t initrd_read(struct vnode *vn, uint64_t pos,
                           void *buf, uint64_t count) {
    struct initrd_file *f = (struct initrd_file *)vn->fs_data;
    if (pos >= f->size) {
        return 0;   /* EOF */
    }
    if (pos + count > f->size) {
        count = f->size - pos;
    }
    const uint8_t *src = (const uint8_t *)(initrd.base + f->data_offset + pos);
    memcpy(buf, src, (size_t)count);
    return (int64_t)count;
}

static int64_t initrd_write(struct vnode *vn, uint64_t pos,
                            const void *buf, uint64_t count) {
    (void)vn; (void)pos; (void)buf; (void)count;
    return -EROFS;   /* initrd is read-only */
}

const struct vfs_ops initrd_ops = {
    .lookup  = initrd_lookup,
    .read    = initrd_read,
    .write   = initrd_write,
    .readdir = initrd_readdir,
};

void initrd_list(void) {
    for (int i = 1; i < initrd.dir_count; i++) {
        kprintf("  /%s/\n", initrd.dirs[i].name);
    }
    for (int i = 0; i < initrd.file_count; i++) {
        kprintf("  /%s  (%llu bytes)\n",
                initrd.files[i].name,
                (unsigned long long)initrd.files[i].size);
    }
}
