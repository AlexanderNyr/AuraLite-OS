/* diskfs.c — tiny AHCI-backed persistent read/write filesystem.
 *
 * Layout on the first AHCI disk:
 *   LBA 2: superblock
 *   LBA 3: 8 fixed-size file table entries
 *   LBA 4..: file data, 8 sectors (4 KiB) per file slot
 *
 * This is deliberately small and flat. It proves the VFS can use a persistent
 * block device for file create/read/write while larger filesystems are future
 * work.
 */

#include <stdint.h>
#include "kernel/fs/diskfs.h"
#include "drivers/ahci/ahci.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define DISKFS_MAGIC 0x41554653u /* AUFS */
#define DISKFS_VERSION 2
#define DISKFS_MAX_FILES 8
#define DISKFS_NAME_MAX 48
#define DISKFS_FILE_SECTORS 8
#define DISKFS_MAX_FILE_SIZE (DISKFS_FILE_SECTORS * AHCI_SECTOR_SIZE)
#define DISKFS_SUPER_LBA 2
#define DISKFS_TABLE_LBA 3
#define DISKFS_DATA_LBA  5

struct diskfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t max_files;
    uint32_t file_sectors;
    uint8_t reserved[AHCI_SECTOR_SIZE - 16];
} __attribute__((packed));

struct diskfs_entry {
    uint8_t in_use;
    uint8_t reserved0[3];
    uint32_t start_lba;
    uint32_t size;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t atime;
    char name[DISKFS_NAME_MAX];
    uint8_t reserved1[44];
} __attribute__((packed));

typedef char static_assert_entry_size[(sizeof(struct diskfs_entry) == 128) ? 1 : -1];

static int disk_port = -1;
static struct diskfs_entry entries[DISKFS_MAX_FILES];
static struct vnode vnodes[DISKFS_MAX_FILES];

static int valid_name(const char *path) {
    if (!path || !*path || path[0] == '/') return 0;
    for (const char *p = path; *p; p++) if (*p == '/') return 0;
    return strlen(path) < DISKFS_NAME_MAX;
}

static int read_sector(uint64_t lba, void *buf) {
    return ahci_read((uint32_t)disk_port, lba, 1, buf);
}

static int write_sector(uint64_t lba, const void *buf) {
    return ahci_write((uint32_t)disk_port, lba, 1, buf);
}

static void sync_table(void) {
    uint8_t sector[AHCI_SECTOR_SIZE * 2];
    memset(sector, 0, sizeof(sector));
    memcpy(sector, entries, sizeof(entries));
    write_sector(DISKFS_TABLE_LBA, sector);
    write_sector(DISKFS_TABLE_LBA + 1, sector + AHCI_SECTOR_SIZE);
}

static void load_table(void) {
    uint8_t sector[AHCI_SECTOR_SIZE * 2];
    memset(sector, 0, sizeof(sector));
    if (read_sector(DISKFS_TABLE_LBA, sector) == 0 &&
        read_sector(DISKFS_TABLE_LBA + 1, sector + AHCI_SECTOR_SIZE) == 0) {
        memcpy(entries, sector, sizeof(entries));
    }
}

static void rebuild_vnodes(void) {
    memset(vnodes, 0, sizeof(vnodes));
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!entries[i].in_use) continue;
        strncpy(vnodes[i].name, entries[i].name, VFS_PATH_MAX - 1);
        vnodes[i].type = VFS_TYPE_FILE;
        vnodes[i].size = entries[i].size;
        vnodes[i].ops = &diskfs_ops;
        vnodes[i].fs_data = &entries[i];
        vnodes[i].inode_id = (uint64_t)i;
        vnodes[i].mtime = entries[i].mtime;
        vnodes[i].ctime = entries[i].ctime;
        vnodes[i].atime = entries[i].atime;
    }
}

static int format_diskfs(void) {
    kprintf("[diskfs] formatting tiny AUFS at LBA %u...\n", DISKFS_SUPER_LBA);
    struct diskfs_super sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = DISKFS_MAGIC;
    sb.version = DISKFS_VERSION;
    sb.max_files = DISKFS_MAX_FILES;
    sb.file_sectors = DISKFS_FILE_SECTORS;
    if (write_sector(DISKFS_SUPER_LBA, &sb) != 0) return -1;
    memset(entries, 0, sizeof(entries));
    sync_table();
    return 0;
}

int diskfs_init(void) {
    disk_port = ahci_get_first_port();
    if (disk_port < 0) {
        kprintf("[diskfs] no AHCI disk available; /disk not mounted\n");
        return -1;
    }

    struct diskfs_super sb;
    memset(&sb, 0, sizeof(sb));
    if (read_sector(DISKFS_SUPER_LBA, &sb) != 0) {
        kprintf("[diskfs] could not read superblock\n");
        return -1;
    }
    if (sb.magic != DISKFS_MAGIC || sb.version != DISKFS_VERSION) {
        if (format_diskfs() != 0) {
            kprintf("[diskfs] format failed\n");
            return -1;
        }
    } else {
        load_table();
    }
    rebuild_vnodes();
    kprintf("[diskfs] tiny persistent filesystem ready on AHCI port %d\n", disk_port);
    return 0;
}

static struct diskfs_entry *entry_from_vnode(struct vnode *vn) {
    return (struct diskfs_entry *)vn->fs_data;
}

static struct vnode diskfs_root_vnode;

static struct vnode *diskfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (path[0] == '\0') {
        diskfs_root_vnode.type = VFS_TYPE_DIR;
        diskfs_root_vnode.size = 0;
        diskfs_root_vnode.ops  = &diskfs_ops;
        return &diskfs_root_vnode;
    }
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (entries[i].in_use && strcmp(entries[i].name, path) == 0) {
            vnodes[i].size = entries[i].size;
            return &vnodes[i];
        }
    }
    return NULL;
}

static struct vnode *diskfs_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!valid_name(path)) return NULL;
    struct vnode *existing = diskfs_lookup(NULL, path);
    if (existing) return existing;
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!entries[i].in_use) {
            memset(&entries[i], 0, sizeof(entries[i]));
            entries[i].in_use = 1;
            entries[i].start_lba = DISKFS_DATA_LBA + (uint32_t)i * DISKFS_FILE_SECTORS;
            entries[i].size = 0;
            entries[i].mtime = entries[i].ctime = entries[i].atime = vfs_now();
            strncpy(entries[i].name, path, DISKFS_NAME_MAX - 1);
            rebuild_vnodes();
            sync_table();
            return &vnodes[i];
        }
    }
    return NULL;
}

static int64_t diskfs_read(struct vnode *vn, uint64_t pos,
                           void *buf, uint64_t count) {
    struct diskfs_entry *e = entry_from_vnode(vn);
    if (!e || pos >= e->size) return 0;
    if (pos + count > e->size) count = e->size - pos;

    uint8_t tmp[DISKFS_MAX_FILE_SIZE];
    for (uint32_t s = 0; s < DISKFS_FILE_SECTORS; s++) {
        if (read_sector(e->start_lba + s, tmp + s * AHCI_SECTOR_SIZE) != 0) return -1;
    }
    memcpy(buf, tmp + pos, count);
    e->atime = vfs_now();
    sync_table();
    rebuild_vnodes();
    return (int64_t)count;
}

static int64_t diskfs_write(struct vnode *vn, uint64_t pos,
                            const void *buf, uint64_t count) {
    struct diskfs_entry *e = entry_from_vnode(vn);
    if (!e) return -1;
    if (pos + count > DISKFS_MAX_FILE_SIZE || pos + count < pos) return -1;

    uint8_t tmp[DISKFS_MAX_FILE_SIZE];
    memset(tmp, 0, sizeof(tmp));
    for (uint32_t s = 0; s < DISKFS_FILE_SECTORS; s++) {
        (void)read_sector(e->start_lba + s, tmp + s * AHCI_SECTOR_SIZE);
    }
    memcpy(tmp + pos, buf, count);
    uint64_t end = pos + count;
    if (end > e->size) e->size = (uint32_t)end;
    e->mtime = e->ctime = vfs_now();

    for (uint32_t s = 0; s < DISKFS_FILE_SECTORS; s++) {
        if (write_sector(e->start_lba + s, tmp + s * AHCI_SECTOR_SIZE) != 0) return -1;
    }
    sync_table();
    rebuild_vnodes();
    return (int64_t)count;
}

static int diskfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    (void)vn;
    int n = 0;
    for (int i = 0; i < DISKFS_MAX_FILES && n < max; i++) {
        if (!entries[i].in_use) continue;
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, entries[i].name, VFS_PATH_MAX - 1);
        out[n].type = VFS_TYPE_FILE;
        out[n].size = entries[i].size;
        out[n].inode = (uint64_t)i;
        n++;
    }
    return n;
}

static int diskfs_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (entries[i].in_use && strcmp(entries[i].name, path) == 0) {
            memset(&entries[i], 0, sizeof(entries[i]));
            sync_table();
            rebuild_vnodes();
            return 0;
        }
    }
    return -1;
}


static int diskfs_stat(struct vnode *vn, struct vfs_stat *out) {
    if (!vn || !out) return -1;
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
    struct diskfs_entry *e = entry_from_vnode(vn);
    if (!e) return -1;
    out->mtime = e->mtime;
    out->ctime = e->ctime;
    out->atime = e->atime;
    return 0;
}

const struct vfs_ops diskfs_ops = {
    .lookup  = diskfs_lookup,
    .create  = diskfs_create,
    .read    = diskfs_read,
    .write   = diskfs_write,
    .readdir = diskfs_readdir,
    .unlink  = diskfs_unlink,
    .stat    = diskfs_stat,
};

void diskfs_list(void) {
    for (int i = 0; i < DISKFS_MAX_FILES; i++) {
        if (entries[i].in_use) {
            kprintf("  /disk/%s  (%u bytes)\n", entries[i].name, entries[i].size);
        }
    }
}

void diskfs_self_test(void) {
    if (disk_port < 0) return;
    kprintf("[diskfs] self-test: create/write/read /disk/persist.txt...\n");
    struct vnode *vn = diskfs_create(NULL, "persist.txt");
    if (!vn) { kprintf("[diskfs] FAIL: create failed\n"); return; }
    const char *msg = "hello persistent ahci fs";
    if (diskfs_write(vn, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[diskfs] FAIL: write failed\n"); return;
    }
    char buf[64];
    memset(buf, 0, sizeof(buf));
    if (diskfs_read(vn, 0, buf, sizeof(buf)-1) != (int64_t)strlen(msg) ||
        strcmp(buf, msg) != 0) {
        kprintf("[diskfs] FAIL: readback mismatch '%s'\n", buf); return;
    }
    kprintf("[diskfs] PASS: persistent AHCI read/write filesystem works\n");
}
