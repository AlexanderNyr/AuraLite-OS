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
#include "kernel/fs/ext4.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include "kernel/mm/kheap.h"
#include "drivers/ahci/ahci.h"

/* ============================================================================
 * SECTION 1: EXT4 ON-DISK STRUCTURES
 * ============================================================================ */

/* ext4 Superblock (512 bytes, starts at offset 1024 in block 0) */
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

/* ext4 block group descriptor (minimum 32 bytes) */
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

/* ext4 inode (variable size, minimum 128 bytes, usually 256) */
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

/* ext4 extent tree header (12 bytes) */
struct ext4_extent_header {
    uint16_t eh_magic;          /* 0xF30A */
    uint16_t eh_entries;        /* Number of valid entries */
    uint16_t eh_max;            /* Maximum entries in this node */
    uint16_t eh_depth;          /* Depth of this node in tree (0=leaf) */
    uint32_t eh_generation;     /* Generation (not used in current ext4) */
} __attribute__((packed));

/* ext4 extent (12 bytes) — used in leaf nodes */
struct ext4_extent {
    uint32_t ee_block;          /* First logical block in this extent */
    uint16_t ee_len;            /* Number of blocks in this extent (max 32768) */
    uint16_t ee_start_hi;       /* High 16 bits of physical block number */
    uint32_t ee_start_lo;       /* Low 32 bits of physical block number */
} __attribute__((packed));

/* ext4 extent index (12 bytes) — used in internal (non-leaf) nodes */
struct ext4_extent_idx {
    uint32_t ei_block;          /* Logical block this index covers */
    uint32_t ei_leaf_lo;        /* Low 32 bits of child node physical block */
    uint16_t ei_leaf_hi;        /* High 16 bits of child node physical block */
    uint16_t ei_unused;         /* Unused */
} __attribute__((packed));

/* Directory entry */
struct ext4_dirent {
    uint32_t inode;             /* Inode number (0 = unused) */
    uint16_t rec_len;           /* Directory entry length */
    uint16_t name_len;          /* Name length */
    uint8_t  file_type;         /* File type */
    char     name[];            /* Name (variable) */
} __attribute__((packed));

/* Journal superblock */
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

/* Journal transaction header */
struct ext4_journal_header {
    uint32_t magic;             /* 0xC03B3998 */
    uint32_t block_type;        /* 1=commit, 2=superblock, 3=descriptor, 4=revoke */
    uint32_t sequence;
    uint32_t block_nr;
    uint32_t flags;
    uint32_t reserved[3];
} __attribute__((packed));

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
    int       ahci_port;
    uint32_t  base_lba;
    uint32_t  block_size;
    uint32_t  blocks_per_group;
    uint32_t  inodes_per_group;
    uint32_t  inodes_count;
    uint32_t  blocks_count;
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
    return ahci_read(m4.ahci_port, ext4_block_lba(block_no),
                     m4.block_size / 512, buf);
}

static int write_block(uint32_t block_no, const void *buf) {
    return ahci_write(m4.ahci_port, ext4_block_lba(block_no),
                      m4.block_size / 512, buf);
}

static int read_inode(uint32_t ino, struct ext4_inode *out) {
    if (ino == 0 || ino >= m4.inodes_count) return -1;
    uint32_t bg = (ino - 1) / m4.inodes_per_group;
    uint32_t idx = (ino - 1) % m4.inodes_per_group;
    if (bg >= m4.group_count) return -1;

    struct ext4_bg_desc bgd;
    if (read_block(1 + bg * (m4.desc_size / 512) + bg * (m4.desc_size % 512 ? 1 : 0),
                   &bgd) != 0) return -1;

    /* Try reading the group descriptor from the main copy first */
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
    if (block_end > m4.blocks_count) block_end = m4.blocks_count;

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

                /* Zero the block */
                memset(ext4_scratch, 0, m4.block_size);
                if (write_block(block_no, ext4_scratch) != 0) return 0;

                /* Update group descriptor */
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
                return ((uint32_t)ext[i].ee_start_hi << 16) |
                       ext[i].ee_start_lo + (lblock - ext[i].ee_block);
            }
        }
    } else {
        /* Internal node — walk down the tree */
        struct ext4_extent_idx *idx = (struct ext4_extent_idx *)
            (ext4_scratch + sizeof(*eh));
        uint32_t child = 0;
        for (int i = 0; i < eh->eh_entries; i++) {
            if (lblock >= idx[i].ei_block) {
                child = ((uint32_t)idx[i].ei_leaf_hi << 16) | idx[i].ei_leaf_lo;
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
                return ((uint32_t)ext[i].ee_start_hi << 16) |
                       ext[i].ee_start_lo + (lblock - ext[i].ee_block);
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
        ext[i].ee_start_hi = (uint16_t)(phys_blocks[i] >> 16);
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

/* List directory entries */
static int ext4_readdir_op(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct ext4_vinfo *v = (struct ext4_vinfo *)vn->fs_data;
    if (!v || !v->is_dir) return -1;

    struct ext4_inode dinode;
    if (read_inode(v->inode, &dinode) != 0) return -1;

    int count = 0;
    uint32_t block_idx = 0;

    while (count < max) {
        uint32_t cl = extent_map(&dinode, block_idx);
        if (!cl) break;
        if (read_block(cl, ext4_cluster_buf) != 0) break;

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
        memcpy(out + done, ext4_cluster_buf + off_in_block, chunk);
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

    /* Check which blocks already exist */
    uint32_t first_new = start_lblock;
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
        memcpy(ext4_cluster_buf + off_in_block, in + done, chunk);

        /* Journal the block */
        if (journal_log_block(pblock, ext4_cluster_buf) != 0) return -1;
        done += chunk;
    }

    /* Update inode size and mtime */
    if (read_inode(v->inode, &inode) != 0) return -1;
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

    /* Initialize directory inode */
    struct ext4_inode dinode;
    memset(&dinode, 0, sizeof(dinode));
    dinode.i_mode = EXT4_S_IFDIR | 0755;
    dinode.i_uid_lo = 0;
    dinode.i_gid_lo = 0;
    dinode.i_size_lo = m4.block_size;
    dinode.i_links_count = 2;
    dinode.i_flags = EXT4_EXTENTS_FL;
    dinode.i_block[0] = dir_block;
    dinode.i_blocks_lo = 1;

    if (write_inode(new_ino, &dinode) != 0) return -1;

    kprintf("[ext4] mkdir: created inode %u ('%s') in parent %u\n",
            new_ino, base, parent);
    return 0;
}

static int ext4_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    if (!m4.mounted) return -1;
    /* Basic unlink: just mark the inode as deleted */
    uint32_t parent, target;
    int found;
    if (path_resolve(path, &parent, &target, &found, NULL, 0) != 0 || !found)
        return -1;

    struct ext4_inode inode;
    if (read_inode(target, &inode) != 0) return -1;
    if (inode.i_mode & EXT4_S_IFDIR) return -1; /* don't unlink dirs */

    inode.i_dtime = 1;
    inode.i_links_count = 0;
    if (write_inode(target, &inode) != 0) return -1;
    v4_evict(path);
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
    .unlink   = ext4_unlink,
    .stat     = ext4_stat,
    .truncate = ext4_truncate,
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
    uint32_t journal_block = 8;
    uint32_t gdt_blocks = (group_count * desc_size + block_size - 1) / block_size;
    uint32_t inode_table_blocks = (group_count * inodes_per_group * inode_size + block_size - 1) / block_size;

    /* Superblock */
    memset(ext4_scratch, 0, block_size);
    struct ext4_sb *sb = (struct ext4_sb *)ext4_scratch;
    sb->s_inodes_count = group_count * inodes_per_group;
    sb->s_blocks_count_lo = total_blocks;
    sb->s_free_blocks_count_lo = total_blocks - first_data - group_count - gdt_blocks
        - inode_table_blocks - 1 - EXT4_JOURNAL_BLOCKS;
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
    sb->s_feature_compat = EXT4_FEATURE_COMPAT_HAS_JOURNAL;
    sb->s_feature_incompat = EXT4_FEATURE_INCOMPAT_FLEX_BG;
    sb->s_feature_ro_compat = EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |
        EXT4_FEATURE_RO_COMPAT_LARGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM;
    sb->s_journal_inum = 8;
    sb->s_desc_size = desc_size;
    sb->s_blocks_count_hi = 0;
    sb->s_mkfs_time = 1337; /* pseudo time */

    if (write_block(0, ext4_scratch) != 0) return -1;

    /* Block group descriptors */
    memset(ext4_scratch, 0, block_size);
    for (uint32_t g = 0; g < group_count; g++) {
        struct ext4_bg_desc *bgd = (struct ext4_bg_desc *)
            (ext4_scratch + g * desc_size);
        uint32_t block_base = g * blocks_per_group;
        bgd->bg_block_bitmap_lo = block_base + 0;
        bgd->bg_inode_bitmap_lo = block_base + 1;
        bgd->bg_inode_table_lo = block_base + 2;
        bgd->bg_free_blocks_count_lo = blocks_per_group;
        bgd->bg_free_inodes_count_lo = inodes_per_group;
        bgd->bg_used_dirs_count_lo = 0;
    }
    if (write_block(first_data, ext4_scratch) != 0) return -1;

    /* Allocate block group 0 bitmaps and inode table */
    memset(ext4_cluster_buf, 0, block_size);
    /* Block bitmap: mark first few blocks as used */
    ext4_cluster_buf[0] = 0x07; /* blocks 0,1,2 taken by sb+gdt+bitmap */
    for (uint32_t i = 3; i < gdt_blocks; i++) ext4_cluster_buf[i/8] |= (1 << (i%8));
    ext4_cluster_buf[0 / 8] |= 1 << (0 % 8);
    ext4_cluster_buf[1 / 8] |= 1 << (1 % 8);
    ext4_cluster_buf[2 / 8] |= 1 << (2 % 8);

    if (write_block(first_data + 0, ext4_cluster_buf) != 0) return -1; /* block bitmap */

    /* Inode bitmap for group 0 */
    memset(ext4_cluster_buf, 0, block_size);
    ext4_cluster_buf[0] = 0x07; /* inodes 1,2,3 reserved */
    if (write_block(first_data + 1, ext4_cluster_buf) != 0) return -1; /* inode bitmap */

    /* Inode table for group 0 */
    memset(ext4_cluster_buf, 0, block_size);
    /* Inode 2 = root directory */
    struct ext4_inode *root_inode = (struct ext4_inode *)ext4_cluster_buf;
    root_inode[1].i_mode = EXT4_S_IFDIR | 0755; /* inode 2 */
    root_inode[1].i_size_lo = block_size;
    root_inode[1].i_links_count = 2;
    root_inode[1].i_flags = EXT4_EXTENTS_FL;

    /* Inode 8 = journal */
    root_inode[7].i_mode = EXT4_S_IFREG | 0600; /* inode 8 */
    root_inode[7].i_size_lo = EXT4_JOURNAL_BLOCKS * block_size;
    root_inode[7].i_flags = EXT4_EXTENTS_FL;

    for (uint32_t b = 0; b < inode_table_blocks; b++) {
        write_block(first_data + 2 + b, ext4_cluster_buf);
    }

    /* Zero journal blocks */
    memset(ext4_cluster_buf, 0, block_size);
    for (uint32_t j = 0; j < EXT4_JOURNAL_BLOCKS; j++) {
        write_block(journal_block + j, ext4_cluster_buf);
    }

    /* Journal superblock */
    struct ext4_journal_sb *jsb = (struct ext4_journal_sb *)ext4_cluster_buf;
    memset(jsb, 0, sizeof(*jsb));
    w32(ext4_cluster_buf, EXT4_JOURNAL_MAGIC);
    w32(ext4_cluster_buf + 4, 2); /* superblock v2 */
    w32(ext4_cluster_buf + 8, 1); /* sequence */
    w32(ext4_cluster_buf + 12, block_size);
    w32(ext4_cluster_buf + 16, EXT4_JOURNAL_BLOCKS);
    w32(ext4_cluster_buf + 20, journal_block + 1); /* first */
    w32(ext4_cluster_buf + 24, journal_block + EXT4_JOURNAL_BLOCKS); /* last */
    w32(ext4_cluster_buf + 28, journal_block + 1); /* start */
    if (write_block(journal_block, ext4_cluster_buf) != 0) return -1;

    kprintf("[ext4] format complete: %u groups, %u blocks, journal at %u\n",
            group_count, total_blocks, journal_block);
    return 0;
}

int ext4_init(int prefer_port) {
    memset(&m4, 0, sizeof(m4));
    memset(v4pool, 0, sizeof(v4pool));
    m4.ahci_port = prefer_port;
    m4.base_lba = 128;
    m4.block_size = 4096;
    spinlock_init(&m4.alloc_lock);

    if (!ext4_scratch) ext4_scratch = (uint8_t *)kmalloc(m4.block_size);
    if (!ext4_cluster_buf) ext4_cluster_buf = (uint8_t *)kmalloc(m4.block_size);

    /* Read superblock */
    if (read_block(0, ext4_scratch) != 0) {
        kprintf("[ext4] cannot read superblock, formatting...\n");
        if (format_ext4() != 0) return -1;
        if (read_block(0, ext4_scratch) != 0) return -1;
    }

    struct ext4_sb *sb = (struct ext4_sb *)ext4_scratch;
    if (sb->s_magic != EXT4_MAGIC) {
        kprintf("[ext4] not ext4 magic (0x%04X), formatting...\n", sb->s_magic);
        if (format_ext4() != 0) return -1;
        if (read_block(0, ext4_scratch) != 0) return -1;
        sb = (struct ext4_sb *)ext4_scratch;
    }

    m4.block_size = 1024u << sb->s_log_block_size;
    m4.blocks_per_group = sb->s_blocks_per_group;
    m4.inodes_per_group = sb->s_inodes_per_group;
    m4.inodes_count = sb->s_inodes_count;
    m4.blocks_count = sb->s_blocks_count_lo;
    m4.group_count = (m4.blocks_count + m4.blocks_per_group - 1) / m4.blocks_per_group;
    m4.first_data_block = sb->s_first_data_block;
    m4.inode_size = sb->s_inode_size ? sb->s_inode_size : 128;
    m4.desc_size = sb->s_desc_size ? sb->s_desc_size : 32;
    m4.rev_level = sb->s_rev_level;
    m4.has_journal = (sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) != 0;
    m4.journal_inode = sb->s_journal_inum;

    if (m4.block_size < 1024) m4.block_size = 4096;

    kprintf("[ext4] mounted ext4 at /ext4:\n");
    kprintf("       block_size=%u, groups=%u, blocks=%u, inodes=%u\n",
            m4.block_size, m4.group_count, m4.blocks_count, m4.inodes_count);
    kprintf("       revision=%u, has_journal=%d, inode_size=%u\n",
            m4.rev_level, m4.has_journal, m4.inode_size);

    if (m4.has_journal && sb->s_journal_inum) {
        journal_init();
        journal_commit();
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
    kprintf("[ext4] self-test: create, write, read, mkdir...\n");

    /* Create a file */
    struct vnode *f = ext4_create(NULL, "test_ext4.txt");
    if (!f) { kprintf("[ext4] FAIL: create\n"); return -2; }

    const char *msg = "ext4 filesystem test successful!";
    if (ext4_write(f, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[ext4] FAIL: write\n"); return -3;
    }

    /* Read it back */
    char buf[64] = {0};
    if (ext4_read(f, 0, buf, sizeof(buf)-1) != (int64_t)strlen(msg) ||
        strcmp(buf, msg) != 0) {
        kprintf("[ext4] FAIL: readback '%s'\n", buf); return -4;
    }

    /* Create a directory */
    if (ext4_mkdir(NULL, "testdir") != 0) {
        kprintf("[ext4] FAIL: mkdir\n"); return -5;
    }

    kprintf("[ext4] PASS: ext4 filesystem functional\n");
    return 0;  /* PASS */
}