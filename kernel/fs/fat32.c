/* fat32.c — small FAT32 implementation for VM hard-disk logging.
 *
 * This is a deliberately compact FAT32 driver for the AuraLite VM use case:
 * a flat root directory, 8.3 filenames, 512-byte sectors, one sector per
 * cluster, one FAT, and AHCI as the block backend. If no FAT32 volume is found
 * at FAT32_BASE_LBA, the driver formats a small FAT32 superfloppy volume there.
 */

#include <stdint.h>
#include "kernel/fs/fat32.h"
#include "drivers/ahci/ahci.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/klog.h"
#include "kernel/lib/string.h"

#define FAT32_BASE_LBA        64u
#define FAT32_TOTAL_SECTORS   8192u
#define FAT32_RESERVED        32u
#define FAT32_NUM_FATS        1u
#define FAT32_FAT_SECTORS     64u
#define FAT32_ROOT_CLUSTER    2u
#define FAT32_EOC             0x0FFFFFFFu
#define FAT32_FREE            0x00000000u
#define FAT32_MAX_FILES       16u
#define FAT32_MAX_CLUSTERS    8192u

struct fat32_file {
    int in_use;
    char display[13];
    char name83[11];
    uint32_t first_cluster;
    uint32_t size;
    uint32_t dir_index;
    struct vnode vnode;
};

static int fat_port = -1;
static uint32_t fat_lba = FAT32_BASE_LBA + FAT32_RESERVED;
static uint32_t data_lba = FAT32_BASE_LBA + FAT32_RESERVED + FAT32_FAT_SECTORS;
static uint32_t cluster_count = 0;
static struct fat32_file files[FAT32_MAX_FILES];
static uint8_t sector[AHCI_SECTOR_SIZE];
static int fat_ready = 0;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int read_sector(uint32_t lba, void *buf) {
    return ahci_read((uint32_t)fat_port, lba, 1, buf);
}
static int write_sector(uint32_t lba, const void *buf) {
    return ahci_write((uint32_t)fat_port, lba, 1, buf);
}
static uint32_t cluster_to_lba(uint32_t cl) {
    return data_lba + (cl - 2u);
}

static void make_display(const char name83[11], char out[13]) {
    int p = 0;
    for (int i = 0; i < 8 && name83[i] != ' '; i++) out[p++] = name83[i];
    int has_ext = 0;
    for (int i = 8; i < 11; i++) if (name83[i] != ' ') has_ext = 1;
    if (has_ext) {
        out[p++] = '.';
        for (int i = 8; i < 11 && name83[i] != ' '; i++) out[p++] = name83[i];
    }
    out[p] = 0;
}

static int to_name83(const char *path, char out[11]) {
    if (!path || !*path || path[0] == '/') return -1;
    memset(out, ' ', 11);
    int i = 0;
    while (*path && *path != '.' && i < 8) {
        char c = *path++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return -1;
        out[i++] = c;
    }
    if (*path == '.') {
        path++;
        i = 8;
        int end = 11;
        while (*path && i < end) {
            char c = *path++;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return -1;
            out[i++] = c;
        }
    }
    if (*path) return -1;
    return 0;
}

static uint32_t fat_get(uint32_t cl) {
    if (cl >= FAT32_MAX_CLUSTERS) return FAT32_EOC;
    uint32_t off = cl * 4u;
    uint32_t lba = fat_lba + off / AHCI_SECTOR_SIZE;
    uint32_t so = off % AHCI_SECTOR_SIZE;
    if (read_sector(lba, sector) != 0) return FAT32_EOC;
    return rd32(sector + so) & 0x0FFFFFFF;
}

static int fat_set(uint32_t cl, uint32_t val) {
    if (cl >= FAT32_MAX_CLUSTERS) return -1;
    uint32_t off = cl * 4u;
    uint32_t lba = fat_lba + off / AHCI_SECTOR_SIZE;
    uint32_t so = off % AHCI_SECTOR_SIZE;
    if (read_sector(lba, sector) != 0) return -1;
    wr32(sector + so, val & 0x0FFFFFFF);
    return write_sector(lba, sector);
}

static uint32_t alloc_cluster(void) {
    for (uint32_t cl = 3; cl < cluster_count + 2 && cl < FAT32_MAX_CLUSTERS; cl++) {
        if (fat_get(cl) == FAT32_FREE) {
            if (fat_set(cl, FAT32_EOC) != 0) return 0;
            memset(sector, 0, sizeof(sector));
            write_sector(cluster_to_lba(cl), sector);
            return cl;
        }
    }
    return 0;
}

static int ensure_chain(struct fat32_file *f, uint32_t clusters_needed) {
    if (clusters_needed == 0) return 0;
    if (f->first_cluster == 0) {
        f->first_cluster = alloc_cluster();
        if (!f->first_cluster) return -1;
    }
    uint32_t cl = f->first_cluster;
    uint32_t have = 1;
    while (have < clusters_needed) {
        uint32_t next = fat_get(cl);
        if (next >= 0x0FFFFFF8 || next == 0) {
            uint32_t n = alloc_cluster();
            if (!n) return -1;
            if (fat_set(cl, n) != 0) return -1;
            cl = n;
        } else {
            cl = next;
        }
        have++;
    }
    return 0;
}

static uint32_t cluster_at(struct fat32_file *f, uint32_t index) {
    uint32_t cl = f->first_cluster;
    while (index-- && cl >= 2 && cl < 0x0FFFFFF8) cl = fat_get(cl);
    return cl;
}

static int update_dir_entry(struct fat32_file *f) {
    if (read_sector(cluster_to_lba(FAT32_ROOT_CLUSTER), sector) != 0) return -1;
    uint8_t *e = sector + f->dir_index * 32u;
    memcpy(e, f->name83, 11);
    e[11] = 0x20; /* archive */
    wr16(e + 20, (uint16_t)(f->first_cluster >> 16));
    wr16(e + 26, (uint16_t)(f->first_cluster & 0xFFFF));
    wr32(e + 28, f->size);
    return write_sector(cluster_to_lba(FAT32_ROOT_CLUSTER), sector);
}

static void rebuild_files(void) {
    memset(files, 0, sizeof(files));
    if (read_sector(cluster_to_lba(FAT32_ROOT_CLUSTER), sector) != 0) return;
    for (uint32_t i = 0; i < 16 && i < FAT32_MAX_FILES; i++) {
        uint8_t *e = sector + i * 32u;
        if (e[0] == 0x00 || e[0] == 0xE5) continue;
        if (e[11] & 0x08) continue; /* volume label */
        struct fat32_file *f = &files[i];
        f->in_use = 1;
        memcpy(f->name83, e, 11);
        make_display(f->name83, f->display);
        f->first_cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        f->size = rd32(e + 28);
        f->dir_index = i;
        strncpy(f->vnode.name, f->display, VFS_PATH_MAX - 1);
        f->vnode.type = VFS_TYPE_FILE;
        f->vnode.size = f->size;
        f->vnode.ops = &fat32_ops;
        f->vnode.fs_data = f;
    }
}

static int format_volume(void) {
    kprintf("[fat32] formatting FAT32 volume at LBA %u (%u sectors)...\n",
            FAT32_BASE_LBA, FAT32_TOTAL_SECTORS);
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xEB; sector[1] = 0x58; sector[2] = 0x90;
    memcpy(sector + 3, "AURALITE", 8);
    wr16(sector + 11, 512);
    sector[13] = 1; /* sectors/cluster */
    wr16(sector + 14, FAT32_RESERVED);
    sector[16] = FAT32_NUM_FATS;
    wr32(sector + 32, FAT32_TOTAL_SECTORS);
    sector[21] = 0xF8;
    wr16(sector + 24, 63);
    wr16(sector + 26, 255);
    wr32(sector + 28, FAT32_BASE_LBA);
    wr32(sector + 36, FAT32_FAT_SECTORS);
    wr32(sector + 44, FAT32_ROOT_CLUSTER);
    wr16(sector + 48, 1);  /* FSInfo */
    wr16(sector + 50, 6);  /* backup boot */
    sector[64] = 0x80;
    sector[66] = 0x29;
    wr32(sector + 67, 0xA2026022u);
    memcpy(sector + 71, "AURALITE   ", 11);
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55; sector[511] = 0xAA;
    if (write_sector(FAT32_BASE_LBA, sector) != 0) return -1;

    memset(sector, 0, sizeof(sector));
    wr32(sector + 0, 0x41615252);
    wr32(sector + 484, 0x61417272);
    wr32(sector + 488, 0xFFFFFFFF);
    wr32(sector + 492, 3);
    sector[510] = 0x55; sector[511] = 0xAA;
    write_sector(FAT32_BASE_LBA + 1, sector);

    memset(sector, 0, sizeof(sector));
    for (uint32_t i = 0; i < FAT32_FAT_SECTORS; i++) write_sector(fat_lba + i, sector);
    read_sector(fat_lba, sector);
    wr32(sector + 0, 0x0FFFFFF8);
    wr32(sector + 4, FAT32_EOC);
    wr32(sector + 8, FAT32_EOC); /* root dir */
    write_sector(fat_lba, sector);

    memset(sector, 0, sizeof(sector));
    write_sector(cluster_to_lba(FAT32_ROOT_CLUSTER), sector);
    return 0;
}

static int parse_or_format(void) {
    if (read_sector(FAT32_BASE_LBA, sector) != 0) return -1;
    if (sector[510] != 0x55 || sector[511] != 0xAA || memcmp(sector + 82, "FAT32", 5) != 0) {
        if (format_volume() != 0) return -1;
        if (read_sector(FAT32_BASE_LBA, sector) != 0) return -1;
    }
    uint16_t bps = rd16(sector + 11);
    uint8_t spc = sector[13];
    uint16_t rsvd = rd16(sector + 14);
    uint8_t fats = sector[16];
    uint32_t total = rd32(sector + 32);
    uint32_t fatsz = rd32(sector + 36);
    uint32_t root = rd32(sector + 44);
    if (bps != 512 || spc != 1 || fats == 0 || root != FAT32_ROOT_CLUSTER) return -1;
    fat_lba = FAT32_BASE_LBA + rsvd;
    data_lba = fat_lba + fats * fatsz;
    cluster_count = total - (data_lba - FAT32_BASE_LBA);
    if (cluster_count > FAT32_MAX_CLUSTERS - 2) cluster_count = FAT32_MAX_CLUSTERS - 2;
    return 0;
}

static struct fat32_file *find_file83(const char name83[11]) {
    for (uint32_t i = 0; i < FAT32_MAX_FILES; i++)
        if (files[i].in_use && memcmp(files[i].name83, name83, 11) == 0) return &files[i];
    return 0;
}

static struct vnode *fat32_lookup(const char *path) {
    char n[11];
    if (to_name83(path, n) != 0) return 0;
    struct fat32_file *f = find_file83(n);
    if (!f) return 0;
    f->vnode.size = f->size;
    return &f->vnode;
}

static struct vnode *fat32_create(const char *path) {
    char n[11];
    if (to_name83(path, n) != 0) return 0;
    struct fat32_file *existing = find_file83(n);
    if (existing) return &existing->vnode;
    if (read_sector(cluster_to_lba(FAT32_ROOT_CLUSTER), sector) != 0) return 0;
    for (uint32_t i = 0; i < 16 && i < FAT32_MAX_FILES; i++) {
        uint8_t *e = sector + i * 32u;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            memset(e, 0, 32);
            memcpy(e, n, 11);
            e[11] = 0x20;
            write_sector(cluster_to_lba(FAT32_ROOT_CLUSTER), sector);
            rebuild_files();
            return fat32_lookup(path);
        }
    }
    return 0;
}

static int64_t fat32_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct fat32_file *f = (struct fat32_file *)vn->fs_data;
    if (!f || pos >= f->size) return 0;
    if (pos + count > f->size) count = f->size - pos;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t cl_index = (uint32_t)((pos + done) / AHCI_SECTOR_SIZE);
        uint32_t off = (uint32_t)((pos + done) % AHCI_SECTOR_SIZE);
        uint32_t cl = cluster_at(f, cl_index);
        if (cl < 2 || cl >= 0x0FFFFFF8) break;
        if (read_sector(cluster_to_lba(cl), sector) != 0) return -1;
        uint64_t chunk = AHCI_SECTOR_SIZE - off;
        if (chunk > count - done) chunk = count - done;
        memcpy(out + done, sector + off, chunk);
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t fat32_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct fat32_file *f = (struct fat32_file *)vn->fs_data;
    if (!f || count == 0) return count ? -1 : 0;
    uint64_t end = pos + count;
    if (end < pos) return -1;
    uint32_t clusters = (uint32_t)((end + AHCI_SECTOR_SIZE - 1) / AHCI_SECTOR_SIZE);
    if (ensure_chain(f, clusters) != 0) return -1;
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t cl_index = (uint32_t)((pos + done) / AHCI_SECTOR_SIZE);
        uint32_t off = (uint32_t)((pos + done) % AHCI_SECTOR_SIZE);
        uint32_t cl = cluster_at(f, cl_index);
        if (read_sector(cluster_to_lba(cl), sector) != 0) return -1;
        uint64_t chunk = AHCI_SECTOR_SIZE - off;
        if (chunk > count - done) chunk = count - done;
        memcpy(sector + off, in + done, chunk);
        if (write_sector(cluster_to_lba(cl), sector) != 0) return -1;
        done += chunk;
    }
    if (end > f->size) f->size = (uint32_t)end;
    f->vnode.size = f->size;
    if (update_dir_entry(f) != 0) return -1;
    return (int64_t)done;
}

const struct vfs_ops fat32_ops = {
    .lookup = fat32_lookup,
    .create = fat32_create,
    .read = fat32_read,
    .write = fat32_write,
};

int fat32_append_log(const char *data, uint64_t len) {
    if (!fat_ready || !data || len == 0) return -1;
    struct vnode *vn = fat32_lookup("AURALOG.TXT");
    if (!vn) vn = fat32_create("AURALOG.TXT");
    if (!vn) return -1;
    struct fat32_file *f = (struct fat32_file *)vn->fs_data;
    return fat32_write(vn, f->size, data, len) == (int64_t)len ? 0 : -1;
}

void fat32_list(void) {
    rebuild_files();
    for (uint32_t i = 0; i < FAT32_MAX_FILES; i++) {
        if (files[i].in_use) {
            kprintf("  /fat/%s  (%u bytes)\n", files[i].display, files[i].size);
        }
    }
}

int fat32_init(void) {
    fat_port = ahci_get_first_port();
    if (fat_port < 0) {
        kprintf("[fat32] no AHCI disk; FAT32 logging disabled\n");
        return -1;
    }
    if (parse_or_format() != 0) {
        kprintf("[fat32] failed to mount/format FAT32 volume\n");
        return -1;
    }
    rebuild_files();
    fat_ready = 1;
    kprintf("[fat32] mounted FAT32 volume at /fat (base LBA %u, %u clusters)\n",
            FAT32_BASE_LBA, cluster_count);
    klog_set_sink(fat32_append_log);
    klog_flush();
    kprintf("[fat32] persistent kernel log: /fat/AURALOG.TXT\n");
    return 0;
}

void fat32_self_test(void) {
    if (!fat_ready) return;
    kprintf("[fat32] self-test: create/write/read /fat/TEST.TXT...\n");
    struct vnode *vn = fat32_create("TEST.TXT");
    if (!vn) { kprintf("[fat32] FAIL: create failed\n"); return; }
    const char *msg = "hello fat32";
    if (fat32_write(vn, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[fat32] FAIL: write failed\n"); return;
    }
    char buf[32];
    memset(buf, 0, sizeof(buf));
    if (fat32_read(vn, 0, buf, sizeof(buf) - 1) != (int64_t)strlen(msg) || strcmp(buf, msg) != 0) {
        kprintf("[fat32] FAIL: readback mismatch '%s'\n", buf); return;
    }
    kprintf("[fat32] PASS: FAT32 read/write and log sink ready\n");
}
