/* initrd.c — USTAR (POSIX tar) initrd parser.
 *
 * Walks the 512-byte tar headers to build a simple in-memory file table, then
 * exposes it through the VFS ops (lookup, read). Files are read-only.
 */

#include <stdint.h>
#include "kernel/fs/initrd.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"

#define INITRD_MAX_FILES 32

struct initrd_file {
    char     name[VFS_PATH_MAX];
    uint64_t size;
    uint64_t data_offset;   /* offset from initrd base to file data */
};

struct initrd_state {
    uint64_t base;                 /* virtual address of the initrd */
    uint64_t total_size;
    struct initrd_file files[INITRD_MAX_FILES];
    int file_count;
};

static struct initrd_state initrd;
static struct vnode       *initrd_vnodes;   /* pool of vnodes */

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

void initrd_init(uint64_t address, uint64_t size) {
    initrd.base       = address;
    initrd.total_size = size;
    initrd.file_count = 0;

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

        /* Type flag at offset 156: '0' or '\0' = regular file. */
        char typeflag = hdr[156];

        /* Parse the file size (octal at offset 124, 12 bytes). */
        uint64_t fsize = parse_octal(hdr + 124, 12);

        if ((typeflag == '0' || typeflag == '\0') &&
            initrd.file_count < INITRD_MAX_FILES) {
            struct initrd_file *f = &initrd.files[initrd.file_count];
            /* tar stores paths with a leading "./" prefix; strip it. */
            const char *name = hdr;
            if (name[0] == '.' && name[1] == '/') {
                name += 2;
            }
            strncpy(f->name, name, VFS_PATH_MAX - 1);
            f->name[VFS_PATH_MAX - 1] = '\0';
            f->size        = fsize;
            f->data_offset = offset + 512;   /* data follows the header */
            initrd.file_count++;
        }

        /* Advance past header + data (padded up to 512). */
        uint64_t data_blocks = (fsize + 511) / 512;
        offset += 512 + data_blocks * 512;
    }

    /* Allocate a pool of vnodes for the files we found. */
    if (initrd.file_count > 0) {
        initrd_vnodes = kmalloc(sizeof(struct vnode) * initrd.file_count);
        memset(initrd_vnodes, 0, sizeof(struct vnode) * initrd.file_count);
        for (int i = 0; i < initrd.file_count; i++) {
            initrd_vnodes[i].name[0] = '\0';
            initrd_vnodes[i].type    = VFS_TYPE_FILE;
            initrd_vnodes[i].size    = initrd.files[i].size;
            initrd_vnodes[i].ops     = &initrd_ops;
            initrd_vnodes[i].fs_data = (void *)&initrd.files[i];
        }
    }

    kprintf("[initrd] parsed %d file(s), %llu bytes\n",
            initrd.file_count, (unsigned long long)initrd.total_size);
}

/* ---- VFS ops ---- */

static struct vnode *initrd_lookup(const char *path) {
    /* If path is empty, return the root "directory". */
    if (path[0] == '\0') {
        return NULL;   /* initrd root listing handled separately */
    }

    for (int i = 0; i < initrd.file_count; i++) {
        if (strcmp(path, initrd.files[i].name) == 0) {
            strncpy(initrd_vnodes[i].name, initrd.files[i].name,
                    VFS_PATH_MAX - 1);
            return &initrd_vnodes[i];
        }
    }
    return NULL;
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
    memcpy(buf, src, count);
    return (int64_t)count;
}

static int64_t initrd_write(struct vnode *vn, uint64_t pos,
                            const void *buf, uint64_t count) {
    (void)vn; (void)pos; (void)buf; (void)count;
    return -1;   /* initrd is read-only */
}

const struct vfs_ops initrd_ops = {
    .lookup = initrd_lookup,
    .read   = initrd_read,
    .write  = initrd_write,
};

void initrd_list(void) {
    for (int i = 0; i < initrd.file_count; i++) {
        kprintf("  /%s  (%llu bytes)\n",
                initrd.files[i].name,
                (unsigned long long)initrd.files[i].size);
    }
}
