/* test_exfat_ntfs.c — host unit test for the FSFULL F1 mount-refusal
 * paths of the exFAT and NTFS drivers.
 *
 * Compiles the REAL kernel/fs/exfat.c and kernel/fs/ntfs.c (not copies)
 * against stubs for kprintf/kmalloc and a RAM-backed bc_get.  Why a host
 * test at all: the NTFS slot is blkdev 6, which QEMU cannot reach with a
 * single 6-port AHCI controller, so the NTFS refusal path has no
 * integration lane — this is that lane.
 *
 * The safety property under test: a foreign or unreadable boot sector
 * makes *_init() return -1 (so the caller will NOT vfs_mount), while a
 * correct OEM signature is accepted.  Neither file contains a write path,
 * so "no writes happen on refusal" is structural here, not asserted.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/fs/buffer_cache.h"
#include "kernel/fs/exfat.h"
#include "kernel/fs/ntfs.h"
#include "kernel/lib/errno.h"

/* ---- stubs for the sources under test ---- */

int kprintf(const char *fmt, ...) { (void)fmt; return 0; }

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

void spinlock_init(spinlock_t *lock) { (void)lock; }

/* RAM-backed fake buffer cache.  The test fills fake_sector, then
 * bc_get returns a buffer carrying a copy of it (or NULL when
 * fake_read_fails is set). */
static uint8_t  fake_sector[BC_BLOCK_SIZE];
static struct buffer fake_buffer;
static int fake_read_fails = 0;

struct buffer *bc_get(uint32_t device_id, uint64_t block_num) {
    (void)device_id;
    (void)block_num;
    if (fake_read_fails) return NULL;
    memset(&fake_buffer, 0, sizeof(fake_buffer));
    memcpy(fake_buffer.data, fake_sector, BC_BLOCK_SIZE);
    return &fake_buffer;
}

void bc_release(struct buffer *buf) { (void)buf; }

/* F2 (FSFULL_PLAN.md): the drivers now read the boot region through the
 * shared cache-backed helpers, so the host lane provides them (mirroring
 * the real buffer_cache.c implementations over the same fake cache). */
int fs_read_block(int dev, uint64_t lba, uint32_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        struct buffer *b = bc_get((uint32_t)dev, lba + i);
        if (!b) return -1;
        memcpy(p, b->data, BC_BLOCK_SIZE);
        bc_release(b);
        p += BC_BLOCK_SIZE;
    }
    return 0;
}

int fs_write_block(int dev, uint64_t lba, uint32_t count, const void *buf) {
    (void)dev; (void)lba; (void)count; (void)buf;
    return 0;
}

int fs_cache_sync(void *fs_data) { (void)fs_data; return 0; }

/* F1 gate stubs: the format knob defaults OFF in the host lane, so a
 * foreign / blank boot sector is refused rather than auto-formatted. */
int fs_format_allowed(void) { return 0; }
void fs_format_set(int allowed, const char *source) { (void)allowed; (void)source; }
const char *fs_format_source(void) { return "host-test"; }

/* ---- test harness ---- */

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  PASS: %s\n", msg);                                     \
        } else {                                                             \
            printf("  FAIL: %s\n", msg);                                     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void clear_sector(void) {
    memset(fake_sector, 0x5A, sizeof(fake_sector));   /* foreign pattern */
    fake_read_fails = 0;
}

/* A geometrically plausible exFAT main boot sector, so the F5 driver can
 * parse it and mount (the old skeleton stopped at the OEM name). */
static void make_exfat_boot(void) {
    clear_sector();
    fake_sector[0] = 0xEB; fake_sector[1] = 0x76; fake_sector[2] = 0x90;
    memcpy(fake_sector + 3, "EXFAT   ", 8);
    uint64_t vol = 131072;
    memcpy(fake_sector + 72, &vol, 8);
    uint32_t fat_off = 128, fat_len = 128, heap = 256, nclu = 16320, root = 2;
    memcpy(fake_sector + 80, &fat_off, 4);
    memcpy(fake_sector + 84, &fat_len, 4);
    memcpy(fake_sector + 88, &heap, 4);
    memcpy(fake_sector + 92, &nclu, 4);
    memcpy(fake_sector + 96, &root, 4);
    fake_sector[108] = 9;   /* 512 bytes/sector */
    fake_sector[109] = 3;   /* 8 sectors/cluster = 4 KiB */
    fake_sector[110] = 1;
    fake_sector[510] = 0x55; fake_sector[511] = 0xAA;
}

/* A geometrically plausible NTFS main boot sector, so the F5b driver can
 * parse and mount it (the old skeleton stopped at the OEM name).  mft_lcn
 * points at a spot the RAM fake returns as the boot sector again; ntfs_init
 * tolerates that via its single-run $MFT fallback. */
static void make_ntfs_boot(void) {
    clear_sector();
    fake_sector[0] = 0xEB; fake_sector[1] = 0x52; fake_sector[2] = 0x90;
    memcpy(fake_sector + 3, "NTFS    ", 8);
    fake_sector[11] = 0x00; fake_sector[12] = 0x02;   /* 512 bytes/sector */
    fake_sector[13] = 8;                              /* 8 sectors/cluster */
    uint64_t total = 131072;
    memcpy(fake_sector + 0x28, &total, 8);
    uint64_t mft = 4;
    memcpy(fake_sector + 0x30, &mft, 8);
    memcpy(fake_sector + 0x38, &mft, 8);              /* mirror */
    fake_sector[0x40] = (uint8_t)-10;                 /* 1024-byte records */
    fake_sector[0x44] = (uint8_t)-12;                 /* 4096-byte index buf */
    fake_sector[510] = 0x55; fake_sector[511] = 0xAA;
}

int main(void) {
    printf("[fs-sigs] foreign boot sector is refused by both drivers\n");
    clear_sector();
    CHECK(exfat_init(2) != 0, "exfat_init refuses a foreign sector");
    CHECK(ntfs_init(6) != 0, "ntfs_init refuses a foreign sector");

    printf("[fs-sigs] unreadable device is refused by both drivers\n");
    clear_sector();
    fake_read_fails = 1;
    CHECK(exfat_init(2) != 0, "exfat_init refuses an unreadable sector");
    CHECK(ntfs_init(6) != 0, "ntfs_init refuses an unreadable sector");

    printf("[fs-sigs] correct OEM signatures are accepted\n");
    make_exfat_boot();
    CHECK(exfat_init(2) == 0, "exfat_init accepts the EXFAT OEM name");
    make_ntfs_boot();
    CHECK(ntfs_init(6) == 0, "ntfs_init accepts the NTFS OEM name");

    printf("[fs-sigs] NTFS mounts read-only: mutation ops refuse with -EROFS\n");
    {
        struct vnode vn;
        memset(&vn, 0, sizeof(vn));
        vn.ops = &ntfs_ops;
        vn.fs_data = NULL;
        /* Direct calls to the mutation ops must refuse, never pretend. */
        CHECK(ntfs_ops.write(&vn, 0, "x", 1) == -EROFS,
              "ntfs write refuses with -EROFS");
        CHECK(ntfs_ops.mkdir(NULL, "/x") == -EROFS,
              "ntfs mkdir refuses with -EROFS");
        CHECK(ntfs_ops.unlink(NULL, "/x") == -EROFS,
              "ntfs unlink refuses with -EROFS");
        CHECK(ntfs_ops.rmdir(NULL, "/x") == -EROFS,
              "ntfs rmdir refuses with -EROFS");
        CHECK(ntfs_ops.rename(NULL, "/a", "/b") == -EROFS,
              "ntfs rename refuses with -EROFS");
        CHECK(ntfs_ops.truncate(&vn, 0) == -EROFS,
              "ntfs truncate refuses with -EROFS");
        CHECK(ntfs_ops.settimes(&vn, 1, 2) == -EROFS,
              "ntfs settimes refuses with -EROFS");
    }

    printf("[fs-sigs] near-miss signatures are still refused\n");
    clear_sector();
    memcpy(fake_sector + 3, "EXFAT  ", 8);            /* 7 chars, not 8 */
    CHECK(exfat_init(2) != 0, "exfat_init refuses 'EXFAT  ' (short OEM)");
    clear_sector();
    memcpy(fake_sector + 3, "NTFS", 4);               /* short magic */
    CHECK(ntfs_init(6) != 0, "ntfs_init refuses a truncated OEM");

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", failures);
    return 1;
}
