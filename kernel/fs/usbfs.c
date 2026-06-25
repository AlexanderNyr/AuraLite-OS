/* usbfs.c — VFS view of the active USB Mass Storage device.
 *
 * Files:
 *   /usb/info        text metadata + status
 *   /usb/sector0.bin first 512-byte sector
 *   /usb/disk.img    raw block device image (read/write)
 */

#include <stdint.h>
#include "kernel/fs/usbfs.h"
#include "drivers/usb/msc.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define USBFS_INO_ROOT    1
#define USBFS_INO_INFO    2
#define USBFS_INO_SECTOR0 3
#define USBFS_INO_DISK    4
#define USBFS_INO_FAT_DIR 5
#define USBFS_INO_FAT_FILE_BASE 100

static struct vnode usbfs_nodes[16];
static int usbfs_ready = 0;
static uint32_t usbfs_generation = 0;
static uint8_t sector_scratch[MSC_SECTOR_SIZE];

struct usb_fat_state {
    int detected;
    uint32_t base_lba;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved;
    uint8_t fats;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t fat_lba;
    uint32_t data_lba;
    uint32_t cluster_count;
};
struct usb_fat_file {
    int in_use;
    char name[VFS_PATH_MAX];
    uint32_t first_cluster;
    uint32_t size;
    uint8_t is_dir;
    uint64_t ino;
};
static struct usb_fat_state fat;
static struct usb_fat_file fat_files[32];

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t fat_cluster_lba(uint32_t cl) { return fat.data_lba + (cl - 2u) * fat.sectors_per_cluster; }

static void u64_to_dec(char *buf, uint64_t v) {
    char tmp[32];
    int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int p = 0;
    while (n) buf[p++] = tmp[--n];
    buf[p] = 0;
}

static void append(char *out, uint32_t *pos, const char *s) {
    while (*s && *pos < 1023) out[(*pos)++] = *s++;
    out[*pos] = 0;
}

static uint32_t make_info(char *out) {
    uint32_t p = 0;
    char num[32];
    append(out, &p, "AuraLite usbfs\n");
    append(out, &p, "status: ");
    append(out, &p, msc_is_present() ? "ready\n" : "no-device\n");
    append(out, &p, "generation: ");
    u64_to_dec(num, usbfs_generation); append(out, &p, num); append(out, &p, "\n");
    append(out, &p, "sector_size: ");
    u64_to_dec(num, MSC_SECTOR_SIZE); append(out, &p, num); append(out, &p, "\n");
    append(out, &p, "sectors: ");
    u64_to_dec(num, msc_get_sector_count()); append(out, &p, num); append(out, &p, "\n");
    append(out, &p, "bytes: ");
    u64_to_dec(num, (uint64_t)msc_get_sector_count() * MSC_SECTOR_SIZE); append(out, &p, num); append(out, &p, "\n");
    append(out, &p, "fat32: "); append(out, &p, fat.detected ? "detected\n" : "not-detected\n");
    append(out, &p, "files: info sector0.bin disk.img"); append(out, &p, fat.detected ? " fat/\n" : "\n");
    return p;
}

static int fat_read_sector(uint32_t lba, uint8_t *buf) {
    return msc_read(lba, 1, buf);
}

static int fat_signature_at(uint32_t base) {
    if (fat_read_sector(base, sector_scratch) != 0) return 0;
    if (sector_scratch[510] != 0x55 || sector_scratch[511] != 0xAA) return 0;
    if (sector_scratch[82] == 'F' && sector_scratch[83] == 'A' &&
        sector_scratch[84] == 'T' && sector_scratch[85] == '3' &&
        sector_scratch[86] == '2') return 1;
    return (rd16(sector_scratch + 11) == 512 && sector_scratch[13] != 0 &&
            rd32(sector_scratch + 36) != 0 && rd32(sector_scratch + 44) >= 2);
}

static void usbfs_probe_fat32(void) {
    memset(&fat, 0, sizeof(fat));
    memset(fat_files, 0, sizeof(fat_files));
    if (!msc_is_present()) return;
    uint32_t base = 0;
    if (!fat_signature_at(0)) {
        if (fat_read_sector(0, sector_scratch) != 0) return;
        for (int i = 0; i < 4; i++) {
            uint8_t *pe = sector_scratch + 446 + i * 16;
            uint8_t type = pe[4];
            if (type == 0x0B || type == 0x0C || type == 0x1B || type == 0x1C) {
                uint32_t lba = rd32(pe + 8);
                if (fat_signature_at(lba)) { base = lba; break; }
            }
        }
        if (base == 0 && !fat_signature_at(0)) return;
    }
    if (fat_read_sector(base, sector_scratch) != 0) return;
    fat.base_lba = base;
    fat.bytes_per_sector = rd16(sector_scratch + 11);
    fat.sectors_per_cluster = sector_scratch[13];
    fat.reserved = rd16(sector_scratch + 14);
    fat.fats = sector_scratch[16];
    uint16_t total16 = rd16(sector_scratch + 19);
    fat.total_sectors = total16 ? total16 : rd32(sector_scratch + 32);
    fat.fat_sectors = rd32(sector_scratch + 36);
    fat.root_cluster = rd32(sector_scratch + 44);
    if (fat.bytes_per_sector != 512 || fat.sectors_per_cluster == 0 || fat.fats == 0 ||
        fat.fat_sectors == 0 || fat.root_cluster < 2) {
        memset(&fat, 0, sizeof(fat));
        return;
    }
    fat.fat_lba = fat.base_lba + fat.reserved;
    fat.data_lba = fat.fat_lba + (uint32_t)fat.fats * fat.fat_sectors;
    fat.cluster_count = (fat.total_sectors > (fat.data_lba - fat.base_lba)) ?
        (fat.total_sectors - (fat.data_lba - fat.base_lba)) / fat.sectors_per_cluster : 0;
    fat.detected = 1;
    kprintf("[usbfs] FAT32 detected: base=%u root_cluster=%u clusters=%u\n",
            fat.base_lba, fat.root_cluster, fat.cluster_count);
}

static uint32_t fat_get(uint32_t cl) {
    if (!fat.detected || cl < 2) return 0x0FFFFFFF;
    uint32_t off = cl * 4u;
    if (fat_read_sector(fat.fat_lba + off / 512u, sector_scratch) != 0) return 0x0FFFFFFF;
    return rd32(sector_scratch + (off % 512u)) & 0x0FFFFFFF;
}

static void fat_name_83(const uint8_t *e, char *out) {
    int p = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[p++] = (char)e[i];
    if (e[8] != ' ') {
        out[p++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[p++] = (char)e[i];
    }
    out[p] = 0;
}
static int name_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}
static void lfn_part(const uint8_t *e, char *tmp) {
    static const uint8_t offs[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    int p = 0;
    for (int i = 0; i < 13; i++) {
        uint16_t ch = rd16(e + offs[i]);
        if (ch == 0x0000 || ch == 0xFFFF) break;
        tmp[p++] = (ch < 128) ? (char)ch : '?';
    }
    tmp[p] = 0;
}
static void prepend(char *dst, const char *src) {
    char old[VFS_PATH_MAX];
    strncpy(old, dst, VFS_PATH_MAX - 1); old[VFS_PATH_MAX - 1] = 0;
    strncpy(dst, src, VFS_PATH_MAX - 1); dst[VFS_PATH_MAX - 1] = 0;
    size_t p = strlen(dst);
    for (size_t i = 0; old[i] && p + 1 < VFS_PATH_MAX; i++) dst[p++] = old[i];
    dst[p] = 0;
}

static int fat_scan_root(struct usb_fat_file *out, int max, const char *find) {
    if (!fat.detected) return -1;
    uint32_t cl = fat.root_cluster;
    int n = 0;
    char lfn[VFS_PATH_MAX];
    lfn[0] = 0;
    while (cl >= 2 && cl < 0x0FFFFFF8u) {
        for (uint8_t s = 0; s < fat.sectors_per_cluster; s++) {
            if (fat_read_sector(fat_cluster_lba(cl) + s, sector_scratch) != 0) return -1;
            for (int off = 0; off < 512; off += 32) {
                uint8_t *e = sector_scratch + off;
                if (e[0] == 0x00) return n;
                if (e[0] == 0xE5) { lfn[0] = 0; continue; }
                uint8_t attr = e[11];
                if (attr == 0x0F) { char part[32]; lfn_part(e, part); prepend(lfn, part); continue; }
                if (attr & 0x08) { lfn[0] = 0; continue; }
                char nm[VFS_PATH_MAX];
                if (lfn[0]) strncpy(nm, lfn, VFS_PATH_MAX - 1);
                else fat_name_83(e, nm);
                nm[VFS_PATH_MAX - 1] = 0;
                lfn[0] = 0;
                uint32_t first = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                uint32_t size = rd32(e + 28);
                uint8_t is_dir = (attr & 0x10) ? 1 : 0;
                if (find && name_eq_ci(nm, find)) {
                    memset(out, 0, sizeof(*out));
                    strncpy(out->name, nm, VFS_PATH_MAX - 1);
                    out->first_cluster = first; out->size = size; out->is_dir = is_dir;
                    out->ino = USBFS_INO_FAT_FILE_BASE + first;
                    return 1;
                }
                if (!find && n < max) {
                    out[n].in_use = 1;
                    strncpy(out[n].name, nm, VFS_PATH_MAX - 1);
                    out[n].first_cluster = first; out[n].size = size; out[n].is_dir = is_dir;
                    out[n].ino = USBFS_INO_FAT_FILE_BASE + first;
                    n++;
                }
            }
        }
        cl = fat_get(cl);
    }
    return n;
}

static struct vnode *node(uint32_t idx, const char *name, uint32_t type, uint64_t size, uint64_t ino) {
    struct vnode *vn = &usbfs_nodes[idx];
    memset(vn, 0, sizeof(*vn));
    strncpy(vn->name, name, VFS_PATH_MAX - 1);
    vn->type = type;
    vn->mode = (type == VFS_TYPE_DIR) ? 0755 : 0644;
    vn->size = size;
    vn->ops = &usbfs_ops;
    vn->inode_id = ino;
    return vn;
}

static struct vnode *usbfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    uint64_t disk_size = (uint64_t)msc_get_sector_count() * MSC_SECTOR_SIZE;
    if (!path || path[0] == 0) return node(0, "usb", VFS_TYPE_DIR, 0, USBFS_INO_ROOT);
    if (strcmp(path, "info") == 0) return node(1, "info", VFS_TYPE_FILE, 256, USBFS_INO_INFO);
    if (strcmp(path, "sector0.bin") == 0) return node(2, "sector0.bin", VFS_TYPE_FILE, msc_is_present() ? MSC_SECTOR_SIZE : 0, USBFS_INO_SECTOR0);
    if (strcmp(path, "disk.img") == 0) return node(3, "disk.img", VFS_TYPE_FILE, msc_is_present() ? disk_size : 0, USBFS_INO_DISK);
    if (strcmp(path, "fat") == 0 && fat.detected) return node(4, "fat", VFS_TYPE_DIR, 0, USBFS_INO_FAT_DIR);
    if (fat.detected && path[0]=='f' && path[1]=='a' && path[2]=='t' && path[3]=='/') {
        const char *name = path + 4;
        struct usb_fat_file *fi = &fat_files[0];
        if (fat_scan_root(fi, 1, name) > 0) {
            struct vnode *vn = node(5, fi->name, fi->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE, fi->size, fi->ino);
            fi->in_use = 1;
            vn->fs_data = fi;
            return vn;
        }
    }
    return NULL;
}

static int usbfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    if (!out || max < 1) return -1;
    if (vn && vn->inode_id == USBFS_INO_FAT_DIR) {
        int n = fat_scan_root(fat_files, 32, NULL);
        if (n < 0) return -1;
        if (n > max) n = max;
        for (int i = 0; i < n; i++) {
            strncpy(out[i].name, fat_files[i].name, VFS_PATH_MAX - 1);
            out[i].type = fat_files[i].is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            out[i].size = fat_files[i].size;
            out[i].inode = fat_files[i].ino;
        }
        return n;
    }
    int n = 0;
    strncpy(out[n].name, "info", VFS_PATH_MAX - 1);
    out[n].type = VFS_TYPE_FILE; out[n].size = 256; out[n].inode = USBFS_INO_INFO; n++;
    if (msc_is_present() && n < max) {
        strncpy(out[n].name, "sector0.bin", VFS_PATH_MAX - 1);
        out[n].type = VFS_TYPE_FILE; out[n].size = MSC_SECTOR_SIZE; out[n].inode = USBFS_INO_SECTOR0; n++;
    }
    if (msc_is_present() && n < max) {
        strncpy(out[n].name, "disk.img", VFS_PATH_MAX - 1);
        out[n].type = VFS_TYPE_FILE;
        out[n].size = (uint64_t)msc_get_sector_count() * MSC_SECTOR_SIZE;
        out[n].inode = USBFS_INO_DISK; n++;
    }
    if (fat.detected && n < max) {
        strncpy(out[n].name, "fat", VFS_PATH_MAX - 1);
        out[n].type = VFS_TYPE_DIR; out[n].size = 0; out[n].inode = USBFS_INO_FAT_DIR; n++;
    }
    return n;
}

static int64_t read_raw(uint64_t pos, void *buf, uint64_t count, uint64_t limit) {
    if (!msc_is_present() || !buf || pos >= limit) return 0;
    if (pos + count > limit) count = limit - pos;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t abs = pos + done;
        uint64_t lba = abs / MSC_SECTOR_SIZE;
        uint32_t off = (uint32_t)(abs % MSC_SECTOR_SIZE);
        if (msc_read(lba, 1, sector_scratch) != 0) break;
        uint32_t n = MSC_SECTOR_SIZE - off;
        if (n > count - done) n = (uint32_t)(count - done);
        memcpy(out + done, sector_scratch + off, n);
        done += n;
    }
    return (int64_t)done;
}

static int64_t write_raw(uint64_t pos, const void *buf, uint64_t count, uint64_t limit) {
    if (!msc_is_present() || !buf || pos >= limit) return 0;
    if (pos + count > limit) count = limit - pos;
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t abs = pos + done;
        uint64_t lba = abs / MSC_SECTOR_SIZE;
        uint32_t off = (uint32_t)(abs % MSC_SECTOR_SIZE);
        uint32_t n = MSC_SECTOR_SIZE - off;
        if (n > count - done) n = (uint32_t)(count - done);
        if (off != 0 || n != MSC_SECTOR_SIZE) {
            if (msc_read(lba, 1, sector_scratch) != 0) break;
        } else {
            memset(sector_scratch, 0, MSC_SECTOR_SIZE);
        }
        memcpy(sector_scratch + off, in + done, n);
        if (msc_write(lba, 1, sector_scratch) != 0) break;
        done += n;
    }
    return (int64_t)done;
}

static int64_t read_fat_file(struct usb_fat_file *fi, uint64_t pos, void *buf, uint64_t count) {
    if (!fat.detected || !fi || fi->is_dir || pos >= fi->size) return 0;
    if (pos + count > fi->size) count = fi->size - pos;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    uint32_t cl = fi->first_cluster;
    uint64_t skip = pos;
    uint32_t cl_bytes = (uint32_t)fat.sectors_per_cluster * MSC_SECTOR_SIZE;
    while (skip >= cl_bytes && cl >= 2 && cl < 0x0FFFFFF8u) { cl = fat_get(cl); skip -= cl_bytes; }
    while (done < count && cl >= 2 && cl < 0x0FFFFFF8u) {
        for (uint8_t s = 0; s < fat.sectors_per_cluster && done < count; s++) {
            if (fat_read_sector(fat_cluster_lba(cl) + s, sector_scratch) != 0) return (int64_t)done;
            uint32_t off = 0;
            if (skip) { off = (skip >= MSC_SECTOR_SIZE) ? MSC_SECTOR_SIZE : (uint32_t)skip; skip -= off; }
            if (off >= MSC_SECTOR_SIZE) continue;
            uint32_t n = MSC_SECTOR_SIZE - off;
            if (n > count - done) n = (uint32_t)(count - done);
            memcpy(out + done, sector_scratch + off, n);
            done += n;
        }
        cl = fat_get(cl);
    }
    return (int64_t)done;
}

static int64_t usbfs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    if (!vn || !buf) return -1;
    if (vn->inode_id == USBFS_INO_INFO) {
        char info[1024];
        uint32_t len = make_info(info);
        if (pos >= len) return 0;
        if (pos + count > len) count = len - pos;
        memcpy(buf, info + pos, count);
        return (int64_t)count;
    }
    if (vn->inode_id == USBFS_INO_SECTOR0) return read_raw(pos, buf, count, MSC_SECTOR_SIZE);
    if (vn->inode_id == USBFS_INO_DISK) return read_raw(pos, buf, count, (uint64_t)msc_get_sector_count() * MSC_SECTOR_SIZE);
    if (vn->inode_id >= USBFS_INO_FAT_FILE_BASE) return read_fat_file((struct usb_fat_file *)vn->fs_data, pos, buf, count);
    return -1;
}

static int64_t usbfs_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    if (!vn || !buf) return -1;
    if (vn->inode_id == USBFS_INO_SECTOR0) return write_raw(pos, buf, count, MSC_SECTOR_SIZE);
    if (vn->inode_id == USBFS_INO_DISK) return write_raw(pos, buf, count, (uint64_t)msc_get_sector_count() * MSC_SECTOR_SIZE);
    return -1;
}

static int usbfs_stat(struct vnode *vn, struct vfs_stat *out) {
    if (!vn || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->type = vn->type;
    out->mode = vn->mode;
    out->size = vn->size;
    if (vn->inode_id == USBFS_INO_DISK) out->size = (uint64_t)msc_get_sector_count() * MSC_SECTOR_SIZE;
    if (vn->inode_id == USBFS_INO_SECTOR0) out->size = msc_is_present() ? MSC_SECTOR_SIZE : 0;
    if (vn->inode_id >= USBFS_INO_FAT_FILE_BASE && vn->fs_data) out->size = ((struct usb_fat_file *)vn->fs_data)->size;
    out->inode = vn->inode_id;
    out->nlink = 1;
    out->blocks = (uint32_t)((out->size + 511) / 512);
    return 0;
}

const struct vfs_ops usbfs_ops = {
    .lookup = usbfs_lookup,
    .read = usbfs_read,
    .write = usbfs_write,
    .readdir = usbfs_readdir,
    .stat = usbfs_stat,
};

void usbfs_init(void) {
    if (!usbfs_ready) {
        usbfs_ready = 1;
        usbfs_generation = 0;
        kprintf("[usbfs] ready: mount at /usb for hotplug USB mass storage\n");
    }
}

void usbfs_notify_attach(void) {
    usbfs_generation++;
    usbfs_probe_fat32();
    kprintf("[usbfs] device available at /usb (info, sector0.bin, disk.img%s)\n",
            fat.detected ? ", fat/" : "");
}

void usbfs_notify_detach(void) {
    usbfs_generation++;
    memset(&fat, 0, sizeof(fat));
    memset(fat_files, 0, sizeof(fat_files));
    kprintf("[usbfs] device removed from /usb\n");
}
