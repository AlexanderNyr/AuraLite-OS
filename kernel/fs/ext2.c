/* ext2.c — ext2 read/write driver with mkfs.
 *
 * On-disk layout (offsets from partition LBA 0):
 *
 *   block 0  : padding (1024-byte boot area, doesn't exist in our setup)
 *   block 1  : SUPERBLOCK (always at byte offset 1024 from FS start)
 *   block 2+ : group descriptor table (GDT)
 *   then per block group:
 *     - block bitmap   (1 block)
 *     - inode bitmap   (1 block)
 *     - inode table    (sb->s_inodes_per_group * inode_size / block_size blocks)
 *     - data blocks    (rest)
 *
 * We support block sizes 1024 / 2048 / 4096.  Logical block N starts at
 * byte offset `N * block_size` from the FS base LBA.
 *
 * Inodes are 1-indexed.  The root directory is inode 2.
 *
 * Indirect block addressing:
 *   inode->i_block[0..11]   : direct
 *   inode->i_block[12]      : indirect (block of pointers)
 *   inode->i_block[13]      : double indirect
 *   inode->i_block[14]      : triple indirect
 *
 * Directory entries are variable-length records of the form
 *   { u32 inode; u16 rec_len; u8 name_len; u8 file_type; char name[]; }
 * padded so rec_len is a multiple of 4.
 */

#include <stdint.h>
#include "kernel/fs/ext2.h"
#include "drivers/ahci/ahci.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/kheap.h"

/* ----- On-disk superblock (offsets per ext2 spec) ----- */
struct ext2_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;       /* block_size = 1024 << this */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;                /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* Dynamic rev (s_rev_level == 1) fields: */
    uint32_t s_first_ino;            /* first non-reserved inode (usually 11) */
    uint16_t s_inode_size;           /* bytes per inode struct (>= 128) */
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    /* … many more fields, irrelevant to us … */
    uint8_t  padding[1024 - 204];
} __attribute__((packed));

#define EXT2_MAGIC          0xEF53
#define EXT2_GOOD_OLD_REV   0
#define EXT2_DYNAMIC_REV    1
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_ROOT_INO       2
#define EXT2_FIRST_USER_INO 11

/* ----- Group descriptor (32 bytes for rev 0/1) ----- */
struct ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed));

/* ----- Inode (rev 0 = 128 bytes) ----- */
struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;               /* in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xA000

/* file types in dir entry */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_SYMLINK  7

struct ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

/* ----- Mount state ----- */
struct ext2_mount {
    int      ahci_port;
    uint32_t fs_base_lba;        /* absolute LBA of the start of FS */
    uint32_t fs_lba_count;       /* size of FS area in 512-byte sectors */
    uint32_t block_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t group_count;
    uint32_t first_data_block;
    uint32_t first_ino;
    int      mounted;
    /* The superblock and GDT are kept resident — small. */
    struct ext2_super        sb;
    struct ext2_group_desc  *gdt;     /* group_count entries */
};

static struct ext2_mount es;

/* Scratch buffer.  Allocated once, big enough for the largest block size. */
static uint8_t *block_buf = NULL;          /* block_size bytes */
/* Indirect-pointer scratch — separate from block_buf so reading a block
 * while another is being indexed doesn't trash either.  We need up to 3
 * concurrent indirect blocks (for triple-indirect), each block_size bytes. */
static uint32_t *ind_buf[3] = { NULL, NULL, NULL };
/* Helper: lazy-allocate the indirect scratch slot. */
static uint32_t *get_ind(int slot) {
    if (!ind_buf[slot]) ind_buf[slot] = (uint32_t *)kmalloc(es.block_size);
    return ind_buf[slot];
}

/* Open-vnode cache: keyed by inode number so multiple lookups share state. */
#define EXT2_MAX_OPEN 128
struct ext2_vinfo {
    int        in_use;
    uint32_t   inode_no;
    struct ext2_inode inode;
    struct vnode vnode;
};
static struct ext2_vinfo vcache[EXT2_MAX_OPEN];

static uint32_t ext2_now(void) {
    uint64_t now = vfs_now();
    return now > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)now;
}

/* ---- Block I/O ---- */

static int read_blocks(uint32_t fs_lba, uint32_t count, void *buf) {
    /* fs_lba here is a 512-byte LBA RELATIVE to fs_base_lba */
    return ahci_read((uint32_t)es.ahci_port, es.fs_base_lba + fs_lba, count, buf);
}
static int write_blocks(uint32_t fs_lba, uint32_t count, const void *buf) {
    return ahci_write((uint32_t)es.ahci_port, es.fs_base_lba + fs_lba, count, buf);
}

/* Read/write one filesystem block (block_size bytes = block_size/512 sectors). */
static int read_block(uint32_t block_no, void *buf) {
    uint32_t spb = es.block_size / 512;
    return read_blocks(block_no * spb, spb, buf);
}
static int write_block(uint32_t block_no, const void *buf) {
    uint32_t spb = es.block_size / 512;
    return write_blocks(block_no * spb, spb, buf);
}

/* ---- Superblock + GDT I/O ---- */

static int read_super(void) {
    /* SB lives at byte 1024 from FS base. */
    /* Read sectors 2 and 3 (= bytes 1024..2047). */
    uint8_t tmp[1024];
    if (read_blocks(2, 2, tmp) != 0) return -1;
    memcpy(&es.sb, tmp, sizeof(es.sb) > 1024 ? 1024 : sizeof(es.sb));
    return 0;
}
static int write_super(void) {
    uint8_t tmp[1024];
    if (read_blocks(2, 2, tmp) != 0) return -1;
    /* Only refresh the first 1024 bytes of the struct we modify. */
    uint32_t want = sizeof(es.sb) < 1024 ? sizeof(es.sb) : 1024;
    memcpy(tmp, &es.sb, want);
    return write_blocks(2, 2, tmp);
}

static uint32_t gdt_start_block(void) {
    return (es.block_size == 1024) ? 2 : 1;
}

static int read_gdt(void) {
    uint32_t bytes = es.group_count * sizeof(struct ext2_group_desc);
    uint32_t blocks = (bytes + es.block_size - 1) / es.block_size;
    es.gdt = (struct ext2_group_desc *)kmalloc(blocks * es.block_size);
    if (!es.gdt) return -1;
    uint32_t start = gdt_start_block();
    uint32_t spb = es.block_size / 512;
    if (read_blocks(start * spb, blocks * spb, es.gdt) != 0) return -1;
    return 0;
}
static int write_gdt(void) {
    uint32_t bytes  = es.group_count * sizeof(struct ext2_group_desc);
    uint32_t blocks = (bytes + es.block_size - 1) / es.block_size;
    uint32_t start  = gdt_start_block();
    uint32_t spb    = es.block_size / 512;
    return write_blocks(start * spb, blocks * spb, es.gdt);
}

/* ---- Inode I/O ---- */

static int read_inode(uint32_t ino, struct ext2_inode *out) {
    if (ino == 0 || ino > es.inodes_count) return -1;
    uint32_t group   = (ino - 1) / es.inodes_per_group;
    uint32_t index   = (ino - 1) % es.inodes_per_group;
    uint32_t off     = index * es.inode_size;
    uint32_t blk_off = off / es.block_size;
    uint32_t in_off  = off % es.block_size;
    uint32_t blk     = es.gdt[group].bg_inode_table + blk_off;
    if (read_block(blk, block_buf) != 0) return -1;
    /* Copy at most sizeof(struct ext2_inode); pad with zero. */
    memset(out, 0, sizeof(*out));
    uint32_t copy = es.inode_size < sizeof(*out) ? es.inode_size : sizeof(*out);
    memcpy(out, block_buf + in_off, copy);
    return 0;
}

static int write_inode(uint32_t ino, const struct ext2_inode *in) {
    if (ino == 0 || ino > es.inodes_count) return -1;
    uint32_t group   = (ino - 1) / es.inodes_per_group;
    uint32_t index   = (ino - 1) % es.inodes_per_group;
    uint32_t off     = index * es.inode_size;
    uint32_t blk_off = off / es.block_size;
    uint32_t in_off  = off % es.block_size;
    uint32_t blk     = es.gdt[group].bg_inode_table + blk_off;
    if (read_block(blk, block_buf) != 0) return -1;
    uint32_t copy = es.inode_size < sizeof(*in) ? es.inode_size : sizeof(*in);
    memcpy(block_buf + in_off, in, copy);
    return write_block(blk, block_buf);
}

/* ---- Bitmap helpers ---- */

static int bitmap_test(const uint8_t *bm, uint32_t idx) {
    return (bm[idx / 8] >> (idx % 8)) & 1;
}
static void bitmap_set(uint8_t *bm, uint32_t idx) {
    bm[idx / 8] |= (uint8_t)(1u << (idx % 8));
}
static void bitmap_clear(uint8_t *bm, uint32_t idx) {
    bm[idx / 8] &= (uint8_t)~(1u << (idx % 8));
}

/* ---- Block allocator ---- */

static uint32_t alloc_data_block(void) {
    for (uint32_t g = 0; g < es.group_count; g++) {
        if (es.gdt[g].bg_free_blocks_count == 0) continue;
        if (read_block(es.gdt[g].bg_block_bitmap, block_buf) != 0) return 0;
        for (uint32_t i = 0; i < es.blocks_per_group; i++) {
            if (!bitmap_test(block_buf, i)) {
                uint32_t bno = g * es.blocks_per_group + es.first_data_block + i;
                if (bno >= es.blocks_count) return 0;
                bitmap_set(block_buf, i);
                if (write_block(es.gdt[g].bg_block_bitmap, block_buf) != 0) return 0;
                es.gdt[g].bg_free_blocks_count--;
                es.sb.s_free_blocks_count--;
                write_gdt();
                write_super();
                /* Zero the freshly allocated block. */
                memset(block_buf, 0, es.block_size);
                write_block(bno, block_buf);
                return bno;
            }
        }
    }
    return 0;
}

static void free_data_block(uint32_t bno) {
    if (bno == 0) return;
    if (bno < es.first_data_block) return;
    uint32_t off  = bno - es.first_data_block;
    uint32_t g    = off / es.blocks_per_group;
    uint32_t i    = off % es.blocks_per_group;
    if (g >= es.group_count) return;
    if (read_block(es.gdt[g].bg_block_bitmap, block_buf) != 0) return;
    if (bitmap_test(block_buf, i)) {
        bitmap_clear(block_buf, i);
        write_block(es.gdt[g].bg_block_bitmap, block_buf);
        es.gdt[g].bg_free_blocks_count++;
        es.sb.s_free_blocks_count++;
        write_gdt();
        write_super();
    }
}

static uint32_t alloc_inode(int for_dir) {
    for (uint32_t g = 0; g < es.group_count; g++) {
        if (es.gdt[g].bg_free_inodes_count == 0) continue;
        if (read_block(es.gdt[g].bg_inode_bitmap, block_buf) != 0) return 0;
        for (uint32_t i = 0; i < es.inodes_per_group; i++) {
            uint32_t ino = g * es.inodes_per_group + i + 1;
            if (ino < es.first_ino) continue;
            if (!bitmap_test(block_buf, i)) {
                bitmap_set(block_buf, i);
                if (write_block(es.gdt[g].bg_inode_bitmap, block_buf) != 0) return 0;
                es.gdt[g].bg_free_inodes_count--;
                if (for_dir) es.gdt[g].bg_used_dirs_count++;
                es.sb.s_free_inodes_count--;
                write_gdt();
                write_super();
                return ino;
            }
        }
    }
    return 0;
}

static void free_inode(uint32_t ino, int was_dir) {
    if (ino == 0 || ino > es.inodes_count) return;
    uint32_t g = (ino - 1) / es.inodes_per_group;
    uint32_t i = (ino - 1) % es.inodes_per_group;
    if (read_block(es.gdt[g].bg_inode_bitmap, block_buf) != 0) return;
    if (bitmap_test(block_buf, i)) {
        bitmap_clear(block_buf, i);
        write_block(es.gdt[g].bg_inode_bitmap, block_buf);
        es.gdt[g].bg_free_inodes_count++;
        if (was_dir && es.gdt[g].bg_used_dirs_count > 0)
            es.gdt[g].bg_used_dirs_count--;
        es.sb.s_free_inodes_count++;
        write_gdt();
        write_super();
    }
}

/* ---- Block addressing within an inode ---- */

/* Number of u32 pointers per indirect block. */
static uint32_t ptrs_per_block(void) { return es.block_size / 4; }

/* Map a file-relative block index to an actual block number.  If `alloc`
 * is non-zero, missing branches are allocated as we go.  Returns 0 on
 * "no block, no alloc" (a sparse hole; reads should treat as zero). */
static uint32_t bmap(struct ext2_inode *inode, uint32_t fblock, int alloc) {
    uint32_t ppb = ptrs_per_block();

    if (fblock < 12) {
        if (inode->i_block[fblock] == 0 && alloc) {
            uint32_t b = alloc_data_block();
            if (!b) return 0;
            inode->i_block[fblock] = b;
            inode->i_blocks += es.block_size / 512;
        }
        return inode->i_block[fblock];
    }

    /* Single indirect. */
    uint32_t off = fblock - 12;
    if (off < ppb) {
        if (inode->i_block[12] == 0) {
            if (!alloc) return 0;
            inode->i_block[12] = alloc_data_block();
            if (!inode->i_block[12]) return 0;
            inode->i_blocks += es.block_size / 512;
        }
        uint32_t *buf = get_ind(0);
        if (read_block(inode->i_block[12], buf) != 0) return 0;
        if (buf[off] == 0 && alloc) {
            uint32_t b = alloc_data_block();
            if (!b) return 0;
            buf[off] = b;
            inode->i_blocks += es.block_size / 512;
            write_block(inode->i_block[12], buf);
        }
        return buf[off];
    }
    off -= ppb;

    /* Double indirect. */
    if (off < ppb * ppb) {
        if (inode->i_block[13] == 0) {
            if (!alloc) return 0;
            inode->i_block[13] = alloc_data_block();
            if (!inode->i_block[13]) return 0;
            inode->i_blocks += es.block_size / 512;
        }
        uint32_t *l1 = get_ind(0);
        if (read_block(inode->i_block[13], l1) != 0) return 0;
        uint32_t idx1 = off / ppb;
        uint32_t idx2 = off % ppb;
        if (l1[idx1] == 0) {
            if (!alloc) return 0;
            l1[idx1] = alloc_data_block();
            if (!l1[idx1]) return 0;
            inode->i_blocks += es.block_size / 512;
            write_block(inode->i_block[13], l1);
        }
        uint32_t l1_idx_blk = l1[idx1];
        uint32_t *l2 = get_ind(1);
        if (read_block(l1_idx_blk, l2) != 0) return 0;
        if (l2[idx2] == 0 && alloc) {
            uint32_t b = alloc_data_block();
            if (!b) return 0;
            l2[idx2] = b;
            inode->i_blocks += es.block_size / 512;
            write_block(l1_idx_blk, l2);
        }
        return l2[idx2];
    }
    off -= ppb * ppb;

    /* Triple indirect. */
    if (off < ppb * ppb * ppb) {
        if (inode->i_block[14] == 0) {
            if (!alloc) return 0;
            inode->i_block[14] = alloc_data_block();
            if (!inode->i_block[14]) return 0;
            inode->i_blocks += es.block_size / 512;
        }
        uint32_t *l1 = get_ind(0);
        if (read_block(inode->i_block[14], l1) != 0) return 0;
        uint32_t idx1 = off / (ppb * ppb);
        uint32_t rest = off % (ppb * ppb);
        uint32_t idx2 = rest / ppb;
        uint32_t idx3 = rest % ppb;
        if (l1[idx1] == 0) {
            if (!alloc) return 0;
            l1[idx1] = alloc_data_block();
            if (!l1[idx1]) return 0;
            inode->i_blocks += es.block_size / 512;
            write_block(inode->i_block[14], l1);
        }
        uint32_t l1_blk = l1[idx1];
        uint32_t *l2 = get_ind(1);
        if (read_block(l1_blk, l2) != 0) return 0;
        if (l2[idx2] == 0) {
            if (!alloc) return 0;
            l2[idx2] = alloc_data_block();
            if (!l2[idx2]) return 0;
            inode->i_blocks += es.block_size / 512;
            write_block(l1_blk, l2);
        }
        uint32_t l2_blk = l2[idx2];
        uint32_t *l3 = get_ind(2);
        if (read_block(l2_blk, l3) != 0) return 0;
        if (l3[idx3] == 0 && alloc) {
            uint32_t b = alloc_data_block();
            if (!b) return 0;
            l3[idx3] = b;
            inode->i_blocks += es.block_size / 512;
            write_block(l2_blk, l3);
        }
        return l3[idx3];
    }
    return 0;  /* file too big */
}

/* Free every block referenced by an inode (used by unlink). */
static void free_all_blocks(struct ext2_inode *inode) {
    uint32_t ppb = ptrs_per_block();
    for (int i = 0; i < 12; i++) {
        if (inode->i_block[i]) { free_data_block(inode->i_block[i]); inode->i_block[i] = 0; }
    }
    /* Allocate a single block-size scratch buffer on the heap to avoid blowing
     * the 16 KiB kernel stack in deeply nested loops. */
    uint32_t *l1 = (uint32_t *)kmalloc(es.block_size);
    uint32_t *l2 = (uint32_t *)kmalloc(es.block_size);
    uint32_t *l3 = (uint32_t *)kmalloc(es.block_size);
    if (!l1 || !l2 || !l3) {
        if (l1) kfree(l1);
        if (l2) kfree(l2);
        if (l3) kfree(l3);
        return;
    }
    if (inode->i_block[12]) {
        if (read_block(inode->i_block[12], l1) == 0) {
            for (uint32_t i = 0; i < ppb; i++)
                if (l1[i]) free_data_block(l1[i]);
        }
        free_data_block(inode->i_block[12]);
        inode->i_block[12] = 0;
    }
    if (inode->i_block[13]) {
        if (read_block(inode->i_block[13], l1) == 0) {
            for (uint32_t i = 0; i < ppb; i++) {
                if (!l1[i]) continue;
                if (read_block(l1[i], l2) == 0) {
                    for (uint32_t j = 0; j < ppb; j++)
                        if (l2[j]) free_data_block(l2[j]);
                }
                free_data_block(l1[i]);
            }
        }
        free_data_block(inode->i_block[13]);
        inode->i_block[13] = 0;
    }
    if (inode->i_block[14]) {
        if (read_block(inode->i_block[14], l1) == 0) {
            for (uint32_t i = 0; i < ppb; i++) {
                if (!l1[i]) continue;
                if (read_block(l1[i], l2) == 0) {
                    for (uint32_t j = 0; j < ppb; j++) {
                        if (!l2[j]) continue;
                        if (read_block(l2[j], l3) == 0) {
                            for (uint32_t k = 0; k < ppb; k++)
                                if (l3[k]) free_data_block(l3[k]);
                        }
                        free_data_block(l2[j]);
                    }
                }
                free_data_block(l1[i]);
            }
        }
        free_data_block(inode->i_block[14]);
        inode->i_block[14] = 0;
    }
    kfree(l1); kfree(l2); kfree(l3);
    inode->i_blocks = 0;
}

/* ---- File-level read/write ---- */

static int64_t inode_read(struct ext2_inode *inode, uint64_t pos, void *buf, uint64_t count) {
    if (pos >= inode->i_size) return 0;
    if (pos + count > inode->i_size) count = inode->i_size - pos;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t fblock = (uint32_t)((pos + done) / es.block_size);
        uint32_t off    = (uint32_t)((pos + done) % es.block_size);
        uint32_t bno    = bmap(inode, fblock, 0);
        uint64_t chunk  = es.block_size - off;
        if (chunk > count - done) chunk = count - done;
        if (bno == 0) {
            /* sparse hole — fill with zero */
            memset(out + done, 0, chunk);
        } else {
            if (read_block(bno, block_buf) != 0) return -1;
            memcpy(out + done, block_buf + off, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t inode_write(uint32_t ino, struct ext2_inode *inode,
                           uint64_t pos, const void *buf, uint64_t count) {
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t fblock = (uint32_t)((pos + done) / es.block_size);
        uint32_t off    = (uint32_t)((pos + done) % es.block_size);
        uint32_t bno    = bmap(inode, fblock, 1);
        if (!bno) return -1;
        if (read_block(bno, block_buf) != 0) return -1;
        uint64_t chunk = es.block_size - off;
        if (chunk > count - done) chunk = count - done;
        memcpy(block_buf + off, in + done, chunk);
        if (write_block(bno, block_buf) != 0) return -1;
        done += chunk;
    }
    if (pos + count > inode->i_size) inode->i_size = (uint32_t)(pos + count);
    inode->i_mtime = inode->i_ctime = ext2_now();
    write_inode(ino, inode);
    return (int64_t)done;
}

/* ---- Directory operations ---- */

/* Iterate dir entries; for each call cb(name, entry, user) — return non-zero
 * from cb to stop iteration.  Returns 0 on completion, -1 on io error. */
typedef int (*dir_iter_cb)(const char *name, uint32_t ino, uint8_t type, void *user);

static int dir_iterate(struct ext2_inode *dir, dir_iter_cb cb, void *user) {
    if ((dir->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
    uint64_t pos = 0;
    while (pos < dir->i_size) {
        uint32_t fblock = (uint32_t)(pos / es.block_size);
        uint32_t off    = (uint32_t)(pos % es.block_size);
        uint32_t bno    = bmap(dir, fblock, 0);
        if (!bno) { pos += es.block_size - off; continue; }
        if (read_block(bno, block_buf) != 0) return -1;
        while (off < es.block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + off);
            if (de->rec_len < 8 || de->rec_len > es.block_size - off) break;
            if (de->inode != 0 && de->name_len > 0) {
                char name[256];
                uint32_t nl = de->name_len;
                memcpy(name, de->name, nl);
                name[nl] = 0;
                if (cb(name, de->inode, de->file_type, user)) return 0;
            }
            off += de->rec_len;
            pos += de->rec_len;
        }
    }
    return 0;
}

struct dir_find_ctx { const char *want; uint32_t found_ino; uint8_t found_type; };
static int dir_find_cb(const char *name, uint32_t ino, uint8_t type, void *user) {
    struct dir_find_ctx *c = (struct dir_find_ctx *)user;
    if (strcmp(name, c->want) == 0) {
        c->found_ino = ino;
        c->found_type = type;
        return 1;
    }
    return 0;
}

/* Look up `name` inside directory inode `dir`. Returns inode # or 0. */
static uint32_t dir_lookup_in(struct ext2_inode *dir, const char *name, uint8_t *out_type) {
    struct dir_find_ctx c = { name, 0, 0 };
    if (dir_iterate(dir, dir_find_cb, &c) != 0) return 0;
    if (out_type) *out_type = c.found_type;
    return c.found_ino;
}

/* Round up to 4. */
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

/* Insert a new entry (ino, name, type) into directory.  Allocates a new dir
 * block if needed.  Returns 0 on success. */
static int dir_insert(uint32_t dir_ino, struct ext2_inode *dir,
                      const char *name, uint32_t ino, uint8_t file_type) {
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t need     = align4(8 + name_len);

    uint64_t pos = 0;
    while (pos < dir->i_size) {
        uint32_t fblock = (uint32_t)(pos / es.block_size);
        uint32_t bno    = bmap(dir, fblock, 0);
        if (!bno) { pos += es.block_size; continue; }
        if (read_block(bno, block_buf) != 0) return -1;

        uint32_t off = 0;
        while (off < es.block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + off);
            if (de->rec_len < 8 || de->rec_len > es.block_size - off) break;

            uint32_t actual = de->inode ? align4(8 + de->name_len) : 0;
            uint32_t slack  = de->rec_len - actual;

            /* Empty entry (inode == 0) — overwrite directly. */
            if (de->inode == 0 && de->rec_len >= need) {
                de->inode = ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);
                if (write_block(bno, block_buf) != 0) return -1;
                return 0;
            }
            /* Split this entry — leave it occupying its true size, place new
             * entry in the slack. */
            if (de->inode && slack >= need) {
                uint16_t old_rec = de->rec_len;
                de->rec_len = (uint16_t)actual;
                struct ext2_dir_entry *nu =
                    (struct ext2_dir_entry *)(block_buf + off + actual);
                nu->inode = ino;
                nu->rec_len = (uint16_t)(old_rec - actual);
                nu->name_len = (uint8_t)name_len;
                nu->file_type = file_type;
                memcpy(nu->name, name, name_len);
                if (write_block(bno, block_buf) != 0) return -1;
                return 0;
            }
            off += de->rec_len;
        }
        pos += es.block_size;
    }

    /* No room — allocate a new block at end of dir. */
    uint32_t new_fblock = (uint32_t)(dir->i_size / es.block_size);
    uint32_t bno = bmap(dir, new_fblock, 1);
    if (!bno) return -1;
    memset(block_buf, 0, es.block_size);
    struct ext2_dir_entry *de = (struct ext2_dir_entry *)block_buf;
    de->inode = ino;
    de->rec_len = (uint16_t)es.block_size;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);
    if (write_block(bno, block_buf) != 0) return -1;
    dir->i_size += es.block_size;
    write_inode(dir_ino, dir);
    return 0;
}

/* Remove an entry by name; coalesces with the previous entry's rec_len. */
static int dir_remove(uint32_t dir_ino, struct ext2_inode *dir, const char *name,
                      uint32_t *out_removed_ino, uint8_t *out_removed_type) {
    (void)dir_ino;
    uint64_t pos = 0;
    while (pos < dir->i_size) {
        uint32_t fblock = (uint32_t)(pos / es.block_size);
        uint32_t bno    = bmap(dir, fblock, 0);
        if (!bno) { pos += es.block_size; continue; }
        if (read_block(bno, block_buf) != 0) return -1;

        uint32_t off = 0;
        struct ext2_dir_entry *prev = NULL;
        while (off < es.block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + off);
            if (de->rec_len < 8 || de->rec_len > es.block_size - off) break;
            char tmp[256]; uint32_t nl = de->name_len;
            memcpy(tmp, de->name, nl); tmp[nl] = 0;
            if (de->inode != 0 && strcmp(tmp, name) == 0) {
                if (out_removed_ino)  *out_removed_ino  = de->inode;
                if (out_removed_type) *out_removed_type = de->file_type;
                if (prev) {
                    prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                } else {
                    de->inode = 0;
                    de->name_len = 0;
                    de->file_type = 0;
                }
                return write_block(bno, block_buf);
            }
            prev = de;
            off += de->rec_len;
        }
        pos += es.block_size;
    }
    return -1;
}

/* ---- Path resolution ---- */

/* Look up path (slash-separated, leading slash optional); returns inode#.
 * Also fills *out_parent_ino and *out_basename if non-NULL. */
static uint32_t path_to_ino(const char *path, uint32_t *out_parent_ino,
                            char *out_basename, int basesz) {
    while (*path == '/') path++;
    uint32_t cur = EXT2_ROOT_INO;
    uint32_t parent = EXT2_ROOT_INO;
    char last[256] = {0};
    while (*path) {
        char comp[256];
        int n = 0;
        while (*path && *path != '/' && n < 255) comp[n++] = *path++;
        comp[n] = 0;
        while (*path == '/') path++;

        struct ext2_inode in;
        if (read_inode(cur, &in) != 0) return 0;
        if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;
        uint8_t t;
        uint32_t nxt = dir_lookup_in(&in, comp, &t);
        parent = cur;
        strncpy(last, comp, sizeof(last)-1);
        if (!nxt) {
            cur = 0;
            if (*path == 0) {
                /* not found, but caller may want parent + basename */
                if (out_parent_ino) *out_parent_ino = parent;
                if (out_basename) strncpy(out_basename, last, basesz - 1), out_basename[basesz-1] = 0;
                return 0;
            }
            return 0;
        }
        cur = nxt;
    }
    if (out_parent_ino) *out_parent_ino = parent;
    if (out_basename)   strncpy(out_basename, last, basesz - 1), out_basename[basesz - 1] = 0;
    return cur;
}

/* ---- vnode cache ---- */

static struct ext2_vinfo *vinfo_get(uint32_t ino) {
    for (int i = 0; i < EXT2_MAX_OPEN; i++) {
        if (vcache[i].in_use && vcache[i].inode_no == ino) {
            return &vcache[i];
        }
    }
    for (int i = 0; i < EXT2_MAX_OPEN; i++) {
        if (!vcache[i].in_use) {
            struct ext2_vinfo *v = &vcache[i];
            memset(v, 0, sizeof(*v));
            if (read_inode(ino, &v->inode) != 0) return NULL;
            v->in_use = 1;
            v->inode_no = ino;
            uint32_t mode = v->inode.i_mode & EXT2_S_IFMT;
            v->vnode.type = (mode == EXT2_S_IFDIR) ? VFS_TYPE_DIR :
                            (mode == EXT2_S_IFLNK) ? VFS_TYPE_SYMLINK : VFS_TYPE_FILE;
            v->vnode.mode = v->inode.i_mode & 0xFFF;
            v->vnode.uid  = v->inode.i_uid;
            v->vnode.gid  = v->inode.i_gid;
            v->vnode.size = v->inode.i_size;
            v->vnode.ops  = &ext2_ops;
            v->vnode.fs_data = v;
            v->vnode.inode_id = ino;
            v->vnode.atime = v->inode.i_atime;
            v->vnode.ctime = v->inode.i_ctime;
            v->vnode.mtime = v->inode.i_mtime;
            return v;
        }
    }
    return NULL;
}

static void vinfo_drop(uint32_t ino) {
    for (int i = 0; i < EXT2_MAX_OPEN; i++) {
        if (vcache[i].in_use && vcache[i].inode_no == ino) {
            vcache[i].in_use = 0;
            return;
        }
    }
}

/* ---- VFS surface ---- */

static struct vnode *ext2_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (!es.mounted) return NULL;
    if (path[0] == 0) {
        struct ext2_vinfo *v = vinfo_get(EXT2_ROOT_INO);
        return v ? &v->vnode : NULL;
    }
    uint32_t ino = path_to_ino(path, NULL, NULL, 0);
    if (!ino) return NULL;
    struct ext2_vinfo *v = vinfo_get(ino);
    return v ? &v->vnode : NULL;
}

static struct vnode *ext2_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!es.mounted) return NULL;
    uint32_t parent;
    char base[256];
    uint32_t ino = path_to_ino(path, &parent, base, sizeof(base));
    if (ino) {
        struct ext2_vinfo *v = vinfo_get(ino);
        return v ? &v->vnode : NULL;
    }
    if (!base[0]) return NULL;

    struct ext2_inode pin;
    if (read_inode(parent, &pin) != 0) return NULL;
    uint32_t new_ino = alloc_inode(0);
    if (!new_ino) return NULL;
    struct ext2_inode ni;
    memset(&ni, 0, sizeof(ni));
    ni.i_mode = EXT2_S_IFREG | 0644;
    ni.i_links_count = 1;
    ni.i_size = 0;
    ni.i_blocks = 0;
    ni.i_atime = ni.i_ctime = ni.i_mtime = ext2_now();
    write_inode(new_ino, &ni);
    if (dir_insert(parent, &pin, base, new_ino, EXT2_FT_REG_FILE) != 0) {
        free_inode(new_ino, 0);
        return NULL;
    }
    struct ext2_vinfo *v = vinfo_get(new_ino);
    return v ? &v->vnode : NULL;
}

static int64_t ext2_read_op(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct ext2_vinfo *v = (struct ext2_vinfo *)vn->fs_data;
    if (!v) return -1;
    /* Refresh size from cached inode in case of concurrent grow. */
    int64_t n = inode_read(&v->inode, pos, buf, count);
    if (n > 0) {
        v->inode.i_atime = ext2_now();
        v->vnode.atime = v->inode.i_atime;
        write_inode(v->inode_no, &v->inode);
    }
    return n;
}

static int64_t ext2_write_op(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct ext2_vinfo *v = (struct ext2_vinfo *)vn->fs_data;
    if (!v) return -1;
    int64_t n = inode_write(v->inode_no, &v->inode, pos, buf, count);
    if (n > 0) {
        v->vnode.size = v->inode.i_size;
        v->vnode.mtime = v->inode.i_mtime;
        v->vnode.ctime = v->inode.i_ctime;
    }
    return n;
}

struct readdir_ctx { struct vfs_dirent *out; int n, max; };
static int readdir_cb(const char *name, uint32_t ino, uint8_t type, void *user) {
    struct readdir_ctx *c = (struct readdir_ctx *)user;
    if (c->n >= c->max) return 1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    memset(&c->out[c->n], 0, sizeof(c->out[c->n]));
    strncpy(c->out[c->n].name, name, VFS_PATH_MAX - 1);
    c->out[c->n].inode = ino;
    if (type == EXT2_FT_DIR)         c->out[c->n].type = VFS_TYPE_DIR;
    else if (type == EXT2_FT_SYMLINK)c->out[c->n].type = VFS_TYPE_SYMLINK;
    else                             c->out[c->n].type = VFS_TYPE_FILE;
    /* For size, we'd have to read the inode — skip for performance. */
    c->n++;
    return 0;
}

static int ext2_readdir_op(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct ext2_vinfo *v = (struct ext2_vinfo *)vn->fs_data;
    if (!v) return -1;
    struct readdir_ctx c = { out, 0, max };
    if (dir_iterate(&v->inode, readdir_cb, &c) != 0) return -1;
    /* Patch sizes by re-reading each entry's inode (cheap-ish, small dirs). */
    for (int i = 0; i < c.n; i++) {
        struct ext2_inode tmp;
        if (read_inode((uint32_t)out[i].inode, &tmp) == 0) {
            out[i].size = tmp.i_size;
        }
    }
    return c.n;
}

static int ext2_mkdir_op(void *fs_data, const char *path) {
    (void)fs_data;
    if (!es.mounted) return -1;
    uint32_t parent;
    char base[256];
    uint32_t ino = path_to_ino(path, &parent, base, sizeof(base));
    if (ino) return -1;
    if (!base[0]) return -1;

    struct ext2_inode pin;
    if (read_inode(parent, &pin) != 0) return -1;

    uint32_t new_ino = alloc_inode(1);
    if (!new_ino) return -1;
    struct ext2_inode ni;
    memset(&ni, 0, sizeof(ni));
    ni.i_mode = EXT2_S_IFDIR | 0755;
    ni.i_links_count = 2;        /* "." */
    ni.i_size = 0;
    ni.i_atime = ni.i_ctime = ni.i_mtime = ext2_now();

    /* Allocate the first directory block and write "." and ".." entries. */
    uint32_t b = alloc_data_block();
    if (!b) { free_inode(new_ino, 1); return -1; }
    ni.i_block[0] = b;
    ni.i_blocks = es.block_size / 512;
    ni.i_size = es.block_size;
    memset(block_buf, 0, es.block_size);
    struct ext2_dir_entry *e1 = (struct ext2_dir_entry *)block_buf;
    e1->inode = new_ino;
    e1->rec_len = align4(8 + 1);
    e1->name_len = 1;
    e1->file_type = EXT2_FT_DIR;
    e1->name[0] = '.';
    struct ext2_dir_entry *e2 = (struct ext2_dir_entry *)(block_buf + e1->rec_len);
    e2->inode = parent;
    e2->rec_len = (uint16_t)(es.block_size - e1->rec_len);
    e2->name_len = 2;
    e2->file_type = EXT2_FT_DIR;
    e2->name[0] = '.'; e2->name[1] = '.';
    write_block(b, block_buf);
    write_inode(new_ino, &ni);

    if (dir_insert(parent, &pin, base, new_ino, EXT2_FT_DIR) != 0) {
        free_data_block(b);
        free_inode(new_ino, 1);
        return -1;
    }
    pin.i_links_count++;        /* parent gets one for ".." */
    pin.i_mtime = pin.i_ctime = ext2_now();
    write_inode(parent, &pin);
    return 0;
}

static int ext2_unlink_op(void *fs_data, const char *path) {
    (void)fs_data;
    if (!es.mounted) return -1;
    uint32_t parent;
    char base[256];
    uint32_t ino = path_to_ino(path, &parent, base, sizeof(base));
    if (!ino) return -1;

    struct ext2_inode in;
    if (read_inode(ino, &in) != 0) return -1;
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -1;

    struct ext2_inode pin;
    if (read_inode(parent, &pin) != 0) return -1;
    uint32_t removed; uint8_t t;
    if (dir_remove(parent, &pin, base, &removed, &t) != 0) return -1;

    in.i_links_count--;
    if (in.i_links_count == 0) {
        free_all_blocks(&in);
        in.i_size = 0;
        in.i_dtime = ext2_now();
        in.i_ctime = in.i_dtime;
        write_inode(ino, &in);
        free_inode(ino, 0);
    } else {
        in.i_ctime = ext2_now();
        write_inode(ino, &in);
    }
    pin.i_mtime = pin.i_ctime = ext2_now();
    write_inode(parent, &pin);
    vinfo_drop(ino);
    return 0;
}

static int ext2_is_dir_empty(struct ext2_inode *in) {
    /* Empty = only "." and ".." entries. */
    int cnt = 0;
    uint64_t pos = 0;
    while (pos < in->i_size) {
        uint32_t fblock = (uint32_t)(pos / es.block_size);
        uint32_t bno    = bmap(in, fblock, 0);
        if (!bno) { pos += es.block_size; continue; }
        if (read_block(bno, block_buf) != 0) return 0;
        uint32_t off = 0;
        while (off < es.block_size) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0) {
                if (de->name_len == 1 && de->name[0] == '.') { /* . */ }
                else if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') { /* .. */ }
                else return 0;
                cnt++;
            }
            off += de->rec_len;
        }
        pos += es.block_size;
    }
    return cnt <= 2;
}

static int ext2_rmdir_op(void *fs_data, const char *path) {
    (void)fs_data;
    if (!es.mounted) return -1;
    uint32_t parent;
    char base[256];
    uint32_t ino = path_to_ino(path, &parent, base, sizeof(base));
    if (!ino) return -1;
    if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) return -1;

    struct ext2_inode in;
    if (read_inode(ino, &in) != 0) return -1;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
    if (!ext2_is_dir_empty(&in)) return -1;

    struct ext2_inode pin;
    if (read_inode(parent, &pin) != 0) return -1;
    uint32_t removed; uint8_t t;
    if (dir_remove(parent, &pin, base, &removed, &t) != 0) return -1;

    free_all_blocks(&in);
    in.i_links_count = 0;
    in.i_size = 0;
    in.i_dtime = ext2_now();
    in.i_ctime = in.i_dtime;
    write_inode(ino, &in);
    free_inode(ino, 1);
    if (pin.i_links_count > 2) pin.i_links_count--;
    pin.i_mtime = pin.i_ctime = ext2_now();
    write_inode(parent, &pin);
    vinfo_drop(ino);
    return 0;
}

static int ext2_rename_op(void *fs_data, const char *from, const char *to) {
    (void)fs_data;
    if (!es.mounted) return -1;
    uint32_t pf, pt;
    char bf[256], bt[256];
    uint32_t ino_from = path_to_ino(from, &pf, bf, sizeof(bf));
    if (!ino_from) return -1;
    uint32_t ino_to   = path_to_ino(to,   &pt, bt, sizeof(bt));
    if (ino_to) return -1;       /* destination exists — refuse */
    if (!bt[0]) return -1;
    /* Determine the file_type from the source inode. */
    struct ext2_inode src;
    if (read_inode(ino_from, &src) != 0) return -1;
    uint8_t ft = ((src.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? EXT2_FT_DIR :
                 ((src.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) ? EXT2_FT_SYMLINK :
                 EXT2_FT_REG_FILE;
    /* Remove from old parent, insert into new. */
    struct ext2_inode pin_from, pin_to;
    if (read_inode(pf, &pin_from) != 0) return -1;
    if (read_inode(pt, &pin_to)   != 0) return -1;
    uint32_t rmv; uint8_t rt;
    if (dir_remove(pf, &pin_from, bf, &rmv, &rt) != 0) return -1;
    if (dir_insert(pt, &pin_to, bt, ino_from, ft) != 0) return -1;
    /* If we moved a directory between parents, fix link counts on parents. */
    src.i_ctime = ext2_now();
    write_inode(ino_from, &src);
    pin_from.i_mtime = pin_from.i_ctime = ext2_now();
    pin_to.i_mtime = pin_to.i_ctime = pin_from.i_ctime;
    if (ft == EXT2_FT_DIR && pf != pt) {
        if (pin_from.i_links_count > 2) pin_from.i_links_count--;
        pin_to.i_links_count++;
    }
    write_inode(pf, &pin_from);
    write_inode(pt, &pin_to);
    return 0;
}

static int ext2_truncate_op(struct vnode *vn, uint64_t new_size) {
    struct ext2_vinfo *v = (struct ext2_vinfo *)vn->fs_data;
    if (!v) return -1;
    if (new_size > 0xFFFFFFFFu) return -1;
    if (new_size < v->inode.i_size) {
        /* Free blocks beyond new size. */
        uint32_t new_blks = (uint32_t)((new_size + es.block_size - 1) / es.block_size);
        uint32_t old_blks = (v->inode.i_size + es.block_size - 1) / es.block_size;
        for (uint32_t i = new_blks; i < old_blks; i++) {
            uint32_t b = bmap(&v->inode, i, 0);
            if (b) free_data_block(b);
        }
        /* For simplicity we leave indirect-pointer leak-cleanup to free_all_blocks,
         * which is invoked only at unlink; partial shrink leaves index blocks. */
    }
    v->inode.i_size = (uint32_t)new_size;
    v->inode.i_mtime = v->inode.i_ctime = ext2_now();
    v->vnode.size = new_size;
    v->vnode.mtime = v->inode.i_mtime;
    v->vnode.ctime = v->inode.i_ctime;
    write_inode(v->inode_no, &v->inode);
    return 0;
}

static int ext2_stat_op(struct vnode *vn, struct vfs_stat *out) {
    struct ext2_vinfo *v = (struct ext2_vinfo *)vn->fs_data;
    if (!v) return -1;
    memset(out, 0, sizeof(*out));
    out->type   = vn->type;
    out->mode   = v->inode.i_mode & 0xFFF;
    out->uid    = v->inode.i_uid;
    out->gid    = v->inode.i_gid;
    out->size   = v->inode.i_size;
    out->inode  = v->inode_no;
    out->nlink  = v->inode.i_links_count;
    out->blocks = v->inode.i_blocks;
    out->mtime  = v->inode.i_mtime;
    out->ctime  = v->inode.i_ctime;
    out->atime  = v->inode.i_atime;
    return 0;
}

static int ext2_chmod_op(struct vnode *vn, uint32_t mode) {
    struct ext2_vinfo *v = (struct ext2_vinfo *)vn->fs_data;
    if (!v) return -1;
    v->inode.i_mode = (v->inode.i_mode & ~0xFFFu) | (mode & 0xFFFu);
    v->inode.i_ctime = ext2_now();
    v->vnode.ctime = v->inode.i_ctime;
    write_inode(v->inode_no, &v->inode);
    return 0;
}

static int ext2_chown_op(struct vnode *vn, uint32_t uid, uint32_t gid) {
    struct ext2_vinfo *v = (struct ext2_vinfo *)vn->fs_data;
    if (!v) return -1;
    if (uid != (uint32_t)-1) v->inode.i_uid = (uint16_t)uid;
    if (gid != (uint32_t)-1) v->inode.i_gid = (uint16_t)gid;
    v->inode.i_ctime = ext2_now();
    v->vnode.ctime = v->inode.i_ctime;
    write_inode(v->inode_no, &v->inode);
    return 0;
}

const struct vfs_ops ext2_ops = {
    .lookup   = ext2_lookup,
    .create   = ext2_create,
    .read     = ext2_read_op,
    .write    = ext2_write_op,
    .readdir  = ext2_readdir_op,
    .mkdir    = ext2_mkdir_op,
    .rmdir    = ext2_rmdir_op,
    .unlink   = ext2_unlink_op,
    .rename   = ext2_rename_op,
    .stat     = ext2_stat_op,
    .truncate = ext2_truncate_op,
    .chmod    = ext2_chmod_op,
    .chown    = ext2_chown_op,
};

/* ---- mkfs.ext2 (in-kernel formatter) ---- */

/* Build a minimal ext2 filesystem on the current AHCI port covering
 * `total_blocks` blocks of `block_size` bytes (block_size in {1024,2048,4096}).
 * The volume is single-group, with 16 KiB inode-table. */
static int format_default(uint32_t total_blocks, uint32_t bsize) {
    /* Clamp sane defaults. */
    if (bsize != 1024 && bsize != 2048 && bsize != 4096) bsize = 1024;
    if (total_blocks < 64) total_blocks = 64;
    uint32_t inodes_per_group = 128;
    uint32_t blocks_per_group = total_blocks;
    uint32_t inode_table_blocks =
        (inodes_per_group * EXT2_GOOD_OLD_INODE_SIZE + bsize - 1) / bsize;

    /* Block layout for single group (block size 1024):
     *   block 0    : superblock (lives at byte 1024..2047 of FS; for 2048/4096
     *                block sizes, sb shares block 0)
     *   block 1    : GDT
     *   block 2    : block bitmap
     *   block 3    : inode bitmap
     *   block 4..  : inode table
     *   block 4+itb..: data blocks (root dir takes first one)
     */
    es.block_size       = bsize;
    es.inodes_per_group = inodes_per_group;
    es.blocks_per_group = blocks_per_group;
    es.inode_size       = EXT2_GOOD_OLD_INODE_SIZE;
    es.inodes_count     = inodes_per_group;
    es.blocks_count     = total_blocks;
    es.first_data_block = (bsize == 1024) ? 1 : 0;
    es.first_ino        = EXT2_FIRST_USER_INO;
    es.group_count      = 1;
    if (!block_buf) block_buf = (uint8_t *)kmalloc(bsize > 4096 ? bsize : 4096);

    uint32_t gdt_blk    = gdt_start_block();
    uint32_t bbm_blk    = gdt_blk + 1;
    uint32_t ibm_blk    = bbm_blk + 1;
    uint32_t itab_blk   = ibm_blk + 1;
    uint32_t first_data = itab_blk + inode_table_blocks;
    uint32_t root_blk   = first_data;   /* root dir gets first data block */

    /* Set superblock. */
    memset(&es.sb, 0, sizeof(es.sb));
    es.sb.s_inodes_count       = es.inodes_count;
    es.sb.s_blocks_count       = es.blocks_count;
    es.sb.s_r_blocks_count     = 0;
    es.sb.s_free_blocks_count  = es.blocks_count - (first_data + 1);   /* minus root */
    es.sb.s_free_inodes_count  = es.inodes_count - (EXT2_FIRST_USER_INO - 1);
    es.sb.s_first_data_block   = es.first_data_block;
    /* s_log_block_size: 0=1024, 1=2048, 2=4096 */
    es.sb.s_log_block_size     = (bsize == 1024) ? 0 : (bsize == 2048) ? 1 : 2;
    es.sb.s_log_frag_size      = es.sb.s_log_block_size;
    es.sb.s_blocks_per_group   = blocks_per_group;
    es.sb.s_frags_per_group    = blocks_per_group;
    es.sb.s_inodes_per_group   = inodes_per_group;
    es.sb.s_mtime              = ext2_now();
    es.sb.s_wtime              = es.sb.s_mtime;
    es.sb.s_mnt_count          = 0;
    es.sb.s_max_mnt_count      = 20;
    es.sb.s_magic              = EXT2_MAGIC;
    es.sb.s_state              = 1;     /* clean */
    es.sb.s_errors             = 1;     /* continue */
    es.sb.s_rev_level          = EXT2_DYNAMIC_REV;
    es.sb.s_first_ino          = EXT2_FIRST_USER_INO;
    es.sb.s_inode_size         = EXT2_GOOD_OLD_INODE_SIZE;
    es.sb.s_creator_os         = 0;     /* Linux */
    memcpy(es.sb.s_volume_name, "AURALITE", 8);

    if (write_super() != 0) return -1;

    /* Allocate + write GDT. */
    if (es.gdt) kfree(es.gdt);
    uint32_t gdt_bytes = sizeof(struct ext2_group_desc);
    es.gdt = (struct ext2_group_desc *)kmalloc(es.block_size);
    if (!es.gdt) return -1;
    memset(es.gdt, 0, es.block_size);
    es.gdt[0].bg_block_bitmap     = bbm_blk;
    es.gdt[0].bg_inode_bitmap     = ibm_blk;
    es.gdt[0].bg_inode_table      = itab_blk;
    es.gdt[0].bg_free_blocks_count = (uint16_t)(es.blocks_count - (first_data + 1));
    es.gdt[0].bg_free_inodes_count = (uint16_t)(es.inodes_count - (EXT2_FIRST_USER_INO - 1));
    es.gdt[0].bg_used_dirs_count   = 1;   /* root */
    (void)gdt_bytes;
    write_gdt();

    /* Zero block bitmap, then mark used blocks. */
    memset(block_buf, 0, es.block_size);
    /* Block bitmap covers blocks [first_data_block .. first_data_block+blocks_per_group)
     * (i.e., bit 0 = first_data_block).  For block_size=1024, first_data_block=1, so
     * bit 0 represents block #1.  We mark gdt/bbm/ibm/itab/root_blk used. */
    uint32_t used_blks[] = { gdt_blk, bbm_blk, ibm_blk, root_blk };
    for (unsigned k = 0; k < sizeof(used_blks)/sizeof(used_blks[0]); k++) {
        uint32_t off = used_blks[k] - es.first_data_block;
        if (off < es.blocks_per_group) bitmap_set(block_buf, off);
    }
    /* Inode table blocks. */
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        uint32_t off = (itab_blk + i) - es.first_data_block;
        if (off < es.blocks_per_group) bitmap_set(block_buf, off);
    }
    write_block(bbm_blk, block_buf);

    /* Inode bitmap: mark inodes 1..(EXT2_FIRST_USER_INO-1) used. */
    memset(block_buf, 0, es.block_size);
    for (uint32_t i = 0; i < EXT2_FIRST_USER_INO - 1; i++) bitmap_set(block_buf, i);
    write_block(ibm_blk, block_buf);

    /* Zero inode table. */
    memset(block_buf, 0, es.block_size);
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        write_block(itab_blk + i, block_buf);
    }

    /* Write root inode (inode #2). */
    struct ext2_inode root;
    memset(&root, 0, sizeof(root));
    root.i_mode  = EXT2_S_IFDIR | 0755;
    root.i_size  = es.block_size;
    root.i_links_count = 3;   /* "." and parent "." (root) + the dir entry */
    root.i_atime = root.i_ctime = root.i_mtime = ext2_now();
    root.i_block[0] = root_blk;
    root.i_blocks = es.block_size / 512;
    write_inode(EXT2_ROOT_INO, &root);

    /* Root dir contents: "." and "..". */
    memset(block_buf, 0, es.block_size);
    struct ext2_dir_entry *e1 = (struct ext2_dir_entry *)block_buf;
    e1->inode = EXT2_ROOT_INO;
    e1->rec_len = align4(8 + 1);
    e1->name_len = 1;
    e1->file_type = EXT2_FT_DIR;
    e1->name[0] = '.';
    struct ext2_dir_entry *e2 = (struct ext2_dir_entry *)(block_buf + e1->rec_len);
    e2->inode = EXT2_ROOT_INO;
    e2->rec_len = (uint16_t)(es.block_size - e1->rec_len);
    e2->name_len = 2;
    e2->file_type = EXT2_FT_DIR;
    e2->name[0] = '.'; e2->name[1] = '.';
    write_block(root_blk, block_buf);

    return 0;
}

/* Parse what's on disk; if no valid ext2, format it. */
static int parse_or_format(void) {
    /* Try block_size = 1024 first (most common for small disks). */
    es.block_size = 1024;
    if (read_super() != 0) return -1;
    if (es.sb.s_magic == EXT2_MAGIC) {
        uint32_t bs = 1024u << es.sb.s_log_block_size;
        if (bs != 1024 && bs != 2048 && bs != 4096) return -1;
        es.block_size       = bs;
        es.inodes_per_group = es.sb.s_inodes_per_group;
        es.blocks_per_group = es.sb.s_blocks_per_group;
        es.inode_size       = (es.sb.s_rev_level == EXT2_DYNAMIC_REV) ?
                              es.sb.s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
        if (es.inode_size == 0) es.inode_size = EXT2_GOOD_OLD_INODE_SIZE;
        es.inodes_count     = es.sb.s_inodes_count;
        es.blocks_count     = es.sb.s_blocks_count;
        es.first_data_block = es.sb.s_first_data_block;
        es.first_ino        = (es.sb.s_rev_level == EXT2_DYNAMIC_REV &&
                                  es.sb.s_first_ino) ? es.sb.s_first_ino : EXT2_FIRST_USER_INO;
        es.group_count      = (es.blocks_count - es.first_data_block +
                                es.blocks_per_group - 1) / es.blocks_per_group;
        if (es.group_count == 0) return -1;
        if (!block_buf) block_buf = (uint8_t *)kmalloc(es.block_size > 4096 ? es.block_size : 4096);
        if (read_gdt() != 0) return -1;
        kprintf("[ext2] mounted existing volume: block_size=%u, groups=%u, "
                "blocks=%u, inodes=%u (free %u blocks / %u inodes)\n",
                es.block_size, es.group_count, es.blocks_count, es.inodes_count,
                es.sb.s_free_blocks_count, es.sb.s_free_inodes_count);
        return 0;
    }
    /* No ext2 → format the whole disk (use whatever capacity AHCI exposed). */
    /* We don't know the exact disk size cheaply; assume the disk image is
     * sized for the volume and format it as 4 MiB / 1 KiB blocks. */
    kprintf("[ext2] no ext2 magic at offset 1024; formatting fresh volume...\n");
    return format_default(4096, 1024);
}

/* ---- Lifecycle ---- */

void ext2_list(void) {
    if (!es.mounted) return;
    struct vfs_dirent *ents = kmalloc(64 * sizeof(struct vfs_dirent));
    if (!ents) return;
    struct ext2_vinfo *root = vinfo_get(EXT2_ROOT_INO);
    if (!root) {
        kfree(ents);
        return;
    }
    int n = ext2_readdir_op(&root->vnode, ents, 64);
    for (int i = 0; i < n; i++) {
        const char *slash = (ents[i].type == VFS_TYPE_DIR) ? "/" : "";
        kprintf("  /ext2/%s%s  (%llu bytes)\n",
                ents[i].name, slash, (unsigned long long)ents[i].size);
    }
    kfree(ents);
}

int ext2_init(int prefer_port) {
    memset(&es, 0, sizeof(es));
    memset(vcache, 0, sizeof(vcache));

    /* Pick AHCI port: prefer requested, then 2nd disk, then 1st. */
    int p = -1;
    if (prefer_port >= 0) p = prefer_port;
    if (p < 0) p = ahci_get_nth_port(1);
    if (p < 0) p = ahci_get_first_port();
    if (p < 0) {
        kprintf("[ext2] no AHCI disk available; not mounted\n");
        return -1;
    }
    es.ahci_port    = p;
    es.fs_base_lba  = 0;     /* assume whole-disk filesystem; offset = 0 */
    es.fs_lba_count = 0;     /* unused */

    if (parse_or_format() != 0) {
        kprintf("[ext2] failed to mount/format\n");
        return -1;
    }
    es.mounted = 1;
    kprintf("[ext2] AHCI port %d mounted at /ext2\n", p);
    return 0;
}

void ext2_self_test(void) {
    if (!es.mounted) return;
    kprintf("[ext2] self-test: file/dir/indirect/rename...\n");

    /* Idempotency: clean up from any earlier boot. */
    ext2_unlink_op(NULL, "TEST.TXT");
    ext2_unlink_op(NULL, "RENAMED.TXT");
    ext2_unlink_op(NULL, "DIR/FILE.TXT");
    ext2_rmdir_op (NULL, "DIR");
    ext2_unlink_op(NULL, "BIG.BIN");

    /* 1. Simple file. */
    struct vnode *vn = ext2_create(NULL, "TEST.TXT");
    if (!vn) { kprintf("[ext2] FAIL: create TEST.TXT\n"); return; }
    const char *m = "hello ext2";
    if (ext2_write_op(vn, 0, m, strlen(m)) != (int64_t)strlen(m)) {
        kprintf("[ext2] FAIL: write TEST.TXT\n"); return;
    }
    char buf[64] = {0};
    if (ext2_read_op(vn, 0, buf, 63) != (int64_t)strlen(m) || strcmp(buf, m) != 0) {
        kprintf("[ext2] FAIL: readback TEST.TXT: '%s'\n", buf); return;
    }

    /* 2. mkdir + nested file. */
    if (ext2_mkdir_op(NULL, "DIR") != 0) { kprintf("[ext2] FAIL: mkdir\n"); return; }
    vn = ext2_create(NULL, "DIR/FILE.TXT");
    if (!vn) { kprintf("[ext2] FAIL: create nested\n"); return; }
    const char *m2 = "nested ext2 file";
    if (ext2_write_op(vn, 0, m2, strlen(m2)) != (int64_t)strlen(m2)) {
        kprintf("[ext2] FAIL: write nested\n"); return;
    }
    memset(buf, 0, sizeof(buf));
    struct vnode *vn2 = ext2_lookup(NULL, "DIR/FILE.TXT");
    if (!vn2 || ext2_read_op(vn2, 0, buf, 63) != (int64_t)strlen(m2) ||
        strcmp(buf, m2) != 0) {
        kprintf("[ext2] FAIL: nested readback\n"); return;
    }

    /* 3. Rename. */
    if (ext2_rename_op(NULL, "TEST.TXT", "RENAMED.TXT") != 0) {
        kprintf("[ext2] FAIL: rename\n"); return;
    }
    if (ext2_lookup(NULL, "TEST.TXT") != NULL) {
        kprintf("[ext2] FAIL: old name still present\n"); return;
    }

    /* 4. Indirect-block stress: write a 20 KiB file at block_size=1024 to
     * force allocation in the single-indirect region. */
    vn = ext2_create(NULL, "BIG.BIN");
    if (!vn) { kprintf("[ext2] FAIL: create BIG.BIN\n"); return; }
    uint8_t chunk[1024];
    for (int i = 0; i < (int)sizeof(chunk); i++) chunk[i] = (uint8_t)(i & 0xFF);
    uint64_t total = 0;
    for (int i = 0; i < 20; i++) {
        if (ext2_write_op(vn, total, chunk, sizeof(chunk)) != (int64_t)sizeof(chunk)) {
            kprintf("[ext2] FAIL: indirect write at %llu\n", (unsigned long long)total);
            return;
        }
        total += sizeof(chunk);
    }
    /* Read back the last chunk to verify. */
    uint8_t back[1024];
    if (ext2_read_op(vn, total - sizeof(back), back, sizeof(back)) != (int64_t)sizeof(back)) {
        kprintf("[ext2] FAIL: indirect readback\n"); return;
    }
    for (int i = 0; i < (int)sizeof(back); i++) {
        if (back[i] != chunk[i]) {
            kprintf("[ext2] FAIL: indirect mismatch byte %d (%u vs %u)\n",
                    i, back[i], chunk[i]);
            return;
        }
    }

    kprintf("[ext2] PASS: ext2 read/write/dir/indirect works\n");
}
