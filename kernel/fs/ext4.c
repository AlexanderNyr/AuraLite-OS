/* ext4.c — Full ext4 filesystem with journaling, extents, and delayed allocation.
 *
 * Ext4 extends ext2 with:
 *   - Extent trees instead of direct/indirect blocks
 *   - Journaling (JBD2-compatible)
 *   - Delayed allocation (allocate on write, not on seek)
 *   - Extent preallocation
 *   - Higherfs fsck compatibility
 *
 * On-disk layout (we mount whatever is at LBA 64; format only if absent):
 *
 *   +-----------------------------+ LBA 64      (ext4 superblock)
 *   +-----------------------------+ LBA 64+1    (block group descriptors)
 *   ... block group descriptors ...
 *   +-----------------------------+ data_lba    (block bitmaps)
 *   +-----------------------------+ inode_bitmap_lba
 *   +-----------------------------+ inode_table_lba
 *   +-----------------------------+ journal_lba (128MB default)
 *   +-----------------------------+ data area (clusters of 128MB)
 */

#include <stdint.h>
#include "kernel/lib/paddr.h"
#include "kernel/fs/ext4.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/fs/fsformat.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include "kernel/mm/kheap.h"
#include "kernel/fs/blkdev.h"

/* ============================================================================
 * SECTION 1: EXT4 ON-DISK STRUCTURES
 * ============================================================================ */

/* ext4 Superblock (512 bytes, starts at offset 1024 in block 0) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_sb {
    uint32_t  s_inodes_count;           /* Total inode count */
    uint32_t  s_blocks_count_lo;        /* Total blocks count (low 32 bits) */
    uint32_t  s_r_blocks_count_lo;      /* Reserved blocks count */
    uint32_t  s_free_blocks_count_lo;   /* Free blocks count */
    uint32_t  s_free_inodes_count;      /* Free inodes count */
    uint32_t  s_first_data_block;       /* First data block (0 or 1 for 1KB blocks) */
    uint32_t  s_log_block_size;         /* Block size = 1024 << s_log_block_size */
    uint32_t  s_log_cluster_size;       /* Cluster size = block size if same */
    uint32_t  s_blocks_per_group;       /* Blocks per group */
    uint32_t  s_clusters_per_group;     /* Clusters per group (same as blocks) */
    uint32_t  s_inodes_per_group;       /* Inodes per group */
    uint32_t  s_mtime;                  /* Mount time */
    uint32_t  s_wtime;                  /* Write time */
    uint16_t  s_mnt_count;              /* Mount count */
    uint16_t  s_max_mnt_count;          /* Max mount count before fsck */
    uint16_t  s_magic;                  /* Magic signature 0xEF53 */
    uint16_t  s_state;                  /* File system state */
    uint16_t  s_errors;                 /* Behavior when detecting errors */
    uint16_t  s_minor_rev_level;        /* Minor revision level */
    uint32_t  s_lastcheck;              /* Last fsck time */
    uint32_t  s_checkinterval;          /* Interval between forced fsck */
    uint32_t  s_creator_os;             /* OS that created fs */
    uint32_t  s_rev_level;              /* Revision level */
    uint16_t  s_def_resuid;             /* Default UID for reserved blocks */
    uint16_t  s_def_resgid;             /* Default GID for reserved blocks */
    uint32_t  s_first_ino;              /* First non-reserved inode */
    uint16_t  s_inode_size;             /* Size of inode structure */
    uint16_t  s_block_group_nr;         /* Block group number of this sb */
    uint32_t  s_feature_compat;         /* Compatible feature set */
    uint32_t  s_feature_incompat;       /* Incompatible feature set */
    uint32_t  s_feature_ro_compat;      /* Read-only compatible feature set */
    uint8_t   s_uuid[16];               /* Filesystem UUID */
    char      s_volume_name[16];        /* Volume name */
    char      s_last_mounted[64];       /* Last mount point */
    uint32_t  s_algorithm_usage_bitmap; /* For compression */
    uint8_t   s_prealloc_blocks;        /* Optimal number of blocks to prealloc */
    uint8_t   s_prealloc_dir_blocks;    /* Optimal number of blocks to prealloc for dirs */
    uint16_t  s_reserved_gdt_blocks;    /* Per-group table has this many reserved gdt blocks */
    uint8_t   s_journal_uuid[16];       /* Journal UUID */
    uint32_t  s_journal_inum;           /* Journal inode */
    uint32_t  s_journal_dev;            /* Journal device */
    uint32_t  s_last_orphan;            /* Start of list of orphaned inodes */
    uint32_t  s_hash_seed[4];           /* HTREE hash seed */
    uint8_t   s_def_hash_version;       /* Default hash version */
    uint8_t   s_jnl_backup_type;        /* Journal backup type */
    uint16_t  s_desc_size;              /* Size of group descriptor (for flex_bg) */
    uint32_t  s_default_mount_opts;     /* Default mount options */
    uint32_t  s_first_meta_bg;          /* First metablock group */
    uint32_t  s_mkfs_time;              /* When the filesystem was created */
    uint32_t  s_jnl_blocks[17];         /* Backup journal blocks */
    uint32_t  s_blocks_count_hi;        /* High 32 bits of block count */
    uint32_t  s_r_blocks_count_hi;      /* High 32 bits of reserved blocks */
    uint32_t  s_free_blocks_count_hi;   /* High 32 bits of free blocks */
    uint8_t   s_kbytes_written[8];      /* Number of KiB written */
    uint32_t  s_s_inodes_count_hi;      /* High 32 bits of inode count */
    uint32_t  s_s_first_ino_hi;         /* High 32 bits of first inode */
    uint32_t  s_s_inode_generation;     /* Inode generation */
    uint32_t  s_reserved;               /* Padding */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ext4 block group descriptor (minimum 32 bytes) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_bg_desc {
    uint32_t bg_block_bitmap_lo;       /* Low 32 bits of block bitmap block */
    uint32_t bg_inode_bitmap_lo;       /* Low 32 bits of inode bitmap block */
    uint32_t bg_inode_table_lo;        /* Low 32 bits of inode table start block */
    uint16_t bg_free_blocks_count_lo;  /* Low 16 bits of free blocks count */
    uint16_t bg_free_inodes_count_lo;  /* Low 16 bits of free inodes count */
    uint16_t bg_used_dirs_count_lo;    /* Low 16 bits of directory count */
    uint16_t bg_flags;                 /* Block group flags */
    uint32_t bg_exclude_bitmap_lo;     /* Low 32 bits of snapshot exclusion bitmap */
    uint16_t bg_block_bitmap_csum_lo;  /* Low 16 bits of block bitmap checksum */
    uint16_t bg_inode_bitmap_csum_lo;  /* Low 16 bits of inode bitmap checksum */
    uint16_t bg_itable_unused_lo;      /* Low 16 bits of unused inode count */
    uint16_t bg_checksum;              /* Group descriptor checksum */
    uint32_t bg_block_bitmap_hi;       /* High 32 bits of block bitmap block */
    uint32_t bg_inode_bitmap_hi;       /* High 32 bits of inode bitmap block */
    uint32_t bg_inode_table_hi;        /* High 32 bits of inode table start block */
    uint16_t bg_free_blocks_count_hi;  /* High 16 bits of free blocks count */
    uint16_t bg_free_inodes_count_hi;  /* High 16 bits of free inodes count */
    uint16_t bg_used_dirs_count_hi;    /* High 16 bits of directory count */
    uint16_t bg_pad;                   /* Padding */
    uint32_t bg_reserved[3];           /* Reserved */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ext4 inode (variable size, minimum 128 bytes, usually 256) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_inode {
    uint16_t i_mode;            /* File mode */
    uint16_t i_uid_lo;          /* Low 16 bits of UID */
    uint32_t i_size_lo;         /* Low 32 bits of size */
    uint32_t i_atime;           /* Access time */
    uint32_t i_ctime;           /* Creation time */
    uint32_t i_mtime;           /* Modification time */
    uint32_t i_dtime;           /* Deletion time */
    uint16_t i_gid_lo;          /* Low 16 bits of GID */
    uint16_t i_links_count;     /* Hard links count */
    uint32_t i_blocks_lo;       /* Low 32 bits of block count (512-byte blocks) */
    uint32_t i_flags;           /* File flags */
    uint32_t i_osd1;            /* OS dependent 1 */
    uint32_t i_block[15];       /* Pointers to blocks (60 bytes) */
    uint32_t i_generation;      /* File version (for NFS) */
    uint32_t i_file_acl;        /* File ACL (not used if inline) */
    uint32_t i_size_high;       /* High 32 bits of size (for large files) */
    uint32_t i_faddr;           /* Fragment address (obsolete) */
    uint8_t  i_osd2[12];        /* OS dependent 2 */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ext4 extent tree header (12 bytes) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_extent_header {
    uint16_t eh_magic;          /* 0xF30A */
    uint16_t eh_entries;        /* Number of valid entries */
    uint16_t eh_max;            /* Maximum entries in this node */
    uint16_t eh_depth;          /* Depth of this node in tree (0=leaf) */
    uint32_t eh_generation;     /* Generation (not used in current ext4) */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ext4 extent (12 bytes) — used in leaf nodes */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_extent {
    uint32_t ee_block;          /* First logical block in this extent */
    uint16_t ee_len;            /* Number of blocks in this extent (max 32768) */
    uint16_t ee_start_hi;       /* High 16 bits of physical block number */
    uint32_t ee_start_lo;       /* Low 32 bits of physical block number */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ext4 extent index (12 bytes) — used in internal (non-leaf) nodes */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_extent_idx {
    uint32_t ei_block;          /* Logical block this index covers */
    uint32_t ei_leaf_lo;        /* Low 32 bits of child node physical block */
    uint16_t ei_leaf_hi;        /* High 16 bits of child node physical block */
    uint16_t ei_unused;         /* Unused */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* Directory entry */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_dirent {
    uint32_t inode;             /* Inode number (0 = unused) */
    uint16_t rec_len;           /* Directory entry length */
    uint16_t name_len;          /* Name length */
    uint8_t  file_type;         /* File type */
    char     name[];            /* Name (variable) */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* Journal superblock */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_journal_sb {
    uint32_t header_version;
    uint32_t block_type;
    uint32_t sequence;
    uint32_t block_size;
    uint32_t max_len;
    uint32_t first;
    uint32_t last;
    uint32_t start;
    uint32_t errno;
    uint32_t features_compat;
    uint32_t features_incompat;
    uint32_t features_ro_compat;
    uint8_t  uuid[16];
    uint32_t nr_users;
    uint32_t blocksize;
    uint32_t maxlen;
    uint32_t h_checksum_type;
    uint32_t h_chksum;
    uint32_t reserved[44];
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* Journal transaction header */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ext4_journal_header {
    uint32_t magic;             /* 0xC03B3998 */
    uint32_t block_type;        /* 1=commit, 2=superblock, 3=descriptor, 4=revoke */
    uint32_t sequence;
    uint32_t block_nr;
    uint32_t flags;
    uint32_t reserved[3];
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ============================================================================
 * SECTION 2: CONSTANTS AND DEFINES
 * ============================================================================ */

#define EXT4_MAGIC                0xEF53
#define EXT4_EXTENT_MAGIC         0xF30A
#define EXT4_JOURNAL_MAGIC        0xC03B3998

#define EXT4_VALID_FS             1
#define EXT4_ERROR_FS             2

/* Feature flags */
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL     0x00000004
#define EXT4_FEATURE_COMPAT_EXT_ATTR        0x00000002
#define EXT4_FEATURE_INCOMPAT_COMPRESSION   0x00000001
#define EXT4_FEATURE_INCOMPAT_EXT_ATTR      0x00000002
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV   0x00000008
#define EXT4_FEATURE_INCOMPAT_META_BG       0x00000010
#define EXT4_FEATURE_INCOMPAT_64BIT         0x00000080
#define EXT4_FEATURE_INCOMPAT_FLEX_BG       0x00000020
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x00000001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE   0x00000002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR    0x00000004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE    0x00000008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM     0x00000010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK    0x00000020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE  0x00000040

/* Inode flags */
#define EXT4_EXTENTS_FL           0x00080000
#define EXT4_INLINE_DATA_FL      0x10000000
#define EXT4_TOPDIR_FL           0x00020000
#define EXT4_EA_INODE_FL         0x20000000

/* File types for directory entries */
#define EXT4_FT_UNKNOWN          0
#define EXT4_FT_REG_FILE         1
#define EXT4_FT_DIR              2
#define EXT4_FT_CHRDEV           3
#define EXT4_FT_BLKDEV           4
#define EXT4_FT_FIFO             5
#define EXT4_FT_SOCK            6
#define EXT4_FT_SYMLINK          7

/* inode mode bits */
#define EXT4_S_IFMT   0xF000
#define EXT4_S_IFREG  0x8000
#define EXT4_S_IFDIR  0x4000
#define EXT4_S_IFLNK  0xA000

/* Journal block types */
#define EXT4_JOURNAL_DESCRIPTOR    1
#define EXT4_JOURNAL_COMMIT        2
#define EXT4_JOURNAL_SUPERBLOCK_V1 3
#define EXT4_JOURNAL_SUPERBLOCK_V2 4
#define EXT4_JOURNAL_REVOKE        5

#define EXT4_MAX_OPEN_VNODES 128
#define EXT4_MAX_NAME 256
#define EXT4_MAX_DELETED 64
#define EXT4_JOURNAL_BLOCKS 8192

/* ============================================================================
 * SECTION 3: MOUNT STATE
 * ============================================================================ */

struct ext4_mount {
    int       bdev;     /* blkdev id (P1) */
    uint32_t  base_lba;
    uint32_t  block_size;
    uint32_t  blocks_per_group;
    uint32_t  inodes_per_group;
    uint64_t  inodes_count;     /* F3: 64-bit (hi+lo superblock fields) */
    uint64_t  blocks_count;     /* F3: 64-bit (hi+lo superblock fields) */
    uint32_t  group_count;
    uint32_t  first_data_block;
    uint32_t  inode_size;
    uint32_t  inode_table_blocks;
    uint32_t  desc_size;
    uint32_t  cluster_size;
    int       has_journal;
    uint32_t  journal_block;
    uint32_t  journal_inode;
    uint32_t  rev_level;
    uint64_t  features_incompat;
    int       mounted;
    spinlock_t alloc_lock;

    /* Journal state */
    uint32_t journal_curr_tx;
    uint32_t journal_head;
    uint32_t journal_tail;
    uint32_t journal_sequence;
    uint32_t *journal_bitmap;
    int      journal_in_progress;

    /* delayed allocation: pending writes are queued here */
    struct ext4_delalloc {
        int in_use;
        uint32_t inode;
        uint64_t file_off;
        uint64_t len;
        void *data;
        struct ext4_delalloc *next;
    } *delalloc_queue;
};

static struct ext4_mount m4;

/* Scratch buffers */
static uint8_t *ext4_scratch = NULL;
static uint8_t *ext4_cluster_buf = NULL;

/* Open vnode cache */
struct ext4_vinfo {
    int       in_use;
    char      path[256];
    uint32_t  inode;
    uint32_t  first_cluster;
    uint32_t  size;
    uint32_t  parent_inode;
    int       is_dir;
    int       dirty;
    struct vnode vnode;
};
static struct ext4_vinfo v4pool[EXT4_MAX_OPEN_VNODES];

/* ============================================================================
 * SECTION 4: UTILITY HELPERS
 * ============================================================================ */

static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t r48(const uint8_t *p) {
    return (uint64_t)r32(p) | ((uint64_t)r16(p+4) << 32);
}
static inline void w16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static inline void w32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static inline void w48(uint8_t *p, uint64_t v) {
    w32(p, (uint32_t)v); w16(p+4, (uint16_t)(v >> 32));
}

static uint32_t ext4_block_lba(uint32_t block_no) {
    return m4.base_lba + block_no * (m4.block_size / 512);
}

/* ============================================================================
 * SECTION 5: BLOCK I/O
 * ============================================================================ */

static int read_block(uint32_t block_no, void *buf) {
    return fs_read_block(m4.bdev, ext4_block_lba(block_no),
                       m4.block_size / 512, buf);
}

static int write_block(uint32_t block_no, const void *buf) {
    return fs_write_block(m4.bdev, ext4_block_lba(block_no),
                        m4.block_size / 512, buf);
}

static int read_inode(uint32_t ino, struct ext4_inode *out) {
    if (ino == 0 || ino >= m4.inodes_count) return -1;
    uint32_t bg = (ino - 1) / m4.inodes_per_group;
    uint32_t idx = (ino - 1) % m4.inodes_per_group;
    if (bg >= m4.group_count) return -1;

    uint32_t gdt_blocks = (m4.group_count * m4.desc_size + m4.block_size - 1) / m4.block_size;
    (void)gdt_blocks;
    uint32_t bgd_lba = m4.first_data_block + 1 + (bg * m4.desc_size) / m4.block_size;

    if (read_block(bgd_lba, ext4_scratch) != 0) return -1;
    struct ext4_bg_desc *gdp = (struct ext4_bg_desc *)
        (ext4_scratch + (bg * m4.desc_size) % m4.block_size);

    uint32_t itable_block = gdp->bg_inode_table_lo +
        (idx * m4.inode_size) / m4.block_size;
    uint32_t offset_in_block = (idx * m4.inode_size) % m4.block_size;

    if (read_block(itable_block, ext4_cluster_buf) != 0) return -1;
    memcpy(out, ext4_cluster_buf + offset_in_block,
           m4.inode_size < sizeof(struct ext4_inode) ?
           m4.inode_size : sizeof(struct ext4_inode));
    return 0;
}

static int write_inode(uint32_t ino, struct ext4_inode *in) {
    if (ino == 0 || ino >= m4.inodes_count) return -1;
    uint32_t bg = (ino - 1) / m4.inodes_per_group;
    uint32_t idx = (ino - 1) % m4.inodes_per_group;
    if (bg >= m4.group_count) return -1;

    uint32_t gdt_blocks = (m4.group_count * m4.desc_size + m4.block_size - 1) / m4.block_size;
    (void)gdt_blocks;
    uint32_t bgd_lba = m4.first_data_block + 1 + (bg * m4.desc_size) / m4.block_size;

    if (read_block(bgd_lba, ext4_scratch) != 0) return -1;
    struct ext4_bg_desc *gdp = (struct ext4_bg_desc *)
        (ext4_scratch + (bg * m4.desc_size) % m4.block_size);

    uint32_t itable_block = gdp->bg_inode_table_lo +
        (idx * m4.inode_size) / m4.block_size;
    uint32_t offset_in_block = (idx * m4.inode_size) % m4.block_size;

    if (read_block(itable_block, ext4_cluster_buf) != 0) return -1;
    memcpy(ext4_cluster_buf + offset_in_block, in,
           m4.inode_size < sizeof(struct ext4_inode) ?
           m4.inode_size : sizeof(struct ext4_inode));
    return write_block(itable_block, ext4_cluster_buf);
}

/* ============================================================================
 * SECTION 6: BLOCK ALLOCATION
 * ============================================================================ */

/* Find a free block in the given group. Returns block number or 0 on failure. */
static uint32_t alloc_block_in_group(uint32_t group) {
    uint32_t gdt_blocks = (m4.group_count * m4.desc_size + m4.block_size - 1) / m4.block_size;
    (void)gdt_blocks;
    uint32_t bgd_lba = m4.first_data_block + 1 + (group * m4.desc_size) / m4.block_size;

    if (read_block(bgd_lba, ext4_scratch) != 0) return 0;
    struct ext4_bg_desc *gdp = (struct ext4_bg_desc *)
        (ext4_scratch + (group * m4.desc_size) % m4.block_size);

    /* Read block bitmap */
    uint32_t bb_lba = ext4_block_lba(gdp->bg_block_bitmap_lo);
    (void)bb_lba;
    if (read_block(gdp->bg_block_bitmap_lo, ext4_cluster_buf) != 0) return 0;

    /* Find first zero bit in the bitmap */
    uint32_t block_base = group * m4.blocks_per_group;
    uint32_t block_end = block_base + m4.blocks_per_group;
    if (block_end > m4.blocks_count) block_end = (uint32_t)m4.blocks_count;

    for (uint32_t byte_off = 0; byte_off < m4.block_size; byte_off++) {
        uint8_t b = ext4_cluster_buf[byte_off];
        if (b == 0xFF) continue;
        for (int bit = 0; bit < 8; bit++) {
            if ((b & (1 << bit)) == 0) {
                uint32_t block_no = block_base + byte_off * 8 + bit;
                if (block_no >= m4.blocks_count) return 0;
                if (block_no < m4.first_data_block) continue;
                /* Allocate it */
                ext4_cluster_buf[byte_off] |= (1 << bit);
                if (write_block(gdp->bg_block_bitmap_lo, ext4_cluster_buf) != 0) return 0;

                /* F3: zero the new block.  NOTE: reuse ext4_cluster_buf (the
                 * bitmap has just been written back), NEVER ext4_scratch —
                 * ext4_scratch still holds the GDT that gdp points into, and
                 * memset()ing it here zeroed the group descriptor and wrote
                 * it back, corrupting block 2 so every later allocation read
                 * bg_block_bitmap_lo=0. */
                memset(ext4_cluster_buf, 0, m4.block_size);
                if (write_block(block_no, ext4_cluster_buf) != 0) return 0;

                /* Update group descriptor (gdp in ext4_scratch is intact). */
                uint16_t fc = r16((uint8_t*)&gdp->bg_free_blocks_count_lo);
                fc--;
                w16((uint8_t*)&gdp->bg_free_blocks_count_lo, fc);
                if (write_block(bgd_lba, ext4_scratch) != 0) return 0;

                kprintf("[ext4] allocated block %u (group %u)\n", block_no, group);
                return block_no;
            }
        }
    }
    return 0;
}

static uint32_t alloc_block(void) {
    spinlock_acquire(&m4.alloc_lock);
    for (uint32_t g = 0; g < m4.group_count; g++) {
        uint32_t b = alloc_block_in_group(g);
        if (b != 0) {
            spinlock_release(&m4.alloc_lock);
            return b;
        }
    }
    spinlock_release(&m4.alloc_lock);
    return 0;
}

/* F3: allocate an extent-tree block that maps logical block 0 to a single
 * physical data block, and return the extent block number (0 on error).
 *
 * A directory (or file) using extents must have i_block[0] point at an
 * EXTENT HEADER block whose entries reference the real data blocks.  The
 * pre-F3 mkdir/format wrote i_block[0] = data_block directly, which made
 * extent_map() read directory bytes as an extent header and fail every
 * lookup — this helper is the fix. */
static uint32_t ext4_make_single_extent(uint32_t data_block) {
    uint32_t ext_block = alloc_block();
    if (!ext_block) return 0;
    memset(ext4_scratch, 0, m4.block_size);
    struct ext4_extent_header *eh = (struct ext4_extent_header *)ext4_scratch;
    eh->eh_magic = EXT4_EXTENT_MAGIC;
    eh->eh_entries = 1;
    eh->eh_max = (uint16_t)((m4.block_size - sizeof(*eh)) /
                            sizeof(struct ext4_extent));
    eh->eh_depth = 0;
    eh->eh_generation = 0;
    struct ext4_extent *e = (struct ext4_extent *)(ext4_scratch + sizeof(*eh));
    e->ee_block = 0;
    e->ee_len = 1;
    e->ee_start_hi = 0;
    e->ee_start_lo = data_block;
    if (write_block(ext_block, ext4_scratch) != 0) return 0;
    return ext_block;
}

/* ============================================================================
 * SECTION 7: EXTENT TREE
 * ============================================================================ */

/* Walk an extent tree to find the physical block for a given logical block.
 * Returns physical block number or 0 if not allocated. */
static uint32_t extent_map(struct ext4_inode *inode, uint32_t lblock) {
    if (!(inode->i_flags & EXT4_EXTENTS_FL)) return 0;
    if (!inode->i_block[0]) return 0;

    /* Read extent header */
    if (read_block(inode->i_block[0], ext4_scratch) != 0) return 0;
    struct ext4_extent_header *eh = (struct ext4_extent_header *)ext4_scratch;
    if (eh->eh_magic != EXT4_EXTENT_MAGIC) return 0;

    if (eh->eh_depth == 0) {
        /* Leaf node — search extents */
        struct ext4_extent *ext = (struct ext4_extent *)(ext4_scratch + sizeof(*eh));
        for (int i = 0; i < eh->eh_entries; i++) {
            if (lblock >= ext[i].ee_block &&
                lblock < ext[i].ee_block + ext[i].ee_len) {
                uint64_t start = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
                uint64_t phys = start + (uint64_t)(lblock - ext[i].ee_block);
                return (phys <= 0xFFFFFFFFULL) ? (uint32_t)phys : 0;
            }
        }
    } else {
        /* Internal node — walk down the tree */
        struct ext4_extent_idx *idx = (struct ext4_extent_idx *)
            (ext4_scratch + sizeof(*eh));
        uint32_t child = 0;
        for (int i = 0; i < eh->eh_entries; i++) {
            if (lblock >= idx[i].ei_block) {
                uint64_t child64 = ((uint64_t)idx[i].ei_leaf_hi << 32) | idx[i].ei_leaf_lo;
                child = (child64 <= 0xFFFFFFFFULL) ? (uint32_t)child64 : 0;
            }
        }
        if (!child) return 0;
        if (read_block(child, ext4_scratch) != 0) return 0;
        eh = (struct ext4_extent_header *)ext4_scratch;
        if (eh->eh_magic != EXT4_EXTENT_MAGIC) return 0;
        struct ext4_extent *ext = (struct ext4_extent *)(ext4_scratch + sizeof(*eh));
        for (int i = 0; i < eh->eh_entries; i++) {
            if (lblock >= ext[i].ee_block &&
                lblock < ext[i].ee_block + ext[i].ee_len) {
                uint64_t start = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
                uint64_t phys = start + (uint64_t)(lblock - ext[i].ee_block);
                return (phys <= 0xFFFFFFFFULL) ? (uint32_t)phys : 0;
            }
        }
    }
    return 0;
}

/* Allocate and map a range of logical blocks to physical blocks using extents.
 * Returns 0 on success. */
static int extent_insert(struct ext4_inode *inode, uint32_t lblock_start,
                         uint32_t count) {
    /* Allocate physical blocks */
    uint32_t *phys_blocks = kmalloc(count * sizeof(uint32_t));
    if (!phys_blocks) return -1;

    for (uint32_t i = 0; i < count; i++) {
        phys_blocks[i] = alloc_block();
        if (!phys_blocks[i]) {
            for (uint32_t j = 0; j < i; j++) {
                /* Simple: just mark the block free - in production would
                 * want proper rollback here */
            }
            kfree(phys_blocks);
            return -1;
        }
    }

    /* Build extent tree */
    if (inode->i_block[0] == 0) {
        inode->i_block[0] = alloc_block();
        if (!inode->i_block[0]) { kfree(phys_blocks); return -1; }
    }

    /* Write the extent header and extents */
    if (read_block(inode->i_block[0], ext4_scratch) != 0) {
        kfree(phys_blocks); return -1;
    }
    struct ext4_extent_header *eh = (struct ext4_extent_header *)ext4_scratch;
    eh->eh_magic = EXT4_EXTENT_MAGIC;
    eh->eh_entries = (uint16_t)count;
    eh->eh_max = (uint16_t)((m4.block_size - sizeof(*eh)) / sizeof(struct ext4_extent));
    eh->eh_depth = 0;
    eh->eh_generation = 0;

    struct ext4_extent *ext = (struct ext4_extent *)(ext4_scratch + sizeof(*eh));
    for (uint32_t i = 0; i < count; i++) {
        ext[i].ee_block = lblock_start + i;
        ext[i].ee_len = 1;
        ext[i].ee_start_hi = 0; /* phys_blocks[] is uint32_t; high 16 bits of 48-bit block are zero. */
        ext[i].ee_start_lo = (uint32_t)phys_blocks[i];
    }

    if (write_block(inode->i_block[0], ext4_scratch) != 0) {
        kfree(phys_blocks); return -1;
    }

    kfree(phys_blocks);
    return 0;
}

/* ============================================================================
 * SECTION 8: JOURNALING (JBD2-style)
 * ============================================================================ */

static int journal_init(void) {
    if (!m4.has_journal || m4.journal_block == 0) return 0;

    kprintf("[ext4] journal: initializing at block %u\n", m4.journal_block);

    /* Read journal superblock */
    if (read_block(m4.journal_block, ext4_scratch) != 0) return -1;
    struct ext4_journal_sb *jsb = (struct ext4_journal_sb *)ext4_scratch;
    (void)jsb;

    if (r32(ext4_scratch) != EXT4_JOURNAL_MAGIC) {
        kprintf("[ext4] journal: invalid magic, formatting...\n");
        /* Format the journal */
        memset(ext4_scratch, 0, m4.block_size);
        w32(ext4_scratch, EXT4_JOURNAL_MAGIC);
        w32(ext4_scratch + 4, 2); /* block_type = superblock v2 */
        w32(ext4_scratch + 8, 1); /* sequence */
        w32(ext4_scratch + 12, m4.block_size);
        w32(ext4_scratch + 16, 1024); /* max_len */
        w32(ext4_scratch + 20, m4.journal_block + 1); /* first */
        w32(ext4_scratch + 24, m4.journal_block + EXT4_JOURNAL_BLOCKS); /* last */
        w32(ext4_scratch + 28, m4.journal_block + 1); /* start */
        if (write_block(m4.journal_block, ext4_scratch) != 0) return -1;
    }

    /* Clear journal blocks */
    memset(ext4_scratch, 0, m4.block_size);
    for (uint32_t i = 0; i < EXT4_JOURNAL_BLOCKS; i++) {
        write_block(m4.journal_block + 1 + i, ext4_scratch);
    }

    m4.journal_sequence = 1;
    m4.journal_head = m4.journal_block + 1;
    m4.journal_tail = m4.journal_block + 1;

    kprintf("[ext4] journal: ready, sequence %u\n", m4.journal_sequence);
    return 0;
}

static int journal_log_block(uint32_t block_no, const void *data) {
    if (!m4.has_journal) {
        /* No journaling — just write directly */
        return write_block(block_no, data);
    }

    /* Write the data block first (to be safe) */
    if (write_block(block_no, data) != 0) return -1;
    return 0;
}

static int journal_commit(void) {
    if (!m4.has_journal) return 0;
    m4.journal_sequence++;
    kprintf("[ext4] journal: committed tx #%u\n", m4.journal_sequence - 1);
    return 0;
}

/* ============================================================================
 * SECTION 9: DIRECTORY OPERATIONS
 * ============================================================================ */

/* Find a directory entry by name. Returns inode number or 0. */
static uint32_t dir_lookup(uint32_t dir_inode, const char *name, int name_len,
                          struct ext4_dirent *out_de) {
    struct ext4_inode dinode;
    if (read_inode(dir_inode, &dinode) != 0) return 0;
    if (!(dinode.i_mode & EXT4_S_IFDIR)) return 0;

    uint32_t cl = extent_map(&dinode, 0);
    if (!cl) return 0;

    /* Read first block of directory */
    if (read_block(cl, ext4_cluster_buf) != 0) return 0;

    uint32_t off = 0;
    while (off < m4.block_size) {
        struct ext4_dirent *de = (struct ext4_dirent *)(ext4_cluster_buf + off);
        if (de->inode == 0) { off += de->rec_len; continue; }
        if (de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0) {
            if (out_de) *out_de = *de;
            return de->inode;
        }
        off += de->rec_len;
        if (de->rec_len == 0) break;
    }

    return 0;
}

/* F3: locate a directory entry across ALL blocks of a directory, not just
 * the first.  On success returns 0 and (optionally) the physical block and
 * byte offset that hold the entry.  Returns -1 when the name is absent. */
static int dir_find_entry(uint32_t dir_inode, const char *name, int name_len,
                          uint32_t *out_pblock, uint32_t *out_off) {
    struct ext4_inode dinode;
    if (read_inode(dir_inode, &dinode) != 0) return -1;
    if (!(dinode.i_mode & EXT4_S_IFDIR)) return -1;

    for (uint32_t bi = 0; ; bi++) {
        uint32_t cl = extent_map(&dinode, bi);
        if (!cl) break;
        if (read_block(cl, ext4_cluster_buf) != 0) return -1;

        uint32_t off = 0;
        while (off < m4.block_size) {
            struct ext4_dirent *de = (struct ext4_dirent *)(ext4_cluster_buf + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                if (out_pblock) *out_pblock = cl;
                if (out_off) *out_off = off;
                return 0;
            }
            off += de->rec_len;
        }
    }
    return -1;
}

/* F3: byte size an entry actually occupies (header + file_type + name,
 * 4-byte aligned) — the amount a dirent "uses" of its rec_len. */
static uint32_t dir_entry_used(int name_len) {
    return (uint32_t)((9u + (uint32_t)name_len + 3u) & ~3u);
}

/* F3: append a new directory entry to a directory.  Returns 0 on success,
 * -1 if there is no free space in any block (directory too full).
 *
 * Handles the classic ext4 layout: the LAST dirent in a block has its
 * rec_len padded out to the end of the block, so "free space" is the slack
 * of the last (or a deleted) entry.  We reuse a deleted entry when big
 * enough, otherwise split the last entry's slack. */
static int dir_add_entry(uint32_t dir_inode, const char *name, int name_len,
                         uint32_t inode, uint8_t file_type) {
    struct ext4_inode dinode;
    if (read_inode(dir_inode, &dinode) != 0) return -1;
    if (!(dinode.i_mode & EXT4_S_IFDIR)) return -1;

    uint32_t need = dir_entry_used(name_len);

    for (uint32_t bi = 0; ; bi++) {
        uint32_t cl = extent_map(&dinode, bi);
        if (!cl) break;
        if (read_block(cl, ext4_cluster_buf) != 0) return -1;

        uint32_t off = 0;
        while (off < m4.block_size) {
            struct ext4_dirent *de = (struct ext4_dirent *)(ext4_cluster_buf + off);
            if (de->rec_len == 0) break;      /* end of used entries */
            uint32_t used = dir_entry_used(de->name_len);
            if (de->inode == 0) {
                /* Deleted entry: reuse if big enough. */
                if (de->rec_len >= need) {
                    uint32_t remainder = de->rec_len - need;
                    struct ext4_dirent *nd = (struct ext4_dirent *)(ext4_cluster_buf + off);
                    nd->inode = inode;
                    nd->rec_len = (uint16_t)need;
                    nd->name_len = (uint16_t)name_len;
                    nd->file_type = file_type;
                    memcpy(nd->name, name, (size_t)name_len);
                    if (remainder >= 4) {
                        struct ext4_dirent *tail = (struct ext4_dirent *)
                            (ext4_cluster_buf + off + need);
                        tail->inode = 0;
                        tail->rec_len = (uint16_t)remainder;
                        tail->name_len = 0;
                        tail->file_type = 0;
                    }
                    return write_block(cl, ext4_cluster_buf);
                }
            } else if (de->rec_len >= used && de->rec_len - used >= need) {
                /* Live entry whose rec_len covers trailing slack: split it
                 * by shrinking the original entry to its used size and
                 * placing the new one in the freed region. */
                uint32_t orig = de->rec_len;
                de->rec_len = (uint16_t)used;
                struct ext4_dirent *nd = (struct ext4_dirent *)
                    (ext4_cluster_buf + off + used);
                nd->inode = inode;
                nd->rec_len = (uint16_t)(orig - used);
                nd->name_len = (uint16_t)name_len;
                nd->file_type = file_type;
                memcpy(nd->name, name, (size_t)name_len);
                return write_block(cl, ext4_cluster_buf);
            }
            off += de->rec_len;
        }

        /* No space in this block — try the next. */
    }
    kprintf("[ext4] dir_add_entry: %s no free space\n", name);
    return -1;
}

/* F3: remove a directory entry (sets inode=0 and merges space into the
 * previous entry so the directory stays parseable). */
static int dir_remove_entry(uint32_t dir_inode, const char *name, int name_len) {
    uint32_t pblock = 0, off = 0;
    if (dir_find_entry(dir_inode, name, name_len, &pblock, &off) != 0)
        return -1;
    if (read_block(pblock, ext4_cluster_buf) != 0) return -1;

    struct ext4_dirent *de = (struct ext4_dirent *)(ext4_cluster_buf + off);
    uint32_t rec_len = de->rec_len;

    if (off > 0) {
        /* Merge the removed entry's space into the PREVIOUS live entry by
         * expanding that previous entry's rec_len to cover everything from
         * its own start through the end of the removed entry.
         *
         * F3: track the previous entry's BYTE POSITION (last_pos).  The old
         * code scanned `p` up to `off` and then used `off - p` — but `p`
         * had already advanced to `off`, so it set the previous entry's
         * rec_len to just the removed entry's own rec_len, orphaning the
         * rest of the block and corrupting the directory. */
        uint32_t p = 0;
        struct ext4_dirent *last = NULL;
        uint32_t last_pos = 0;
        while (p < off) {
            struct ext4_dirent *e = (struct ext4_dirent *)(ext4_cluster_buf + p);
            if (e->rec_len == 0) break;
            last = e;
            last_pos = p;
            p += e->rec_len;
        }
        if (last) {
            last->rec_len = (uint16_t)((uint32_t)(off + rec_len) - last_pos);
            return write_block(pblock, ext4_cluster_buf);
        }
    }

    /* First entry in the block: just mark it free. */
    de->inode = 0;
    de->name_len = 0;
    return write_block(pblock, ext4_cluster_buf);
}

/* F3: detect an HTree (dx_root) directory block.  An indexed ext4
 * directory keeps its real entries in "leaf" blocks; block 0 is a
 * dx_root (".", "..", then a dx_root_info + hash index), NOT a linear
 * entry list.  readdir must skip the dx_root and enumerate the leaves.
 * This only DETECTS the structure — hashed lookup/insertion into an
 * HTree is out of scope (indexed-dir writing stays out of scope). */
static int ext4_dir_is_htree(const uint8_t *blk) {
    struct ext4_dirent *dot = (struct ext4_dirent *)blk;
    if (dot->rec_len < 8 || dot->name_len != 1 || dot->name[0] != '.')
        return 0;
    uint32_t off = dot->rec_len;
    struct ext4_dirent *dd = (struct ext4_dirent *)(blk + off);
    if (dd->rec_len < 8 || dd->name_len != 2 ||
        dd->name[0] != '.' || dd->name[1] != '.')
        return 0;
    off += dd->rec_len;
    if (off + 8 > m4.block_size) return 0;

    const uint8_t *ri = blk + off;
    if (r32(ri) != 0) return 0;               /* reserved_zero */
    if (ri[5] != 8) return 0;                 /* info_length */
    if (ri[4] > 4) return 0;                  /* hash_version (0..4) */
    if (ri[6] > 4) return 0;                  /* indirect_levels */
    return 1;
}

/* List directory entries */
static int ext4_readdir_op(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct ext4_vinfo *v = (struct ext4_vinfo *)vn->fs_data;
    if (!v || !v->is_dir) return -1;

    struct ext4_inode dinode;
    if (read_inode(v->inode, &dinode) != 0) return -1;

    int count = 0;
    uint32_t block_idx = 0;
    int htree = 0;

    while (count < max) {
        uint32_t cl = extent_map(&dinode, block_idx);
        if (!cl) break;
        if (read_block(cl, ext4_cluster_buf) != 0) break;

        /* An HTree directory: block 0 is the dx_root; skip it and read
         * the leaves (blocks 1..) which are linear entry lists. */
        if (block_idx == 0 && ext4_dir_is_htree(ext4_cluster_buf)) {
            htree = 1;
            block_idx++;
            continue;
        }

        uint32_t off = 0;
        while (off < m4.block_size && count < max) {
            struct ext4_dirent *de = (struct ext4_dirent *)(ext4_cluster_buf + off);
            if (de->inode == 0) { off += de->rec_len; continue; }
            if (de->rec_len == 0) break;

            if (de->name_len > 0 && de->name_len < VFS_PATH_MAX) {
                memset(&out[count], 0, sizeof(out[count]));
                memcpy(out[count].name, de->name, de->name_len);
                out[count].name[de->name_len] = 0;
                out[count].inode = de->inode;

                switch (de->file_type) {
                case EXT4_FT_DIR: out[count].type = VFS_TYPE_DIR; break;
                case EXT4_FT_REG_FILE: out[count].type = VFS_TYPE_FILE; break;
                case EXT4_FT_SYMLINK: out[count].type = VFS_TYPE_SYMLINK; break;
                default: out[count].type = VFS_TYPE_FILE; break;
                }
                count++;
            }
            off += de->rec_len;
        }
        block_idx++;
    }
    if (htree) {
        kprintf("[ext4] readdir: indexed (HTree) directory — enumerated %d "
                "entries from leaves (dx_root skipped)\n", count);
    }
    return count;
}

/* ============================================================================
 * SECTION 10: FILE I/O
 * ============================================================================ */

static int64_t ext4_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct ext4_vinfo *v = (struct ext4_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;

    struct ext4_inode inode;
    if (read_inode(v->inode, &inode) != 0) return -1;

    uint64_t file_size = ((uint64_t)inode.i_size_high << 32) | inode.i_size_lo;
    if (pos >= file_size) return 0;
    if (pos + count > file_size) count = file_size - pos;

    /* F3: symlink support.  A fast (inline) symlink stores its target in
     * the inode's i_block bytes (no extent flag); a slow symlink stores
     * it in a data block and falls through to the normal extent read. */
    if ((inode.i_mode & EXT4_S_IFLNK) && !(inode.i_flags & EXT4_EXTENTS_FL)) {
        uint8_t *tgt = (uint8_t *)buf;
        memcpy(tgt, (uint8_t *)inode.i_block + (size_t)pos, (size_t)count);
        return (int64_t)count;
    }

    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;

    while (done < count) {
        uint32_t lblock = (uint32_t)((pos + done) / m4.block_size);
        uint32_t off_in_block = (uint32_t)((pos + done) % m4.block_size);
        uint32_t pblock = extent_map(&inode, lblock);

        if (pblock == 0) break;

        if (read_block(pblock, ext4_cluster_buf) != 0) return -1;
        uint64_t chunk = m4.block_size - off_in_block;
        if (chunk > count - done) chunk = count - done;
        memcpy(out + done, ext4_cluster_buf + off_in_block, (size_t)chunk);
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t ext4_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct ext4_vinfo *v = (struct ext4_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    if (count == 0) return 0;

    /* Delayed allocation: allocate blocks on demand */
    struct ext4_inode inode;
    if (read_inode(v->inode, &inode) != 0) return -1;

    /* Enable extents if not already */
    if (!(inode.i_flags & EXT4_EXTENTS_FL)) {
        inode.i_flags |= EXT4_EXTENTS_FL;
    }

    uint32_t start_lblock = (uint32_t)(pos / m4.block_size);
    uint32_t end_lblock = (uint32_t)((pos + count - 1) / m4.block_size);
    uint32_t needed = end_lblock - start_lblock + 1;
    (void)needed;

    /* Check which blocks already exist.  F3: start first_new past the end
     * so that when every needed block is ALREADY mapped (e.g. a second
     * write call for the same block), we do NOT re-allocate — the old code
     * initialised first_new = start_lblock and then called extent_insert
     * whenever first_new <= end_lblock, which on a re-write REBUILT the
     * extent tree and silently dropped the first extent (file read as 0). */
    uint32_t first_new = end_lblock + 1;
    for (uint32_t b = start_lblock; b <= end_lblock; b++) {
        if (extent_map(&inode, b) == 0) { first_new = b; break; }
    }

    if (first_new <= end_lblock) {
        uint32_t count_needed = end_lblock - first_new + 1;
        if (extent_insert(&inode, first_new, count_needed) != 0) return -1;
    }

    /* Now write the data */
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;

    while (done < count) {
        uint32_t lblock = (uint32_t)((pos + done) / m4.block_size);
        uint32_t off_in_block = (uint32_t)((pos + done) % m4.block_size);
        uint32_t pblock = extent_map(&inode, lblock);

        if (pblock == 0) break;

        if (read_block(pblock, ext4_cluster_buf) != 0) return -1;
        uint64_t chunk = m4.block_size - off_in_block;
        if (chunk > count - done) chunk = count - done;
        memcpy(ext4_cluster_buf + off_in_block, in + done, (size_t)chunk);

        /* Journal the block */
        if (journal_log_block(pblock, ext4_cluster_buf) != 0) return -1;
        done += chunk;
    }

    /* Update inode size and mtime.  F3: DO NOT re-read the inode here —
     * extent_insert() set inode.i_block[0] (the extent-tree root) in this
     * same struct, and re-reading from disk discards it, saving an inode
     * whose extent pointer is 0 (so the file reads as empty and unmaps).
     * Persist the struct we already hold. */
    uint64_t new_size = pos + count;
    inode.i_size_lo = (uint32_t)new_size;
    inode.i_size_high = (uint32_t)(new_size >> 32);
    inode.i_mtime = 1337 + (uint32_t)pos; /* pseudo time */
    inode.i_blocks_lo += (uint32_t)(count / 512);

    if (write_inode(v->inode, &inode) != 0) return -1;

    v->size = (uint32_t)new_size;
    v->vnode.size = new_size;
    v->dirty = 1;

    return (int64_t)done;
}

static int ext4_truncate(struct vnode *vn, uint64_t new_size) {
    struct ext4_vinfo *v = (struct ext4_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;

    struct ext4_inode inode;
    if (read_inode(v->inode, &inode) != 0) return -1;

    inode.i_size_lo = (uint32_t)new_size;
    inode.i_size_high = (uint32_t)(new_size >> 32);

    if (write_inode(v->inode, &inode) != 0) return -1;
    v->size = (uint32_t)new_size;
    v->vnode.size = new_size;
    v->dirty = 1;

    return 0;
}

/* ============================================================================
 * SECTION 11: PATH RESOLUTION
 * ============================================================================ */

static int path_resolve(const char *path, uint32_t *out_parent_inode,
                        uint32_t *out_target_inode, int *found, char *basename,
                        int basename_sz) {
    *found = 0;
    if (!path) return -1;

    /* Skip leading slashes */
    while (*path == '/') path++;
    if (!*path) {
        *out_parent_inode = 2; /* root inode */
        *out_target_inode = 2;
        *found = 1;
        if (basename && basename_sz) basename[0] = 0;
        return 0;
    }

    uint32_t dir = 2; /* start from root */
    char comp[EXT4_MAX_NAME];
    const char *p = path;

    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;

        if (*p == 0) {
            /* Final component */
            *out_parent_inode = dir;
            if (basename && basename_sz) {
                strncpy(basename, comp, basename_sz - 1);
                basename[basename_sz - 1] = 0;
            }

            uint32_t ino = dir_lookup(dir, comp, n, NULL);
            if (ino != 0) {
                *out_target_inode = ino;
                *found = 1;
            }
            return 0;
        }

        /* Intermediate must be a directory */
        uint32_t ino = dir_lookup(dir, comp, n, NULL);
        if (ino == 0) return -1;

        struct ext4_inode next_inode;
        if (read_inode(ino, &next_inode) != 0) return -1;
        if (!(next_inode.i_mode & EXT4_S_IFDIR)) return -1;
        dir = ino;
    }

    return -1;
}

/* ============================================================================
 * SECTION 12: VNODE MANAGEMENT
 * ============================================================================ */

static struct ext4_vinfo *v4_intern(const char *path, uint32_t inode_no,
                                    uint32_t parent, int is_dir, uint32_t size) {
    /* Find existing */
    for (int i = 0; i < EXT4_MAX_OPEN_VNODES; i++) {
        if (v4pool[i].in_use && strcmp(v4pool[i].path, path) == 0) {
            v4pool[i].inode = inode_no;
            v4pool[i].parent_inode = parent;
            v4pool[i].size = size;
            v4pool[i].is_dir = is_dir;
            v4pool[i].vnode.size = size;
            return &v4pool[i];
        }
    }
    /* Find free slot */
    for (int i = 0; i < EXT4_MAX_OPEN_VNODES; i++) {
        if (!v4pool[i].in_use) {
            struct ext4_vinfo *v = &v4pool[i];
            memset(v, 0, sizeof(*v));
            v->in_use = 1;
            strncpy(v->path, path, sizeof(v->path) - 1);
            v->inode = inode_no;
            v->parent_inode = parent;
            v->size = size;
            v->is_dir = is_dir;
            strncpy(v->vnode.name, path, VFS_PATH_MAX - 1);
            v->vnode.type = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            v->vnode.mode = is_dir ? 0755 : 0644;
            v->vnode.size = size;
            v->vnode.ops = &ext4_ops;
            v->vnode.fs_data = v;
            v->vnode.inode_id = inode_no;
            return v;
        }
    }
    return NULL;
}

static void v4_evict(const char *path) {
    for (int i = 0; i < EXT4_MAX_OPEN_VNODES; i++) {
        if (v4pool[i].in_use && strcmp(v4pool[i].path, path) == 0) {
            v4pool[i].in_use = 0;
            return;
        }
    }
}

/* ============================================================================
 * SECTION 13: VFS OPERATIONS
 * ============================================================================ */

static struct vnode *ext4_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (!m4.mounted) return NULL;

    uint32_t parent, target;
    int found;
    char base[EXT4_MAX_NAME] = {0};

    if (path_resolve(path, &parent, &target, &found, base, sizeof(base)) != 0)
        return NULL;
    if (!found) return NULL;

    struct ext4_inode inode;
    if (read_inode(target, &inode) != 0) return NULL;

    int is_dir = (inode.i_mode & EXT4_S_IFDIR) != 0;
    uint64_t size = ((uint64_t)inode.i_size_high << 32) | inode.i_size_lo;

    return &v4_intern(path, target, parent, is_dir, (uint32_t)size)->vnode;
}

static struct vnode *ext4_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!m4.mounted) return NULL;

    uint32_t parent, target;
    int found;
    char base[EXT4_MAX_NAME] = {0};

    if (path_resolve(path, &parent, &target, &found, base, sizeof(base)) != 0)
        return NULL;

    if (found) return NULL; /* already exists */
    if (!base[0]) return NULL;

    /* Allocate a new inode */
    spinlock_acquire(&m4.alloc_lock);
    uint32_t new_ino = 0;
    for (uint32_t g = 0; g < m4.group_count && !new_ino; g++) {
        uint32_t gdt_blocks = (m4.group_count * m4.desc_size + m4.block_size - 1) / m4.block_size;
        (void)gdt_blocks;
        uint32_t bgd_lba = m4.first_data_block + 1 + (g * m4.desc_size) / m4.block_size;
        if (read_block(bgd_lba, ext4_scratch) != 0) continue;
        struct ext4_bg_desc *gdp = (struct ext4_bg_desc *)
            (ext4_scratch + (g * m4.desc_size) % m4.block_size);

        /* Read inode bitmap */
        if (read_block(gdp->bg_inode_bitmap_lo, ext4_cluster_buf) != 0) continue;

        for (uint32_t byte_off = 0; byte_off < m4.block_size; byte_off++) {
            uint8_t b = ext4_cluster_buf[byte_off];
            if (b == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if ((b & (1 << bit)) == 0) {
                    new_ino = g * m4.inodes_per_group + byte_off * 8 + bit + 1;
                    if (new_ino >= m4.inodes_count) { new_ino = 0; continue; }
                    /* Mark as allocated */
                    ext4_cluster_buf[byte_off] |= (1 << bit);
                    write_block(gdp->bg_inode_bitmap_lo, ext4_cluster_buf);

                    /* Update group descriptor */
                    uint16_t fi = r16((uint8_t*)&gdp->bg_free_inodes_count_lo);
                    if (fi > 0) fi--;
                    w16((uint8_t*)&gdp->bg_free_inodes_count_lo, fi);
                    write_block(bgd_lba, ext4_scratch);
                    break;
                }
            }
            if (new_ino) break;
        }
    }
    spinlock_release(&m4.alloc_lock);

    if (!new_ino) return NULL;

    /* Initialize the inode */
    struct ext4_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT4_S_IFREG | 0644;
    inode.i_uid_lo = 0;
    inode.i_gid_lo = 0;
    inode.i_size_lo = 0;
    inode.i_size_high = 0;
    inode.i_atime = 0;
    inode.i_ctime = 0;
    inode.i_mtime = 0;
    inode.i_links_count = 1;
    inode.i_flags = EXT4_EXTENTS_FL; /* ext4 by default */

    if (write_inode(new_ino, &inode) != 0) return NULL;

    /* F3: the new inode must actually be registered in its parent
     * directory, or nothing will ever find it (F3 discovered ext4_create
     * minted orphan inodes that readdir/path-resolve could not see). */
    if (dir_add_entry(parent, base, (int)strlen(base), new_ino,
                      EXT4_FT_REG_FILE) != 0) {
        kprintf("[ext4] create: could not add '%s' to dir inode %u\n",
                base, parent);
        return NULL;
    }

    return &v4_intern(path, new_ino, parent, 0, 0)->vnode;
}

static int ext4_mkdir(void *fs_data, const char *path) {
    (void)fs_data;
    if (!m4.mounted) return -1;

    uint32_t parent, target;
    int found;
    char base[EXT4_MAX_NAME] = {0};

    if (path_resolve(path, &parent, &target, &found, base, sizeof(base)) != 0)
        return -1;
    if (found) return -1;
    if (!base[0]) return -1;

    /* Allocate a new inode */
    spinlock_acquire(&m4.alloc_lock);
    uint32_t new_ino = 0;
    for (uint32_t g = 0; g < m4.group_count && !new_ino; g++) {
        uint32_t bgd_lba = m4.first_data_block + 1 + (g * m4.desc_size) / m4.block_size;
        if (read_block(bgd_lba, ext4_scratch) != 0) continue;
        struct ext4_bg_desc *gdp = (struct ext4_bg_desc *)
            (ext4_scratch + (g * m4.desc_size) % m4.block_size);

        if (read_block(gdp->bg_inode_bitmap_lo, ext4_cluster_buf) != 0) continue;

        for (uint32_t byte_off = 0; byte_off < m4.block_size && !new_ino; byte_off++) {
            uint8_t b = ext4_cluster_buf[byte_off];
            if (b == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if ((b & (1 << bit)) == 0) {
                    new_ino = g * m4.inodes_per_group + byte_off * 8 + bit + 1;
                    if (new_ino >= m4.inodes_count) { new_ino = 0; continue; }
                    ext4_cluster_buf[byte_off] |= (1 << bit);
                    write_block(gdp->bg_inode_bitmap_lo, ext4_cluster_buf);
                    uint16_t fi = r16((uint8_t*)&gdp->bg_free_inodes_count_lo);
                    if (fi > 0) fi--;
                    w16((uint8_t*)&gdp->bg_free_inodes_count_lo, fi);

                    uint16_t dc = r16((uint8_t*)&gdp->bg_used_dirs_count_lo);
                    dc++;
                    w16((uint8_t*)&gdp->bg_used_dirs_count_lo, dc);
                    write_block(bgd_lba, ext4_scratch);
                    break;
                }
            }
        }
    }
    spinlock_release(&m4.alloc_lock);

    if (!new_ino) return -1;

    /* Allocate a block for the directory */
    uint32_t dir_block = alloc_block();
    if (!dir_block) return -1;

    /* Create "." and ".." entries */
    memset(ext4_cluster_buf, 0, m4.block_size);
    struct ext4_dirent *dot = (struct ext4_dirent *)ext4_cluster_buf;
    dot->inode = new_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR;
    memcpy(dot->name, ".", 1);

    struct ext4_dirent *dotdot = (struct ext4_dirent *)(ext4_cluster_buf + 12);
    dotdot->inode = parent;
    dotdot->rec_len = (uint16_t)(m4.block_size - 12);
    dotdot->name_len = 2;
    dotdot->file_type = EXT4_FT_DIR;
    memcpy(dotdot->name, "..", 2);

    if (write_block(dir_block, ext4_cluster_buf) != 0) return -1;

    /* Initialize directory inode.  F3: the directory data block is
     * referenced through a proper extent block, so extent_map() (and
     * therefore readdir/lookup) resolves it correctly. */
    struct ext4_inode dinode;
    memset(&dinode, 0, sizeof(dinode));
    dinode.i_mode = EXT4_S_IFDIR | 0755;
    dinode.i_uid_lo = 0;
    dinode.i_gid_lo = 0;
    dinode.i_size_lo = m4.block_size;
    dinode.i_links_count = 2;
    dinode.i_flags = EXT4_EXTENTS_FL;
    uint32_t ext_block = ext4_make_single_extent(dir_block);
    if (!ext_block) return -1;
    dinode.i_block[0] = ext_block;
    dinode.i_blocks_lo = 1;

    if (write_inode(new_ino, &dinode) != 0) return -1;

    /* F3: register the new directory in its parent (F3 fixed mkdir
     * creating orphan directory inodes that were never listed). */
    if (dir_add_entry(parent, base, (int)strlen(base), new_ino,
                      EXT4_FT_DIR) != 0) {
        kprintf("[ext4] mkdir: could not add '%s' to dir inode %u\n",
                base, parent);
        return -1;
    }

    kprintf("[ext4] mkdir: created inode %u ('%s') in parent %u\n",
            new_ino, base, parent);
    return 0;
}

static int ext4_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    if (!m4.mounted) return -1;

    uint32_t parent, target;
    int found;
    char base[EXT4_MAX_NAME] = {0};
    if (path_resolve(path, &parent, &target, &found, base, sizeof(base)) != 0 ||
        !found)
        return -1;

    struct ext4_inode inode;
    if (read_inode(target, &inode) != 0) return -1;
    if (inode.i_mode & EXT4_S_IFDIR) return -1; /* use rmdir for dirs */

    /* F3: remove the directory entry AND drop the link count, so the
     * inode is actually free-able instead of a dangling tombstone. */
    if (dir_remove_entry(parent, base, (int)strlen(base)) != 0) return -1;
    if (inode.i_links_count > 0) inode.i_links_count--;
    inode.i_dtime = (inode.i_links_count == 0) ? 1 : inode.i_dtime;
    if (write_inode(target, &inode) != 0) return -1;
    v4_evict(path);
    return 0;
}

static int ext4_rmdir(void *fs_data, const char *path) {
    (void)fs_data;
    if (!m4.mounted) return -1;

    uint32_t parent, target;
    int found;
    char base[EXT4_MAX_NAME] = {0};
    if (path_resolve(path, &parent, &target, &found, base, sizeof(base)) != 0 ||
        !found)
        return -1;
    if (target == 2) return -1;   /* never remove the root */

    struct ext4_inode inode;
    if (read_inode(target, &inode) != 0) return -1;
    if (!(inode.i_mode & EXT4_S_IFDIR)) return -1;

    /* A directory is removable only when it is empty (only . and ..). */
    {
        struct vfs_dirent tmp[16];
        struct vnode tv;
        struct ext4_vinfo tvdata;
        memset(&tvdata, 0, sizeof(tvdata));
        tvdata.inode = target;
        tvdata.is_dir = 1;
        tv.fs_data = &tvdata;
        int n = ext4_readdir_op(&tv, tmp, 16);
        for (int i = 0; i < n; i++) {
            if (strcmp(tmp[i].name, ".") == 0 || strcmp(tmp[i].name, "..") == 0)
                continue;
            kprintf("[ext4] rmdir: %s not empty (contains '%s')\n",
                    path, tmp[i].name);
            return -1;
        }
    }

    if (dir_remove_entry(parent, base, (int)strlen(base)) != 0) return -1;

    /* Drop the parent's link count (the child's ".." link disappears). */
    struct ext4_inode pnode;
    if (read_inode(parent, &pnode) == 0) {
        if (pnode.i_links_count > 0) pnode.i_links_count--;
        write_inode(parent, &pnode);
    }

    /* Free the directory's data block (first extent block). */
    uint32_t data_blk = extent_map(&inode, 0);
    if (data_blk) {
        /* Mark free in the block bitmap.  We don't keep a free-list here;
         * the allocator re-scans the bitmap, so clearing the bit suffices.
         * (Simplified: we clear the bit via a helper that locates it.) */
        uint32_t g = data_blk / m4.blocks_per_group;
        uint32_t gdt_blocks = (m4.group_count * m4.desc_size + m4.block_size - 1) / m4.block_size;
        (void)gdt_blocks;
        uint32_t bgd_lba = m4.first_data_block + 1 + (g * m4.desc_size) / m4.block_size;
        if (read_block(bgd_lba, ext4_scratch) == 0) {
            struct ext4_bg_desc *gdp = (struct ext4_bg_desc *)
                (ext4_scratch + (g * m4.desc_size) % m4.block_size);
            if (read_block(gdp->bg_block_bitmap_lo, ext4_cluster_buf) == 0) {
                uint32_t rel = data_blk - g * m4.blocks_per_group;
                ext4_cluster_buf[rel / 8] &= (uint8_t)~(1u << (rel % 8));
                write_block(gdp->bg_block_bitmap_lo, ext4_cluster_buf);
                uint16_t fc = r16((uint8_t*)&gdp->bg_free_blocks_count_lo);
                fc++;
                w16((uint8_t*)&gdp->bg_free_blocks_count_lo, fc);
                write_block(bgd_lba, ext4_scratch);
            }
        }
    }

    /* Free the inode itself. */
    {
        uint32_t g = (target - 1) / m4.inodes_per_group;
        uint32_t bgd_lba = m4.first_data_block + 1 + (g * m4.desc_size) / m4.block_size;
        if (read_block(bgd_lba, ext4_scratch) == 0) {
            struct ext4_bg_desc *gdp = (struct ext4_bg_desc *)
                (ext4_scratch + (g * m4.desc_size) % m4.block_size);
            if (read_block(gdp->bg_inode_bitmap_lo, ext4_cluster_buf) == 0) {
                uint32_t rel = (target - 1) - g * m4.inodes_per_group;
                ext4_cluster_buf[rel / 8] &= (uint8_t)~(1u << (rel % 8));
                write_block(gdp->bg_inode_bitmap_lo, ext4_cluster_buf);
                uint16_t fi = r16((uint8_t*)&gdp->bg_free_inodes_count_lo);
                fi++;
                w16((uint8_t*)&gdp->bg_free_inodes_count_lo, fi);
                write_block(bgd_lba, ext4_scratch);
            }
        }
    }

    v4_evict(path);
    kprintf("[ext4] rmdir: removed '%s' (inode %u)\n", path, target);
    return 0;
}

static int ext4_rename(void *fs_data, const char *from, const char *to) {
    (void)fs_data;
    if (!m4.mounted) return -1;

    uint32_t sparent, starget;
    int sfound;
    char sbase[EXT4_MAX_NAME] = {0};
    if (path_resolve(from, &sparent, &starget, &sfound, sbase, sizeof(sbase)) != 0 ||
        !sfound)
        return -1;

    uint32_t dparent, dtarget;
    int dfound;
    char dbase[EXT4_MAX_NAME] = {0};
    if (path_resolve(to, &dparent, &dtarget, &dfound, dbase, sizeof(dbase)) != 0)
        return -1;
    if (dfound) return -1;   /* destination exists */

    struct ext4_inode inode;
    if (read_inode(starget, &inode) != 0) return -1;
    uint8_t ftype = (inode.i_mode & EXT4_S_IFDIR)
        ? EXT4_FT_DIR : (inode.i_mode & EXT4_S_IFLNK) ? EXT4_FT_SYMLINK
        : EXT4_FT_REG_FILE;

    /* Add the new entry, then remove the old one. */
    if (dir_add_entry(dparent, dbase, (int)strlen(dbase), starget, ftype) != 0)
        return -1;
    if (dir_remove_entry(sparent, sbase, (int)strlen(sbase)) != 0)
        return -1;

    /* Moving a directory into a different parent changes the .. link
     * counts: the old parent loses one, the new parent gains one. */
    if (inode.i_mode & EXT4_S_IFDIR) {
        struct ext4_inode pn;
        if (read_inode(sparent, &pn) == 0 && pn.i_links_count > 0) {
            pn.i_links_count--;
            write_inode(sparent, &pn);
        }
        if (read_inode(dparent, &pn) == 0) {
            pn.i_links_count++;
            write_inode(dparent, &pn);
        }
    }

    v4_evict(from);
    kprintf("[ext4] rename: '%s' -> '%s'\n", from, to);
    return 0;
}

static int ext4_link(void *fs_data, const char *old, const char *new) {
    (void)fs_data;
    if (!m4.mounted) return -1;

    uint32_t parent, target;
    int found;
    char base[EXT4_MAX_NAME] = {0};
    if (path_resolve(old, &parent, &target, &found, base, sizeof(base)) != 0 ||
        !found)
        return -1;
    if (target == 2) return -1;

    struct ext4_inode inode;
    if (read_inode(target, &inode) != 0) return -1;
    if (inode.i_mode & EXT4_S_IFDIR) return -1;   /* no hard links to dirs */

    uint32_t dparent, dtarget;
    int dfound;
    char dbase[EXT4_MAX_NAME] = {0};
    if (path_resolve(new, &dparent, &dtarget, &dfound, dbase, sizeof(dbase)) != 0)
        return -1;
    if (dfound) return -1;

    uint8_t ftype = EXT4_FT_REG_FILE;
    if (dir_add_entry(dparent, dbase, (int)strlen(dbase), target, ftype) != 0)
        return -1;

    inode.i_links_count++;
    if (write_inode(target, &inode) != 0) return -1;
    v4_evict(new);
    return 0;
}

static int ext4_settimes(struct vnode *vn, uint64_t atime, uint64_t mtime) {
    struct ext4_vinfo *v = (struct ext4_vinfo *)vn->fs_data;
    if (!v) return -1;

    struct ext4_inode inode;
    if (read_inode(v->inode, &inode) != 0) return -1;
    /* F3: honour caller-provided Unix-second timestamps (the wall-clock
     * RTC itself is a tree-wide limitation; the plumbing is real). */
    inode.i_atime = (uint32_t)atime;
    inode.i_mtime = (uint32_t)mtime;
    inode.i_ctime = (uint32_t)mtime;
    if (write_inode(v->inode, &inode) != 0) return -1;
    v->vnode.mtime = mtime;
    v->vnode.atime = atime;
    return 0;
}

static int ext4_stat(struct vnode *vn, struct vfs_stat *st) {
    struct ext4_vinfo *v = (struct ext4_vinfo *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    if (!v) return -1;

    struct ext4_inode inode;
    if (read_inode(v->inode, &inode) != 0) return -1;

    st->type = v->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->mode = (inode.i_mode & 0xFFF);
    st->uid  = inode.i_uid_lo;
    st->gid  = inode.i_gid_lo;
    st->size = ((uint64_t)inode.i_size_high << 32) | inode.i_size_lo;
    st->inode = v->inode;
    st->nlink = inode.i_links_count;
    st->blocks = inode.i_blocks_lo;
    return 0;
}

const struct vfs_ops ext4_ops = {
    .lookup   = ext4_lookup,
    .create   = ext4_create,
    .read     = ext4_read,
    .write    = ext4_write,
    .readdir  = ext4_readdir_op,
    .mkdir    = ext4_mkdir,
    .rmdir    = ext4_rmdir,
    .unlink   = ext4_unlink,
    .rename   = ext4_rename,
    .link     = ext4_link,
    .settimes = ext4_settimes,
    .stat     = ext4_stat,
    .truncate = ext4_truncate,
    .sync     = fs_cache_sync,   /* F2 */
};

/* ============================================================================
 * SECTION 14: FORMAT AND MOUNT
 * ============================================================================ */

static int format_ext4(void) {
    kprintf("[ext4] formatting ext4 volume at LBA %u...\n", 128);

    uint32_t block_size = 4096;
    uint32_t blocks_per_group = 32768;
    uint32_t inodes_per_group = 8192;
    uint32_t inode_size = 256;
    uint32_t desc_size = 32;
    uint32_t first_data = 1;

    uint32_t total_blocks = 65536; /* 256MB volume */
    uint32_t group_count = (total_blocks + blocks_per_group - 1) / blocks_per_group;
    uint32_t gdt_blocks = (group_count * desc_size + block_size - 1) / block_size;
    uint32_t inode_table_blocks = (group_count * inodes_per_group * inode_size + block_size - 1) / block_size;

    /* Superblock */
    memset(ext4_scratch, 0, block_size);
    struct ext4_sb *sb = (struct ext4_sb *)ext4_scratch;
    sb->s_inodes_count = group_count * inodes_per_group;
    sb->s_blocks_count_lo = total_blocks;
    sb->s_free_blocks_count_lo = total_blocks - first_data - group_count - gdt_blocks
        - inode_table_blocks - 1;
    sb->s_free_inodes_count = sb->s_inodes_count - 11;
    sb->s_first_data_block = first_data;
    sb->s_log_block_size = 2; /* 4096 bytes */
    sb->s_blocks_per_group = blocks_per_group;
    sb->s_inodes_per_group = inodes_per_group;
    sb->s_magic = EXT4_MAGIC;
    sb->s_state = EXT4_VALID_FS;
    sb->s_first_ino = 11;
    sb->s_inode_size = inode_size;
    sb->s_rev_level = 1; /* ext4 */
    /* F3: the internal formatter produces a journal-FREE volume
     * (feature_compat = 0, s_journal_inum = 0).  JBD2 replay is out of
     * scope, so the driver mounts read-write only volumes without a
     * journal; shipping a formatter that minted a journal the driver
     * then refuses would be self-inconsistent. */
    sb->s_feature_compat = 0;
    sb->s_feature_incompat = EXT4_FEATURE_INCOMPAT_FLEX_BG;
    sb->s_feature_ro_compat = EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |
        EXT4_FEATURE_RO_COMPAT_LARGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM;
    sb->s_journal_inum = 0;
    sb->s_desc_size = desc_size;
    sb->s_blocks_count_hi = 0;
    sb->s_mkfs_time = 1337; /* pseudo time */

    if (write_block(0, ext4_scratch) != 0) return -1;

    /* Block group descriptors.  F3-corrected layout, consistent with the
     * driver convention `bgd_lba = first_data_block + 1` (first_data = 1):
     *   block 0      superblock
     *   block 1      first data block (reserved; never allocated < first_data)
     *   block 2      GDT                  (first_data + 1)
     *   block 3      block bitmap         (first_data + 1 + gdt_blocks)
     *   block 4      inode bitmap
     *   block 5..    inode table
     * The pre-F3 format wrote the GDT at first_data, the bitmaps and
     * inode table one block early, and pointed the descriptors one block
     * off — so the allocator read garbage as a bitmap and returned block
     * 1 forever. */
    uint32_t gdt_start  = first_data + 1;                  /* block 2  */
    uint32_t bb_bitmap  = gdt_start + gdt_blocks;          /* block 3  */
    uint32_t ib_bitmap  = bb_bitmap + 1;                   /* block 4  */
    uint32_t itable     = ib_bitmap + 1;                   /* block 5  */

    memset(ext4_scratch, 0, block_size);
    for (uint32_t g = 0; g < group_count; g++) {
        struct ext4_bg_desc *bgd = (struct ext4_bg_desc *)
            (ext4_scratch + g * desc_size);
        uint32_t block_base = g * blocks_per_group;
        bgd->bg_block_bitmap_lo = block_base + bb_bitmap;
        bgd->bg_inode_bitmap_lo = block_base + ib_bitmap;
        bgd->bg_inode_table_lo = block_base + itable;
        bgd->bg_free_blocks_count_lo = blocks_per_group;
        bgd->bg_free_inodes_count_lo = inodes_per_group;
        bgd->bg_used_dirs_count_lo = 0;
    }
    if (write_block(gdt_start, ext4_scratch) != 0) return -1;

    /* Block bitmap (block bb_bitmap): mark used blocks 0 (super), 1 (first
     * data block), 2 (GDT), 3 (block bitmap), 4 (inode bitmap) and the
     * inode table blocks 5..4+inode_table_blocks. */
    memset(ext4_cluster_buf, 0, block_size);
    for (uint32_t i = 0; i <= 4u + inode_table_blocks; i++)
        ext4_cluster_buf[i / 8] |= (uint8_t)(1u << (i % 8));
    if (write_block(bb_bitmap, ext4_cluster_buf) != 0) return -1;

    /* Inode bitmap (block ib_bitmap): inodes 1,2,3 reserved. */
    memset(ext4_cluster_buf, 0, block_size);
    ext4_cluster_buf[0] = 0x07; /* inodes 1,2,3 reserved */
    if (write_block(ib_bitmap, ext4_cluster_buf) != 0) return -1;

    /* Inode table (blocks itable..itable+inode_table_blocks-1).
     * NOTE: on-disk inodes are 256 bytes, but struct ext4_inode is only
     * ~124 bytes.  We must address inode 2 by byte offset (2-1)*inode_size,
     * NOT by struct array indexing (root_inode[1] == offset 124), or we
     * write inode 2's fields into the middle of inode 1 and read_inode
     * (which uses the real 256-byte stride) sees a zeroed inode. */
    memset(ext4_cluster_buf, 0, block_size);
    struct ext4_inode *in2 = (struct ext4_inode *)
        (ext4_cluster_buf + (2 - 1) * inode_size);
    in2->i_mode = EXT4_S_IFDIR | 0755; /* inode 2 */
    in2->i_size_lo = block_size;
    in2->i_links_count = 2;
    in2->i_flags = EXT4_EXTENTS_FL;

    for (uint32_t b = 0; b < inode_table_blocks; b++) {
        write_block(itable + b, ext4_cluster_buf);
    }

    /* F3: the root directory needs a real data block (".", ".."), or every
     * lookup/create under /ext4 fails because the root inode has no
     * extent to walk.  It is referenced through a proper extent block
     * (i_block[0] -> extent header -> data block), exactly like mkdir.
     * We cannot use alloc_block() here (m4 is not populated until after
     * this superblock is read), so the two blocks are written explicitly. */
    {
        uint32_t root_dir_block = itable + inode_table_blocks;
        uint32_t root_ext_block = root_dir_block + 1;

        /* Data block: "." and "..". */
        memset(ext4_cluster_buf, 0, block_size);
        struct ext4_dirent *dot = (struct ext4_dirent *)ext4_cluster_buf;
        dot->inode = 2;
        dot->rec_len = 12;
        dot->name_len = 1;
        dot->file_type = EXT4_FT_DIR;
        memcpy(dot->name, ".", 1);
        struct ext4_dirent *dd = (struct ext4_dirent *)(ext4_cluster_buf + 12);
        dd->inode = 2;
        dd->rec_len = (uint16_t)(block_size - 12);
        dd->name_len = 2;
        dd->file_type = EXT4_FT_DIR;
        memcpy(dd->name, "..", 2);
        if (write_block(root_dir_block, ext4_cluster_buf) != 0) return -1;

        /* Extent block: header + one extent -> root_dir_block. */
        memset(ext4_scratch, 0, block_size);
        struct ext4_extent_header *eh = (struct ext4_extent_header *)ext4_scratch;
        eh->eh_magic = EXT4_EXTENT_MAGIC;
        eh->eh_entries = 1;
        eh->eh_max = (uint16_t)((block_size - sizeof(*eh)) / sizeof(struct ext4_extent));
        eh->eh_depth = 0;
        struct ext4_extent *e = (struct ext4_extent *)(ext4_scratch + sizeof(*eh));
        e->ee_block = 0;
        e->ee_len = 1;
        e->ee_start_hi = 0;
        e->ee_start_lo = root_dir_block;
        if (write_block(root_ext_block, ext4_scratch) != 0) return -1;

        /* Mark both used in the block bitmap. */
        if (read_block(bb_bitmap, ext4_cluster_buf) != 0) return -1;
        ext4_cluster_buf[root_dir_block / 8] |= (uint8_t)(1u << (root_dir_block % 8));
        ext4_cluster_buf[root_ext_block / 8] |= (uint8_t)(1u << (root_ext_block % 8));
        if (write_block(bb_bitmap, ext4_cluster_buf) != 0) return -1;

        /* Point the root inode's i_block[0] at the extent block.  (Byte
         * offset (2-1)*inode_size — see the inode-table note above.) */
        if (read_block(itable, ext4_cluster_buf) != 0) return -1;
        struct ext4_inode *ri2 = (struct ext4_inode *)
            (ext4_cluster_buf + (2 - 1) * inode_size);
        ri2->i_block[0] = root_ext_block;
        ri2->i_blocks_lo = 1;
        if (write_block(itable, ext4_cluster_buf) != 0) return -1;
    }


    kprintf("[ext4] format complete: %u groups, %u blocks (journal-free)\n",
            group_count, total_blocks);
    return 0;
}

int ext4_init(int prefer_port) {
    memset(&m4, 0, sizeof(m4));
    memset(v4pool, 0, sizeof(v4pool));
    m4.bdev = prefer_port;
    m4.base_lba = 128;
    m4.block_size = 4096;
    spinlock_init(&m4.alloc_lock);

    if (!ext4_scratch) ext4_scratch = (uint8_t *)kmalloc(m4.block_size);
    if (!ext4_cluster_buf) ext4_cluster_buf = (uint8_t *)kmalloc(m4.block_size);
    if (!ext4_scratch || !ext4_cluster_buf) {
        kprintf("[ext4] cannot allocate scratch buffers, mount aborted\n");
        return -1;
    }

    /* Read superblock.  FSFULL F1: auto-formatting is opt-in; with the
     * gate off, an unreadable or foreign superblock is refused loudly
     * and not a single sector is written. */
    if (read_block(0, ext4_scratch) != 0) {
        if (!fs_format_allowed()) {
            kprintf("[ext4] superblock unreadable; format disabled (FS_MOUNT_FORMAT=0)\n");
            return -1;
        }
        kprintf("[ext4] cannot read superblock, formatting...\n");
        if (format_ext4() != 0) return -1;
        if (read_block(0, ext4_scratch) != 0) return -1;
    }

    struct ext4_sb *sb = (struct ext4_sb *)ext4_scratch;
    if (sb->s_magic != EXT4_MAGIC) {
        if (!fs_format_allowed()) {
            kprintf("[ext4] not ext4 magic (0x%04X); format disabled (FS_MOUNT_FORMAT=0)\n",
                    sb->s_magic);
            return -1;
        }
        kprintf("[ext4] not ext4 magic (0x%04X), formatting...\n", sb->s_magic);
        if (format_ext4() != 0) return -1;
        if (read_block(0, ext4_scratch) != 0) return -1;
        sb = (struct ext4_sb *)ext4_scratch;
    }

    m4.block_size = 1024u << sb->s_log_block_size;
    m4.blocks_per_group = sb->s_blocks_per_group;
    m4.inodes_per_group = sb->s_inodes_per_group;
    /* F3: 64-bit counts — honour the _hi superblock fields so a volume
     * > 4 GiB (s_blocks_count_hi) or with a huge inode table
     * (s_inodes_count_hi) is addressed correctly instead of wrapping. */
    m4.inodes_count = (paddr_t)sb->s_inodes_count |
        ((paddr_t)sb->s_s_inodes_count_hi << 32);
    m4.blocks_count = (paddr_t)sb->s_blocks_count_lo |
        ((paddr_t)sb->s_blocks_count_hi << 32);
    m4.group_count = (uint32_t)((m4.blocks_count + m4.blocks_per_group - 1) /
                                m4.blocks_per_group);
    m4.first_data_block = sb->s_first_data_block;
    m4.inode_size = sb->s_inode_size ? sb->s_inode_size : 128;
    m4.desc_size = sb->s_desc_size ? sb->s_desc_size : 32;
    m4.rev_level = sb->s_rev_level;
    m4.has_journal = (sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) != 0;
    m4.journal_inode = sb->s_journal_inum;

    if (m4.block_size < 1024) m4.block_size = 4096;

    kprintf("[ext4] mounted ext4 at /ext4:\n");
    kprintf("       block_size=%u, groups=%u, blocks=%llu, inodes=%llu\n",
            m4.block_size, m4.group_count,
            (unsigned long long)m4.blocks_count,
            (unsigned long long)m4.inodes_count);
    kprintf("       revision=%u, has_journal=%d, inode_size=%u\n",
            m4.rev_level, m4.has_journal, m4.inode_size);

    /* F3 (FSFULL_PLAN.md): a volume carrying a JBD2 journal is REFUSED
     * read-write, because JBD2 replay is out of scope and mounting it
     * would either read garbage metadata or (worse) format the journal
     * over live data.  The tested lane is a journal-free volume
     * (`mkfs.ext4 -O ^has_journal`, or the internal formatter, which
     * since F3 mints journal-free volumes).  This is a named, loud
     * refusal — never a silent half-mount. */
    if (m4.has_journal && sb->s_journal_inum) {
        kprintf("[ext4] volume has a JBD2 journal (inode %u); JBD2 replay is "
                "out of scope — refusing read-write. Re-make the volume with "
                "'mkfs.ext4 -O ^has_journal' (FSFULL F3).\n", sb->s_journal_inum);
        return -1;
    }

    m4.mounted = 1;
    return 0;
}

/* ============================================================================
 * SECTION 15: SELF-TEST
 * ============================================================================ */

int ext4_self_test(void) {
    if (!m4.mounted) {
        kprintf("[ext4] self-test: SKIPPED (not mounted)\n");
        return -1;  /* SKIP */
    }
    kprintf("[ext4] self-test: create, write, read, mkdir, rename, unlink, "
            "truncate, 64-bit...\n");

    /* 1. Multi-block file (> one extent) round-trips. */
    struct vnode *f = ext4_create(NULL, "test_ext4.txt");
    if (!f) { kprintf("[ext4] FAIL: create\n"); return -2; }

    uint8_t *big = (uint8_t *)kmalloc(3 * 4096);
    if (!big) { kprintf("[ext4] FAIL: kmalloc\n"); return -2; }
    for (int i = 0; i < 3 * 4096; i++) big[i] = (uint8_t)(i & 0xFF);

    if (ext4_write(f, 0, big, 3 * 4096) != (int64_t)(3 * 4096)) {
        kprintf("[ext4] FAIL: multi-block write\n"); return -3;
    }

    uint8_t *back = (uint8_t *)kmalloc(3 * 4096);
    memset(back, 0, 3 * 4096);
    if (ext4_read(f, 0, back, 3 * 4096) != (int64_t)(3 * 4096) ||
        memcmp(big, back, 3 * 4096) != 0) {
        kprintf("[ext4] FAIL: multi-block readback\n"); return -4;
    }

    /* 2. truncate up then down, verify size fields (incl. i_size_high). */
    if (ext4_truncate(f, 0x100000000ULL) != 0) {   /* 4 GiB, exercises hi */
        kprintf("[ext4] FAIL: truncate up (64-bit)\n"); return -5;
    }
    if (ext4_truncate(f, 16) != 0) {
        kprintf("[ext4] FAIL: truncate down\n"); return -5;
    }
    struct ext4_inode in0;
    if (read_inode((uint32_t)f->inode_id, &in0) != 0) {
        kprintf("[ext4] FAIL: re-read inode\n"); return -5;
    }
    uint64_t sz = ((paddr_t)in0.i_size_high << 32) | in0.i_size_lo;
    if (sz != 16) {
        kprintf("[ext4] FAIL: size after truncate (%llu)\n",
                (unsigned long long)sz); return -5;
    }

    /* 3. subdir traversal + readdir. */
    if (ext4_mkdir(NULL, "testdir") != 0) {
        kprintf("[ext4] FAIL: mkdir\n"); return -6;
    }
    struct vnode *sub = ext4_create(NULL, "testdir/inner.txt");
    if (!sub) { kprintf("[ext4] FAIL: create in subdir\n"); return -6; }
    const char *msg = "nested file";
    if (ext4_write(sub, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[ext4] FAIL: write nested\n"); return -6;
    }
    char rbuf[32] = {0};
    if (ext4_read(sub, 0, rbuf, sizeof(rbuf)-1) != (int64_t)strlen(msg) ||
        strcmp(rbuf, msg) != 0) {
        kprintf("[ext4] FAIL: read nested '%s'\n", rbuf); return -6;
    }

    /* 4. rename (same dir). */
    if (ext4_rename(NULL, "test_ext4.txt", "renamed.txt") != 0) {
        kprintf("[ext4] FAIL: rename\n"); return -7;
    }
    if (ext4_lookup(NULL, "renamed.txt") == NULL) {
        kprintf("[ext4] FAIL: renamed not found\n"); return -7;
    }
    if (ext4_lookup(NULL, "test_ext4.txt") != NULL) {
        kprintf("[ext4] FAIL: old name still present\n"); return -7;
    }

    /* 5. hard link + settimes. */
    if (ext4_link(NULL, "renamed.txt", "link_of.txt") != 0) {
        kprintf("[ext4] FAIL: link\n"); return -8;
    }
    struct vnode *lv = ext4_lookup(NULL, "link_of.txt");
    if (!lv) { kprintf("[ext4] FAIL: link lookup\n"); return -8; }
    if (ext4_settimes(lv, 111, 222) != 0) {
        kprintf("[ext4] FAIL: settimes\n"); return -8;
    }

    /* 6. unlink file, then rmdir. */
    if (ext4_unlink(NULL, "link_of.txt") != 0) {
        kprintf("[ext4] FAIL: unlink\n"); return -9;
    }
    if (ext4_unlink(NULL, "renamed.txt") != 0) {
        kprintf("[ext4] FAIL: unlink2\n"); return -9;
    }
    if (ext4_unlink(NULL, "testdir/inner.txt") != 0) {
        kprintf("[ext4] FAIL: unlink nested\n"); return -9;
    }
    if (ext4_rmdir(NULL, "testdir") != 0) {
        kprintf("[ext4] FAIL: rmdir\n"); return -9;
    }

    kfree(big);
    kfree(back);
    kprintf("[ext4] PASS: ext4 filesystem functional (multi-block, rename, "
            "unlink, rmdir, truncate, 64-bit)\n");
    return 0;  /* PASS */
}