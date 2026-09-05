/* tests/unit/test_fscheck.c — host gate for the fscheck walkers
 * (RESIDUE2 T3: fsck tooling, CHECKS first).
 *
 * The SAME kernel/fs/fscheck.c object the kernel links is fed crafted
 * FAT32 and ext2 images through a RAM blkdev; each walker must report
 * zero findings on the clean volume and the NAMED finding for every
 * corruption this test injects.  The finding text is captured from the
 * kprintf stub so the integration case's greps and this test pin the
 * exact same strings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "kernel/fs/blkdev.h"
#include "kernel/fs/fscheck.h"

/* ---- host stubs for the kernel utilities fscheck links against ---- */

static char capture[65536];
static size_t capture_len;

void kprintf(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0 && capture_len + (size_t)n < sizeof(capture) - 1) {
        memcpy(capture + capture_len, tmp, (size_t)n);
        capture_len += (size_t)n;
        capture[capture_len] = '\0';
    }
}

void *kmalloc(uint64_t size) { return malloc((size_t)size); }
void  kfree(void *p)         { free(p); }

static void capture_reset(void) {
    capture_len = 0;
    capture[0] = '\0';
}
static int captured(const char *needle) {
    return strstr(capture, needle) != NULL;
}

/* ---- RAM-backed device (16 MiB) ------------------------------------- */

#define RAM_SECTORS (32768)
static uint8_t *disk;

static int ram_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (lba + count > RAM_SECTORS) return -1;
    memcpy(buf, disk + lba * BLKDEV_SECTOR_SIZE,
           (size_t)count * BLKDEV_SECTOR_SIZE);
    return 0;
}
static int ram_write(void *ctx, uint64_t lba, uint32_t count,
                     const void *buf) {
    (void)ctx;
    if (lba + count > RAM_SECTORS) return -1;
    memcpy(disk + lba * BLKDEV_SECTOR_SIZE, buf,
           (size_t)count * BLKDEV_SECTOR_SIZE);
    return 0;
}
static const struct blkdev_ops ram_ops = {
    .read = ram_read, .write = ram_write, .sector_count = 0,
};

/* ---- little-endian pokers ------------------------------------------- */

static void w16(uint64_t off, uint16_t v) {
    disk[off] = (uint8_t)v; disk[off + 1] = (uint8_t)(v >> 8);
}
static void w32(uint64_t off, uint32_t v) {
    disk[off] = (uint8_t)v; disk[off + 1] = (uint8_t)(v >> 8);
    disk[off + 2] = (uint8_t)(v >> 16); disk[off + 3] = (uint8_t)(v >> 24);
}

/* ========================================================================
 * FAT32 image (volume at LBA 0 for the host lane)
 *
 *   8192 sectors (4 MiB), spc=8 (4 KiB clusters), reserved=32, 2 FATs
 *   of 64 sectors each → 1004 data clusters (numbered 2..1005).
 *   Root dir = cluster 2; one file HELLO.TXT (5 bytes) = cluster 3.
 * ======================================================================== */

#define F_TOTAL   8192u
#define F_RES     32u
#define F_NFATS   2u
#define F_FATSZ   64u
#define F_SPC     8u
#define F_FAT_LBA (F_RES)
#define F_DATA_LBA (F_RES + F_NFATS * F_FATSZ)
#define F_NCLUST  ((F_TOTAL - F_DATA_LBA) / F_SPC)
#define F_USED    2u                       /* clusters 2 and 3 */

static void fat_set(uint32_t cl, uint32_t v) {
    uint64_t off = (uint64_t)(F_FAT_LBA * 512u) + (uint64_t)cl * 4u;
    w32(off, v);
    w32(off + F_FATSZ * 512u, v);          /* mirror in FAT #2 */
}

static void make_fat32(void) {
    memset(disk, 0, (size_t)RAM_SECTORS * BLKDEV_SECTOR_SIZE);

    /* BPB */
    uint64_t b = 0;
    memcpy(disk + b + 3, "MSWIN4.1", 8);
    w16(b + 11, 512);
    disk[b + 13] = F_SPC;
    w16(b + 14, F_RES);
    disk[b + 16] = F_NFATS;
    w32(b + 32, F_TOTAL);
    w32(b + 36, F_FATSZ);
    w32(b + 44, 2);                        /* root cluster */
    w16(b + 48, 1);                        /* fsinfo sector */
    memcpy(disk + b + 82, "FAT32   ", 8);
    disk[b + 510] = 0x55; disk[b + 511] = 0xAA;

    /* FSInfo */
    uint64_t f = 512;
    w32(f + 0, 0x41615252u);
    w32(f + 484, 0x61417272u);
    w32(f + 488, F_NCLUST - F_USED);       /* free count */
    w32(f + 492, 4);
    w32(f + 508, 0xAA550000u);

    /* FATs */
    fat_set(0, 0x0FFFFFF8u);
    fat_set(1, 0x0FFFFFFFu);
    fat_set(2, 0x0FFFFFF8u);               /* root dir, one cluster */
    fat_set(3, 0x0FFFFFF8u);               /* HELLO.TXT, one cluster */

    /* Root directory (cluster 2) */
    uint64_t root = (uint64_t)(F_DATA_LBA + (2 - 2) * F_SPC) * 512u;
    memcpy(disk + root + 0, "HELLO   TXT", 11);
    disk[root + 11] = 0x20;                /* archive */
    w16(root + 20, 0);                     /* start hi */
    w16(root + 26, 3);                     /* start lo */
    w32(root + 28, 5);                     /* size */
    /* next entry: end marker already zero */

    /* File data (cluster 3) */
    uint64_t data = (uint64_t)(F_DATA_LBA + (3 - 2) * F_SPC) * 512u;
    memcpy(disk + data, "hello", 5);
}

/* ========================================================================
 * ext2 image (volume at LBA 0)
 *
 *   1 MiB, 1024-byte blocks, 1024 blocks, 64 inodes, one group.
 *   Layout (block numbers):
 *     1  superblock          2  group descriptor table
 *     3  block bitmap        4  inode bitmap
 *     5..12 inode table (64 * 128 B = 8 blocks)
 *     13 root directory data block
 * ======================================================================== */

#define E_BLOCKS      1024u
#define E_BLOCK_SIZE  1024u
#define E_SPB         (E_BLOCK_SIZE / 512u)
#define E_USED_BLOCKS 13u                  /* blocks 1..13 */
#define E_USED_INODES 10u                  /* reserved inodes 1..10 (root is 2) */
#define E_ITABLE_BLK  5u
#define E_ITABLE_BLKS 8u

static void e_blk(uint32_t block, const uint8_t *src) {
    memcpy(disk + (uint64_t)block * E_BLOCK_SIZE, src, E_BLOCK_SIZE);
}

static void make_ext2(void) {
    memset(disk, 0, (size_t)RAM_SECTORS * BLKDEV_SECTOR_SIZE);

    /* Superblock at byte 1024 (block 1). */
    uint8_t sb[E_BLOCK_SIZE] = {0};
    #define SB32(off, v) do { uint32_t x = (v); \
        sb[(off)] = x & 0xFF; sb[(off)+1] = (x>>8) & 0xFF; \
        sb[(off)+2] = (x>>16) & 0xFF; sb[(off)+3] = (x>>24) & 0xFF; \
    } while (0)
    #define SB16(off, v) do { uint16_t x = (v); \
        sb[(off)] = x & 0xFF; sb[(off)+1] = (x>>8) & 0xFF; } while (0)
    SB32(0,  64);                       /* inodes_count */
    SB32(4,  E_BLOCKS);                 /* blocks_count */
    SB32(12, (E_BLOCKS - 1) - E_USED_BLOCKS);  /* free blocks (blk 0 n/a) */
    SB32(16, 64 - E_USED_INODES);       /* free inodes */
    SB32(20, 1);                        /* first_data_block */
    SB32(24, 0);                        /* log_block_size (1 KiB) */
    SB32(32, 8192);                     /* blocks_per_group */
    SB32(40, 64);                       /* inodes_per_group */
    SB16(56, 0xEF53);                   /* magic */
    SB16(58, 1);                        /* state: clean */
    SB16(88, 128);                      /* inode size */
    e_blk(1, sb);

    /* Group descriptor table (block 2). */
    uint8_t gdt[E_BLOCK_SIZE] = {0};
    gdt[0] = 3;                          /* block bitmap block */
    gdt[4] = 4;                          /* inode bitmap block */
    gdt[8] = E_ITABLE_BLK;               /* inode table block */
    uint16_t fb = (uint16_t)((E_BLOCKS - 1) - E_USED_BLOCKS);
    uint16_t fi = (uint16_t)(64 - E_USED_INODES);
    gdt[12] = fb & 0xFF; gdt[13] = fb >> 8;
    gdt[14] = fi & 0xFF; gdt[15] = fi >> 8;
    e_blk(2, gdt);

    /* Block bitmap (block 3): blocks 1..13 used → bits 0..12
     * (bit i covers block first_data_block + i). */
    uint8_t bbm[E_BLOCK_SIZE] = {0};
    bbm[0] = 0xFF;                       /* blocks 1..8 */
    bbm[1] = 0x1F;                       /* blocks 9..13 */
    e_blk(3, bbm);

    /* Inode bitmap (block 4): inodes 1..10 → bits 0..9 (root is 2). */
    uint8_t ibm[E_BLOCK_SIZE] = {0};
    ibm[0] = 0xFF;
    ibm[1] = 0x03;
    e_blk(4, ibm);

    /* Inode table: inode 2 (index 1) = root directory. */
    uint8_t it[E_BLOCK_SIZE] = {0};
    uint8_t *ino = it + 128;             /* inode 2 */
    ino[0] = 0xED; ino[1] = 0x41;        /* mode: dir 0755 */
    ino[26] = 3;                         /* links_count */
    uint32_t blk0 = 13;
    ino[40] = blk0 & 0xFF; ino[41] = (blk0 >> 8) & 0xFF;
    /* Inode table spans 8 blocks; write each. */
    for (uint32_t i = 0; i < E_ITABLE_BLKS; i++) {
        memset(disk + (uint64_t)(E_ITABLE_BLK + i) * E_BLOCK_SIZE, 0,
               E_BLOCK_SIZE);
    }
    memcpy(disk + (uint64_t)E_ITABLE_BLK * E_BLOCK_SIZE, it, E_BLOCK_SIZE);
    /* The rest of inode 2's 128 bytes sit in the same first block. */

    /* Root directory data block 13 (contents not parsed by fscheck). */
}

/* ======================================================================== */

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n    capture: %.400s\n", msg, capture); \
                failures++; } \
} while (0)

int main(void) {
    disk = malloc((size_t)RAM_SECTORS * BLKDEV_SECTOR_SIZE);
    if (!disk) return 1;
    int dev = blkdev_register("ram0", &ram_ops, 0, BLKDEV_SECTOR_SIZE);
    if (dev < 0) { printf("FAIL: blkdev_register\n"); return 1; }

    /* ---------------- FAT32 ---------------- */
    printf("== fscheck_fat32 ==\n");

    make_fat32();
    capture_reset();
    int n = fscheck_fat32(dev, 0);
    CHECK(n == 0, "clean FAT32 volume: zero findings");
    CHECK(captured("[fscheck] fat32: CLEAN"), "clean volume prints CLEAN");

    /* FSInfo free-count drift */
    make_fat32();
    w32(512 + 488, F_NCLUST - F_USED + 5);
    capture_reset();
    n = fscheck_fat32(dev, 0);
    CHECK(n == 1 && captured("FSInfo free count"),
          "FSInfo free-count drift detected");

    /* Entry start cluster out of range */
    make_fat32();
    w16((uint64_t)(F_DATA_LBA * 512u) + 26, 4000);   /* HELLO.TXT start */
    capture_reset();
    n = fscheck_fat32(dev, 0);
    CHECK(n >= 1 && captured("entry start cluster 4000 out of range"),
          "out-of-range entry start detected");

    /* Cross-link: file chain points back into the root chain */
    make_fat32();
    fat_set(3, 2);
    capture_reset();
    n = fscheck_fat32(dev, 0);
    CHECK(n >= 1 && captured("cross-link or loop"),
          "cross-linked chain detected");

    /* Size/chain mismatch: 10000 bytes need 3 four-KiB clusters */
    make_fat32();
    w32((uint64_t)(F_DATA_LBA * 512u) + 28, 10000);
    capture_reset();
    n = fscheck_fat32(dev, 0);
    CHECK(n >= 1 && captured("chain length 1 != size-implied 3"),
          "size/chain mismatch detected");

    /* Reserved FAT entries broken */
    make_fat32();
    fat_set(1, 0);
    capture_reset();
    n = fscheck_fat32(dev, 0);
    CHECK(n >= 1 && captured("reserved entries bad"),
          "broken FAT[0]/FAT[1] detected");

    /* Not FAT32 at all */
    make_fat32();
    disk[82] = 'X';
    capture_reset();
    n = fscheck_fat32(dev, 0);
    CHECK(n == -1 && captured("not a FAT32 boot sector"),
          "foreign boot sector refused");

    /* ---------------- ext2 ---------------- */
    printf("== fscheck_ext2 ==\n");

    make_ext2();
    capture_reset();
    n = fscheck_ext2(dev, 0);
    CHECK(n == 0, "clean ext2 volume: zero findings");
    CHECK(captured("[fscheck] ext2: CLEAN"), "clean volume prints CLEAN");

    /* Superblock free-block count drift */
    make_ext2();
    w32((uint64_t)1 * E_BLOCK_SIZE + 12,
        ((E_BLOCKS - 1) - E_USED_BLOCKS) + 7);
    capture_reset();
    n = fscheck_ext2(dev, 0);
    CHECK(n == 1 && captured("superblock free blocks"),
          "superblock free-block drift detected");

    /* Group descriptor free-block count drift */
    make_ext2();
    {
        uint64_t gdt_off = (uint64_t)2 * E_BLOCK_SIZE + 12;
        uint16_t fb = (uint16_t)((E_BLOCKS - 1) - E_USED_BLOCKS);
        disk[gdt_off] = (uint8_t)((fb + 1) & 0xFF);
        disk[gdt_off + 1] = (uint8_t)((fb + 1) >> 8);
    }
    capture_reset();
    n = fscheck_ext2(dev, 0);
    CHECK(n >= 1 && captured("free blocks gd=") && captured("!= bitmap"),
          "group free-block drift detected");

    /* Root inode's bitmap bit cleared */
    make_ext2();
    disk[(uint64_t)4 * E_BLOCK_SIZE + 0] &= ~0x02;   /* inode 2 = bit 1 */
    capture_reset();
    n = fscheck_ext2(dev, 0);
    CHECK(n >= 1 && captured("inode 2 in use but bitmap says free"),
          "inode/bitmap disagreement detected");

    /* Magic broken */
    make_ext2();
    disk[(uint64_t)1 * E_BLOCK_SIZE + 56] = 0x00;
    capture_reset();
    n = fscheck_ext2(dev, 0);
    CHECK(n == -1 && captured("superblock magic"),
          "broken ext2 magic refused");

    /* Volume offset: the FAT32 superfloppy sits at LBA 64 in-kernel —
     * prove the walker honours base_lba. */
    make_fat32();
    memmove(disk + 64 * 512, disk, (size_t)(RAM_SECTORS - 64) * 512);
    memset(disk, 0, 64 * 512);
    capture_reset();
    n = fscheck_fat32(dev, 64);
    CHECK(n == 0 && captured("CLEAN"),
          "base_lba 64 (the superfloppy offset) honoured");

    free(disk);
    if (failures) {
        printf("[fscheck] %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("[fscheck] all consistency-walker checks passed\n");
    return 0;
}
