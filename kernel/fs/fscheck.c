/* kernel/fs/fscheck.c — read-only FAT32/ext2 consistency walkers
 * (RESIDUE2 T3).
 *
 * Scope, held to the plan's design rule: CHECKS first, repair second.
 * Nothing in this file writes to the volume.  Every inconsistency is a
 * named, greppable line; the integration case and the host unit test
 * pin the names, so a walker that silently stopped noticing corruption
 * turns CI red.
 *
 * Walkers (pure read-only, through the blkdev seam, no driver state):
 *
 *   fscheck_fat32(dev)
 *     - BPB sanity (512 B/sector, power-of-two cluster size, FAT geometry
 *       fits the volume, root cluster in range);
 *     - FSInfo signatures and its free-count hint;
 *     - one full FAT scan: reserved entries, cluster bounds, free count;
 *     - directory-chain walk from the root cluster: every active entry's
 *       start cluster is in range, its chain terminates, chains never
 *       loop or share clusters (cross-link), and a file's chain length
 *       matches ceil(size / cluster-bytes).
 *
 *   fscheck_ext2(dev)
 *     - superblock sanity (magic, block size, group geometry);
 *     - every group descriptor's bitmap/inode-table blocks in range;
 *     - per-group free block/inode counts vs bitmap popcounts;
 *     - inode-table scan: in-use inodes are marked in the inode bitmap
 *       and vice versa; the root inode exists and is a directory.
 *
 * Host test: tests/unit/test_fscheck.c feeds crafted + corrupted images
 * through a RAM blkdev and pins each finding class.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/fs/fscheck.h"
#include "kernel/fs/blkdev.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/kheap.h"

/* ---- boot gate (fsformat.c shape) ----------------------------------- */

static int         fscheck_on  = 0;   /* build default: OFF */
static const char *fscheck_src = "build";

int fscheck_enabled(void) { return fscheck_on; }
const char *fscheck_source(void) { return fscheck_src; }
void fscheck_set(int enabled, const char *source) {
    fscheck_on  = enabled ? 1 : 0;
    fscheck_src = source ? source : "build";
}

/* ---- shared helpers --------------------------------------------------- */

static inline uint16_t fc_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t fc_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fc_read(int dev, uint64_t lba, uint32_t count, void *buf) {
    return blkdev_read(dev, lba, count, buf);
}

/* Count set bits among the FIRST nbits bits of a bitmap block.  The
 * partial last byte must be masked: bits past the group's valid range
 * are padding the formatter may leave zero, and counting them as free
 * (or used) is exactly the one-block drift fsck must not fabricate. */
static uint32_t fc_popcount_bits(const uint8_t *p, uint32_t nbits) {
    uint32_t c = 0;
    uint32_t full = nbits / 8;
    for (uint32_t i = 0; i < full; i++) {
        uint8_t b = p[i];
        while (b) { c += b & 1; b >>= 1; }
    }
    uint32_t rem = nbits % 8;
    if (rem) {
        uint8_t b = (uint8_t)(p[full] & ((1u << rem) - 1u));
        while (b) { c += b & 1; b >>= 1; }
    }
    return c;
}

/* ========================================================================
 * FAT32
 * ======================================================================== */

#define FAT32_MAX_DEPTH   12      /* directory recursion cap */
#define FAT32_MAX_CHAIN   4194304 /* loop guard, clusters */

struct fatgeo {
    uint32_t reserved;        /* sectors */
    uint32_t nfats;
    uint32_t fatsz;           /* sectors per FAT */
    uint32_t totsec;
    uint32_t rootcl;
    uint32_t fsinfo_sect;
    uint32_t fat_lba;
    uint32_t data_lba;
    uint32_t nclusters;       /* data clusters, numbered 2..nclusters+1 */
    uint32_t spc;
};

static int fat_findings;
static uint32_t fat_base;

static void fat_finding(const char *fmt, uint32_t a, uint32_t b, uint32_t c) {
    /* One print shape for every finding: name + three value slots.
     * Kept deliberately dumb so the host test's capture buffer can
     * grep it without format surprises. */
    kprintf("[fscheck] fat32: FINDING: ");
    kprintf(fmt, a, b, c);
    kprintf("\n");
    fat_findings++;
}

/* Read one FAT entry.  `fatsect` caches the last-read FAT sector. */
static int fat_entry(int dev, struct fatgeo *g, uint32_t cl,
                     uint32_t *out, uint32_t *cachesec, uint8_t *cache) {
    uint64_t off  = cl * 4ull;
    uint32_t sec  = (uint32_t)(off / 512u);
    if (sec != *cachesec) {
        if (fc_read(dev, g->fat_lba + sec, 1, cache) != 0)
            return -1;
        *cachesec = sec;
    }
    *out = fc_rd32(cache + (off % 512u)) & 0x0FFFFFFFu;
    return 0;
}

/* Walk one cluster chain.  Marks `visited` bits.  `expect_clusters`:
 * 0 = directory (any length), else exact file chain length. */
static void fat_walk_chain(int dev, struct fatgeo *g, uint32_t start,
                           uint8_t *visited, uint32_t expect_clusters,
                           int is_dir) {
    uint32_t cachesec = 0xFFFFFFFFu;
    uint8_t  cache[512];
    uint32_t cl = start;
    uint32_t len = 0;

    while (cl >= 2u && cl < g->nclusters + 2u && len < FAT32_MAX_CHAIN) {
        uint32_t idx = cl - 2u;
        if (visited[idx / 8] & (1u << (idx % 8))) {
            fat_finding("cross-link or loop at cluster %u", cl, 0, 0);
            return;
        }
        visited[idx / 8] |= (uint8_t)(1u << (idx % 8));
        len++;
        uint32_t next;
        if (fat_entry(dev, g, cl, &next, &cachesec, cache) != 0) {
            fat_finding("I/O error in FAT at cluster %u", cl, 0, 0);
            return;
        }
        if (next >= 0x0FFFFFF8u)
            break;                                  /* end of chain */
        if (next == 1u || next == 0u) {
            fat_finding("bad FAT entry %u mid-chain at cluster %u",
                        next, cl, 0);
            return;
        }
        cl = next;
    }
    if (len >= FAT32_MAX_CHAIN) {
        fat_finding("chain at cluster %u exceeds walk cap", start, 0, 0);
        return;
    }
    if (!is_dir && expect_clusters != 0 && len != expect_clusters) {
        fat_finding("chain length %u != size-implied %u",
                    len, expect_clusters, 0);
    }
}

static void fat_walk_dir(int dev, struct fatgeo *g, uint32_t start,
                         uint8_t *visited, uint8_t *cluster_buf,
                         int depth);

/* Scan one directory's worth of entries (all clusters of its chain). */
static void fat_scan_dir_entries(int dev, struct fatgeo *g, uint32_t start,
                                 uint8_t *visited, uint8_t *cluster_buf,
                                 int depth) {
    uint32_t cachesec = 0xFFFFFFFFu;
    uint8_t  cache[512];
    uint32_t cl = start;
    uint32_t guard = 0;

    while (cl >= 2u && cl < g->nclusters + 2u && guard++ < FAT32_MAX_CHAIN) {
        uint64_t lba = g->data_lba + (cl - 2u) * 1ull * g->spc;
        for (uint32_t s = 0; s < g->spc; s++) {
            if (fc_read(dev, lba + s, 1, cluster_buf) != 0) {
                fat_finding("I/O error reading dir cluster %u", cl, 0, 0);
                return;
            }
            for (int e = 0; e < 16; e++) {
                const uint8_t *ent = cluster_buf + e * 32;
                if (ent[0] == 0x00)
                    return;                     /* end of directory */
                if (ent[0] == 0xE5)
                    continue;                   /* deleted */
                if (ent[11] == 0x0F)
                    continue;                   /* LFN piece */
                /* "." and ".." are structural: fsck skips them in the
                 * cross-link analysis, and skipping them here stops a
                 * legal directory from recursing into itself. */
                if (ent[0] == 0x2E &&
                    (ent[1] == ' ' || ent[1] == 0x2E))
                    continue;
                uint32_t st = (uint32_t)(fc_rd16(ent + 26) |
                                         (fc_rd16(ent + 20) << 16));
                uint32_t size = fc_rd32(ent + 28);
                int is_dir = (ent[11] & 0x18) == 0x10; /* DIR, not volume */
                if (st == 0) {
                    /* A zero-size entry with no cluster is legal (created
                     * but never written, or an empty dir not yet given a
                     * cluster).  A size with no chain is not. */
                    if (size != 0)
                        fat_finding("entry with size %u but no cluster",
                                    size, 0, 0);
                    continue;
                }
                if (st < 2u || st >= g->nclusters + 2u) {
                    fat_finding("entry start cluster %u out of range",
                                st, 0, 0);
                    continue;
                }
                if (is_dir) {
                    fat_walk_dir(dev, g, st, visited, cluster_buf,
                                 depth + 1);
                } else {
                    uint32_t clu_bytes = g->spc * 512u;
                    uint32_t expect = size ?
                        (size + clu_bytes - 1) / clu_bytes : 0;
                    fat_walk_chain(dev, g, st, visited, expect, 0);
                }
            }
        }
        uint32_t next;
        if (fat_entry(dev, g, cl, &next, &cachesec, cache) != 0)
            return;
        if (next >= 0x0FFFFFF8u)
            return;
        if (next < 2u)
            return;
        cl = next;
    }
}

static void fat_walk_dir(int dev, struct fatgeo *g, uint32_t start,
                         uint8_t *visited, uint8_t *cluster_buf,
                         int depth) {
    if (depth > FAT32_MAX_DEPTH) {
        fat_finding("directory depth exceeds %u", FAT32_MAX_DEPTH, 0, 0);
        return;
    }
    fat_walk_chain(dev, g, start, visited, 0, 1);
    fat_scan_dir_entries(dev, g, start, visited, cluster_buf, depth);
}

int fscheck_fat32(int dev, uint32_t base_lba) {
    fat_findings = 0;
    fat_base = base_lba;
    uint8_t *sect = kmalloc(512);
    if (!sect) return -1;

    kprintf("[fscheck] fat32: walking blk%d (read-only)...\n", dev);

    if (fc_read(dev, fat_base + 0, 1, sect) != 0) {
        kprintf("[fscheck] fat32: FINDING: sector 0 unreadable\n");
        kfree(sect);
        return -1;
    }
    if (sect[510] != 0x55 || sect[511] != 0xAA ||
        memcmp(sect + 82, "FAT32", 5) != 0) {
        kprintf("[fscheck] fat32: FINDING: not a FAT32 boot sector\n");
        kfree(sect);
        return -1;
    }

    struct fatgeo g;
    memset(&g, 0, sizeof(g));
    uint32_t bps = fc_rd16(sect + 11);
    g.spc        = sect[13];
    g.reserved   = fc_rd16(sect + 14);
    g.nfats      = sect[16];
    g.totsec     = fc_rd32(sect + 32);
    g.fatsz      = fc_rd32(sect + 36);
    g.rootcl     = fc_rd32(sect + 44);
    g.fsinfo_sect = fc_rd16(sect + 48);

    if (bps != 512) {
        fat_finding("bytes-per-sector %u != 512", bps, 0, 0);
        kfree(sect);
        return -1;
    }
    if (g.spc == 0 || (g.spc & (g.spc - 1)) != 0 || g.spc > 128) {
        fat_finding("sectors-per-cluster %u not a sane power of two",
                    g.spc, 0, 0);
        kfree(sect);
        return -1;
    }
    if (g.nfats == 0 || g.nfats > 4) {
        fat_finding("FAT count %u out of range", g.nfats, 0, 0);
        kfree(sect);
        return -1;
    }
    if (g.fatsz == 0 || g.reserved == 0) {
        fat_finding("FAT geometry invalid (reserved=%u fatsz=%u)",
                    g.reserved, g.fatsz, 0);
        kfree(sect);
        return -1;
    }
    g.fat_lba  = fat_base + g.reserved;
    g.data_lba = g.fat_lba + g.nfats * g.fatsz;
    uint32_t data_rel = g.reserved + g.nfats * g.fatsz;
    if (g.totsec <= data_rel) {
        fat_finding("total sectors %u does not cover the FAT area",
                    g.totsec, 0, 0);
        kfree(sect);
        return -1;
    }
    /* totsec counts VOLUME sectors, so the cluster count subtracts the
     * RELATIVE data offset, not the absolute LBA (the superfloppy sits
     * at LBA 64 in-kernel). */
    g.nclusters = (g.totsec - data_rel) / g.spc;
    if (g.rootcl < 2u || g.rootcl >= g.nclusters + 2u) {
        fat_finding("root cluster %u out of range", g.rootcl, 0, 0);
        kfree(sect);
        return -1;
    }

    /* FSInfo: signatures + the free-count hint to cross-check later. */
    uint32_t fsi_free = 0xFFFFFFFFu;
    if (g.fsinfo_sect != 0 && g.fsinfo_sect < g.reserved) {
        if (fc_read(dev, fat_base + g.fsinfo_sect, 1, sect) != 0) {
            fat_finding("FSInfo sector %u unreadable", g.fsinfo_sect, 0, 0);
        } else {
            if (fc_rd32(sect + 0) != 0x41615252u ||
                fc_rd32(sect + 484) != 0x61417272u) {
                fat_finding("FSInfo signatures bad at sector %u",
                            g.fsinfo_sect, 0, 0);
            } else {
                fsi_free = fc_rd32(sect + 488);
            }
        }
    } else if (g.fsinfo_sect >= g.reserved) {
        fat_finding("FSInfo sector %u outside reserved area",
                    g.fsinfo_sect, 0, 0);
    }

    /* One full FAT scan: reserved entries, bounds, free count. */
    uint32_t cachesec = 0xFFFFFFFFu;
    uint8_t *cache = sect;                      /* reuse */
    uint32_t free_count = 0;
    int reserved_ok = 0;
    {
        uint32_t e0, e1;
        if (fat_entry(dev, &g, 0, &e0, &cachesec, cache) == 0 &&
            fat_entry(dev, &g, 1, &e1, &cachesec, cache) == 0) {
            reserved_ok = ((e0 & 0x0FFFFFFFu) >= 0x0FFFFFF8u &&
                           (e1 & 0x0FFFFFFFu) >= 0x0FFFFFF8u);
        }
        if (!reserved_ok)
            fat_finding("FAT[0]/FAT[1] reserved entries bad", 0, 0, 0);
    }
    for (uint32_t cl = 2; cl < g.nclusters + 2u; cl++) {
        uint32_t v;
        if (fat_entry(dev, &g, cl, &v, &cachesec, cache) != 0) {
            fat_finding("I/O error scanning FAT at cluster %u", cl, 0, 0);
            break;
        }
        if (v == 1u)
            fat_finding("FAT entry 1 at cluster %u (reserved value)", cl, 0, 0);
        else if (v < 0x0FFFFFF7u && v != 0u &&
                 (v < 2u || v >= g.nclusters + 2u))
            fat_finding("FAT entry %u out of range at cluster %u", v, cl, 0);
        else if (v == 0u)
            free_count++;
    }
    if (fsi_free != 0xFFFFFFFFu && fsi_free != free_count) {
        fat_finding("FSInfo free count %u != counted %u",
                    fsi_free, free_count, 0);
    }

    /* Directory walk from the root cluster. */
    uint8_t *visited = kmalloc((g.nclusters + 7) / 8);
    uint8_t *cluster_buf = kmalloc(512);
    if (visited && cluster_buf) {
        memset(visited, 0, (g.nclusters + 7) / 8);
        fat_walk_dir(dev, &g, g.rootcl, visited, cluster_buf, 0);
    } else {
        kprintf("[fscheck] fat32: FINDING: out of memory for walk\n");
        fat_findings++;
    }
    if (visited) kfree(visited);
    if (cluster_buf) kfree(cluster_buf);

    if (fat_findings == 0)
        kprintf("[fscheck] fat32: CLEAN (%u clusters, %u free)\n",
                g.nclusters, free_count);
    else
        kprintf("[fscheck] fat32: %u finding(s)\n", fat_findings);
    kfree(sect);
    return fat_findings;
}

/* ========================================================================
 * ext2
 * ======================================================================== */

#define EXT2_MAGIC_VAL    0xEF53u
#define EXT2_SB_OFFSET    1024u          /* byte offset of the superblock */
#define EXT2_MAX_GROUPS   4096u
#define EXT2_INODE_SIZE   128u

static int ext2_findings;
static uint32_t ext2_base;

static void ext2_finding(const char *fmt, uint32_t a, uint32_t b, uint32_t c) {
    kprintf("[fscheck] ext2: FINDING: ");
    kprintf(fmt, a, b, c);
    kprintf("\n");
    ext2_findings++;
}

int fscheck_ext2(int dev, uint32_t base_lba) {
    ext2_findings = 0;
    ext2_base = base_lba;
    uint8_t *blk = kmalloc(4096);
    if (!blk) return -1;

    kprintf("[fscheck] ext2: walking blk%d (read-only)...\n", dev);

    /* The superblock lives at byte 1024 — sectors 2-3 for 1K blocks,
     * inside block 0 for larger ones.  Read both sectors. */
    if (fc_read(dev, ext2_base + EXT2_SB_OFFSET / 512u, 2, blk) != 0) {
        kprintf("[fscheck] ext2: FINDING: superblock sectors unreadable\n");
        kfree(blk);
        return -1;
    }
    uint32_t inodes_count   = fc_rd32(blk + 0);
    uint32_t blocks_count   = fc_rd32(blk + 4);
    uint32_t free_blocks    = fc_rd32(blk + 12);
    uint32_t free_inodes    = fc_rd32(blk + 16);
    uint32_t first_data_blk = fc_rd32(blk + 20);
    uint32_t log_block_size = fc_rd32(blk + 24);
    uint32_t blocks_per_grp = fc_rd32(blk + 32);
    uint32_t inodes_per_grp = fc_rd32(blk + 40);
    uint32_t magic          = fc_rd16(blk + 56);

    if (magic != EXT2_MAGIC_VAL) {
        kprintf("[fscheck] ext2: FINDING: superblock magic 0x%x != 0xEF53\n",
                magic);
        kfree(blk);
        return -1;
    }
    if (log_block_size > 2) {
        ext2_finding("log_block_size %u implies blocks > 4096",
                     log_block_size, 0, 0);
        kfree(blk);
        return -1;
    }
    uint32_t block_size = 1024u << log_block_size;
    uint32_t sect_per_blk = block_size / 512u;
    if (blocks_count == 0 || inodes_count == 0 ||
        blocks_per_grp == 0 || inodes_per_grp == 0) {
        ext2_finding("superblock geometry zero (blocks=%u inodes=%u)",
                     blocks_count, inodes_count, 0);
        kfree(blk);
        return -1;
    }
    uint32_t groups = (blocks_count - first_data_blk + blocks_per_grp - 1) /
                      blocks_per_grp;
    if (groups == 0 || groups > EXT2_MAX_GROUPS) {
        ext2_finding("group count %u out of range", groups, 0, 0);
        kfree(blk);
        return -1;
    }

    /* Group descriptor table: the block after the superblock block. */
    uint32_t sb_block   = (block_size == 1024u) ? 1u : 0u;
    uint32_t gdt_block  = sb_block + 1u;
    uint32_t gdt_sectors = (groups * 32u + block_size - 1) / block_size *
                           sect_per_blk;

    uint8_t *gdt = kmalloc(groups * 32u);
    if (!gdt) { kfree(blk); return -1; }
    {
        uint32_t need = groups * 32u;
        uint32_t off = 0;
        uint64_t lba = gdt_block * 1ull * sect_per_blk;
        while (off < need) {
            if (fc_read(dev, ext2_base + lba, sect_per_blk, blk) != 0) {
                ext2_finding("group descriptor table unreadable", 0, 0, 0);
                kfree(gdt); kfree(blk);
                return -1;
            }
            uint32_t take = need - off < block_size ? need - off : block_size;
            memcpy(gdt + off, blk, take);
            off += take;
            lba += sect_per_blk;
        }
        (void)gdt_sectors;
    }

    uint32_t sum_free_blocks = 0, sum_free_inodes = 0;
    uint32_t itable_blocks = (inodes_per_grp * EXT2_INODE_SIZE +
                              block_size - 1) / block_size;

    uint8_t *bm = kmalloc(block_size);
    uint8_t *itable = kmalloc(itable_blocks * block_size);
    if (!bm || !itable) {
        if (bm) kfree(bm);
        if (itable) kfree(itable);
        kfree(gdt); kfree(blk);
        return -1;
    }

    for (uint32_t grp = 0; grp < groups; grp++) {
        const uint8_t *gd = gdt + grp * 32u;
        uint32_t bb = fc_rd32(gd + 0);      /* block bitmap block */
        uint32_t ib = fc_rd32(gd + 4);      /* inode bitmap block */
        uint32_t it = fc_rd32(gd + 8);      /* inode table block */
        uint32_t fb = fc_rd16(gd + 12);
        uint32_t fi = fc_rd16(gd + 14);

        if (bb >= blocks_count || ib >= blocks_count ||
            it < first_data_blk || it + itable_blocks > blocks_count) {
            ext2_finding("group %u descriptor blocks out of range", grp, 0, 0);
            continue;
        }

        /* Block bitmap vs the group's free-block count. */
        if (fc_read(dev, ext2_base + bb * 1ull * sect_per_blk, sect_per_blk, bm) != 0) {
            ext2_finding("group %u block bitmap unreadable", grp, 0, 0);
            continue;
        }
        uint32_t valid_blocks = blocks_per_grp;
        if (grp == groups - 1)
            valid_blocks = blocks_count - first_data_blk -
                           grp * blocks_per_grp;
        uint32_t used = fc_popcount_bits(bm, valid_blocks);
        uint32_t freeb = valid_blocks > used ? valid_blocks - used : 0;
        if (freeb != fb)
            ext2_finding("group %u free blocks gd=%u != bitmap %u",
                         grp, fb, freeb);
        sum_free_blocks += freeb;

        /* Inode bitmap vs the group's free-inode count, then the inode
         * table cross-check: in-use ⇔ bit set. */
        if (fc_read(dev, ext2_base + ib * 1ull * sect_per_blk, sect_per_blk, bm) != 0) {
            ext2_finding("group %u inode bitmap unreadable", grp, 0, 0);
            continue;
        }
        uint32_t valid_inodes = inodes_per_grp;
        if (grp == groups - 1 && inodes_count > grp * inodes_per_grp)
            valid_inodes = inodes_count - grp * inodes_per_grp;
        uint32_t iused = fc_popcount_bits(bm, valid_inodes);
        uint32_t freei = valid_inodes > iused ? valid_inodes - iused : 0;
        if (freei != fi)
            ext2_finding("group %u free inodes gd=%u != bitmap %u",
                         grp, fi, freei);
        sum_free_inodes += freei;

        if (fc_read(dev, ext2_base + it * 1ull * sect_per_blk,
                    itable_blocks * sect_per_blk, itable) != 0) {
            ext2_finding("group %u inode table unreadable", grp, 0, 0);
            continue;
        }
        for (uint32_t i = 0; i < valid_inodes; i++) {
            const uint8_t *ino = itable + i * EXT2_INODE_SIZE;
            uint16_t mode  = fc_rd16(ino + 0);
            uint16_t links = fc_rd16(ino + 26);
            int bit = (bm[i / 8] >> (i % 8)) & 1;
            int in_use = (links > 0 || mode != 0);
            uint32_t global_ino = grp * inodes_per_grp + i + 1;
            if (in_use && !bit)
                ext2_finding("inode %u in use but bitmap says free",
                             global_ino, 0, 0);
            else if (!in_use && bit && global_ino > 10)
                ext2_finding("inode %u bitmap-set but mode/links zero",
                             global_ino, 0, 0);
            /* The root inode must be a directory. */
            if (global_ino == 2 && in_use && (mode & 0xF000) != 0x4000)
                ext2_finding("root inode 2 is not a directory (mode 0x%x)",
                             mode, 0, 0);
        }
    }

    if (sum_free_blocks != free_blocks)
        ext2_finding("superblock free blocks %u != summed %u",
                     free_blocks, sum_free_blocks, 0);
    if (sum_free_inodes != free_inodes)
        ext2_finding("superblock free inodes %u != summed %u",
                     free_inodes, sum_free_inodes, 0);

    kfree(bm);
    kfree(itable);
    kfree(gdt);

    if (ext2_findings == 0)
        kprintf("[fscheck] ext2: CLEAN (%u blocks, %u inodes, %u groups)\n",
                blocks_count, inodes_count, groups);
    else
        kprintf("[fscheck] ext2: %u finding(s)\n", ext2_findings);
    kfree(blk);
    return ext2_findings;
}
