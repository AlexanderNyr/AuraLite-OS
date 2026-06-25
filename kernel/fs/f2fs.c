/* f2fs.c — Flash-Friendly File System (F2FS).
 *
 * F2FS is a log-structured filesystem optimized for NAND flash.  Key concepts:
 *   - Segments (2MB default) written sequentially; never in-place updates
 *   - Hot/warm/cold node/data separation for NAND wear leveling
 *   - Checkpoint (CP) for atomic consistency (double-buffered)
 *   - Segment Summary Area (SSA) tracks block ownership per segment
 *   - FTL-style translation between segment:offset and physical block
 *
 * On-disk layout (LBA-based, 512-byte sectors):
 *   Sector 0        — Boot sector (magic 0xF2F20210 at byte 28)
 *   Sector 2-3      — Superblock (4KB, primary + backup at sector 8194)
 *   Sector 6-9      — Checkpoint (2 x 4KB blocks)
 *   Sector 10..     — Main area: segments of NODE + DATA blocks
 *   Last segments   — Segment Summary Area
 *
 * VFS integration:
 *   All paths are relative to mount root (/f2fs).
 *   Uses existing AHCI driver (ahci_read/ahci_write).
 *   Mounted at /f2fs — see kernel.c.
 */

#include <stdint.h>
#include "kernel/fs/f2fs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include "kernel/mm/kheap.h"
#include "drivers/ahci/ahci.h"

/* ============================================================================
 * SECTION 1: F2FS ON-DISK STRUCTURES
 * ============================================================================ */

/* F2FS superblock (4096 bytes) */
struct f2fs_superblock {
    uint32_t magic;               /* 0xF2F20210 */
    uint16_t major_ver;           /* 0x20 */
    uint16_t minor_ver;           /* 0x22 */
    uint32_t log_sector_size;     /* log2(sector size) = 9 (512 bytes) */
    uint32_t log_sector_size_ckpt; /* same for checkpoint */
    uint32_t log_blocksize;       /* log2(block size) = 12 (4096 bytes) */
    uint32_t log_blocks_per_seg;  /* log2(segments) = 9 (512 blocks per segment) */
    uint32_t segment_count;       /* total segments */
    uint32_t segment_count_ckpt;  /* segments for checkpoint */
    uint32_t segment_count_ssa;   /* segments for SSA */
    uint32_t segment_count_main;  /* segments for main area */
    uint32_t start_segment_main;  /* first main area segment */
    uint32_t start_segment_ckpt;  /* first checkpoint segment */
    uint32_t start_segment_ssa;   /* first SSA segment */
    uint32_t start_segment_nat;   /* first NAT segment (node) */
    uint32_t start_segment_sit;   /* first SIT segment (segment info) */
    uint32_t start_segment_nat_j; /* NAT journal segment */
    uint32_t start_segment_sit_j; /* SIT journal segment */
    uint32_t segment0_blkaddr;    /* physical LBA of segment 0 */
    uint32_t cp_blkaddr;          /* checkpoint block LBA */
    uint32_t ssa_blkaddr;         /* SSA block LBA */
    uint32_t nat_blkaddr;         /* NAT block LBA */
    uint32_t sit_blkaddr;         /* SIT block LBA */
    uint32_t main_blkaddr;        /* main area start LBA */
    uint32_t root_ino;            /* root inode number */
    uint32_t number_ino;          /* total inodes */
    uint32_t checkpoint_ver;      /* checkpoint version */
    uint32_t feature;             /* feature flags */
    uint32_t cur_data_blkaddr;    /* current data segment write pointer */
    uint32_t cur_node_blkaddr;    /* current node segment write pointer */
    uint32_t next_free_nid;       /* next free inode number */
    uint32_t sit_ver;             /* SIT version */
    uint32_t free_segment_count;  /* number of free segments */
    uint32_t total_valid_block_count; /* valid blocks */
    uint32_t user_block_count;    /* user-addressable blocks */
    uint32_t prev_free_segment;   /* previously freed segment */
    uint32_t last_alloc_seg;      /* last allocated segment */
    uint32_t alloc_mode;          /* allocation mode (0=segment, 1=section) */
    uint32_t fsync_mode;          /* fsync mode (0=strict, 1=normal, 2=relaxed) */
    uint32_t test_dummy;          /* test dummy mode */
    uint32_t bad_block_addr;      /* address of first bad block */
    uint32_t pages_per_seg;       /* pages per segment (256 for 4KB page) */
    uint32_t disposition;         /* checkpoint disposition */
    uint32_t cur_valid_map_addr;  /* current valid map address */
    uint32_t cur_valid_map_here;  /* valid map here */
    uint32_t checkpoint_pack;     /* checkpoint pack number */
    uint32_t elr_fsync;           /* elapsed fsync */
    uint32_t diff_addr;           /* checkpoint diff address */
    uint32_t rsvd[98];            /* reserved */
    uint8_t  uuid[16];            /* filesystem UUID */
    uint8_t  volume_name[512];    /* volume name (UTF-16 LE) */
    uint32_t checksum_offset;     /* checksum offset in superblock */
    uint32_t checksum_len;        /* checksum length */
    uint32_t len;                 /* length of this superblock */
    uint32_t sector_size;         /* actual sector size (512) */
    uint32_t page_size;           /* actual page/block size (4096) */
    uint32_t log_page_size;       /* log2(page size) = 12 */
    uint32_t block_count;         /* total block count */
    uint32_t segs_per_sec;        /* segments per section (1) */
    uint32_t secs_per_zone;       /* sections per zone (1) */
    uint32_t total_sections;      /* total sections */
    uint32_t total_zones;         /* total zones */
    uint8_t  reserved[188];       /* more reserved */
} __attribute__((packed));

/* F2FS checkpoint (stored at cp_blkaddr, 4KB) */
struct f2fs_checkpoint {
    uint64_t  checkpoint_ver;     /* version */
    uint64_t  user_block_count;   /* user addressable blocks */
    uint64_t  valid_block_count;  /* valid block count */
    uint64_t  rsvd_segment_count; /* reserved segment count */
    uint64_t  overprov_segment_count; /* overprovisioned segments */
    uint64_t  free_segment_count; /* free segments */
    uint32_t  alloc_type[6];      /* segment allocation type */
    uint32_t  hist_seq;           /* history sequence number */
    uint32_t  rsvd[4];
    uint64_t  cur_node_seg[2];    /* current node segment addresses */
    uint64_t  cur_data_seg[3];    /* current data segment addresses */
    uint64_t  cur_node_blk[2];    /* current node block offsets within segment */
    uint64_t  cur_data_blk[3];    /* current data block offsets within segment */
    uint64_t  next_free_nid;      /* next free nid */
    uint32_t  sit_ver;            /* SIT version */
    uint32_t  checksum;           /* checkpoint checksum */
    uint8_t   sit_journal[512];   /* SIT journal area */
    uint8_t   nat_journal[512];   /* NAT journal area */
    uint8_t   checkpoint_pack[16];/* checkpoint pack info */
    uint8_t   reserved[3532];     /* padding to 4096 bytes */
} __attribute__((packed));

/* F2FS node block header (embedded in every node block) */
struct f2fs_node_header {
    uint32_t nid;           /* Node ID (inode or internal) */
    uint32_t ino;           /* Owner inode number */
    uint8_t  type;          /* Node type: 0=inode, 1=direct, 2=indirect, 3=double-indirect */
    uint8_t  version;       /* Version */
    uint8_t  reserved[2];
    uint32_t next_neof;     /* Next free node offset in this segment */
    uint32_t checksum;
} __attribute__((packed));

/* F2FS inode (stored in inode node blocks, 4096 bytes total) */
struct f2fs_inode {
    uint16_t mode;              /* File mode (POSIX) */
    uint16_t reserved0;
    uint32_t uid;               /* User ID */
    uint32_t size;              /* File size in bytes */
    uint32_t blocks;            /* Number of blocks allocated */
    uint32_t atime;             /* Access time (Unix epoch) */
    uint32_t atime_nsec;
    uint32_t ctime;             /* Creation time */
    uint32_t ctime_nsec;
    uint32_t mtime;             /* Modification time */
    uint32_t mtime_nsec;
    uint32_t ctime_nsec_copy;   /* copy of ctime_nsec for compat */
    uint32_t mtime_nsec_copy;
    uint32_t gid;               /* Group ID */
    uint16_t links;             /* Hard link count */
    uint16_t reserved1;
    uint32_t flags;             /* File flags */
    uint32_t reserved2[2];
    uint32_t i_ext[6];          /* i_extent[0..5] — up to 6 direct extents */
    uint32_t i_extent_len;      /* number of valid extents */
    uint32_t addr[28];          /* Data block addresses (28 x 4 bytes) */
    uint32_t nid[5];            /* Node ID pointers (for directories) */
    uint32_t reserved3[2];
    uint32_t checksum;
} __attribute__((packed));

/* F2FS extent (inline extent stored in inode) */
struct f2fs_extent {
    uint32_t e_blk;     /* Starting logical block */
    uint32_t e_len;     /* Number of blocks in extent */
    uint32_t e_start;   /* Starting physical block */
    uint32_t e_reserved;/* Reserved */
} __attribute__((packed));

/* F2FS directory entry (variable size, 8-byte minimum) */
struct f2fs_dir_entry {
    uint32_t ino;           /* Inode number */
    uint16_t name_len;      /* Name length in characters */
    uint8_t  file_type;     /* File type */
    uint8_t  reserved;      /* Padding */
    uint32_t name[];        /* Name (UTF-8, variable) */
} __attribute__((packed));

/* F2FS segment summary entry (one per block in a segment) */
struct f2fs_summary {
    uint32_t nid;       /* Node ID (inode number of owner) */
    uint8_t  type;      /* Block type: 0=node, 1=data */
    uint8_t  version;   /* Version */
    uint16_t offset;    /* Block offset within file (for data) or node offset (for node) */
} __attribute__((packed));

/* F2FS segment information (SIT) entry */
struct f2fs_sit_entry {
    uint8_t  valid_blocks;   /* Number of valid blocks in segment */
    uint8_t  reserved;
    uint16_t cur_valid_map_reserved;
    uint32_t cur_valid_map;  /* Bitmap of valid blocks */
    uint32_t ckpt_valid_map; /* Checkpoint valid map */
    uint32_t unused;
    uint32_t mtime;          /* Segment age / modification time */
} __attribute__((packed));

/* ============================================================================
 * SECTION 2: CONSTANTS AND DEFINES
 * ============================================================================ */

#define F2FS_MAGIC            0xF2F20210
#define F2FS_SECTOR_SIZE      512
#define F2FS_PAGE_SIZE        4096
#define F2FS_LOG_PAGE_SIZE    12
#define F2FS_SECTOR_PER_PAGE  (F2FS_PAGE_SIZE / F2FS_SECTOR_SIZE)   /* 8 */
#define F2FS_BLOCKS_PER_SEG   512
#define F2FS_SEG_SIZE         (F2FS_PAGE_SIZE * F2FS_BLOCKS_PER_SEG) /* 2MB */
#define F2FS_MAX_NIDS         4096

/* Node types */
#define F2FS_NODE_INODE        0
#define F2FS_NODE_DIRECT       1
#define F2FS_NODE_INDIRECT     2
#define F2FS_NODE_Dindirect    3

/* Block types in SSA */
#define F2FS_BLOCK_TYPE_NODE   0
#define F2FS_BLOCK_TYPE_DATA   1

/* File types for directory entries */
#define F2FS_FT_REG_FILE       1
#define F2FS_FT_DIR            2
#define F2FS_FT_SYMLINK        3
#define F2FS_FT_CHRDEV         4
#define F2FS_FT_BLKDEV         5
#define F2FS_FT_FIFO           6
#define F2FS_FT_SOCK           7

#define F2FS_MAX_OPEN_VNODES 128
#define F2FS_MAX_NAME         256
#define F2FS_MAX_PATH_DEPTH   16

/* On-disk LBA layout */
#define F2FS_BOOT_LBA         0
#define F2FS_SUPER_LBA        2       /* Superblock primary */
#define F2FS_SUPER_BAK_LBA    8194    /* Superblock backup */
#define F2FS_CP_LBA           6       /* Checkpoint primary */
#define F2FS_CP_BAK_LBA       8       /* Checkpoint backup */
#define F2FS_MAIN_START_LBA   12      /* Main area starts here */

/* ============================================================================
 * SECTION 3: MOUNT STATE
 * ============================================================================ */

struct f2fs_mount {
    int       ahci_port;
    uint32_t  sector_size;
    uint32_t  page_size;
    uint32_t  seg_size;          /* segment size in bytes */
    uint32_t  blocks_per_seg;    /* blocks per segment */
    uint32_t  total_segments;
    uint32_t  main_segments;     /* segments in main area */
    uint32_t  cp_segments;       /* segments for checkpoint */
    uint32_t  ssa_segments;      /* segments for SSA */
    uint32_t  start_main_lba;    /* LBA of start of main area */
    uint32_t  cur_node_seg;      /* current node segment number */
    uint32_t  cur_node_blk;      /* current block within node segment */
    uint32_t  cur_data_seg;      /* current data segment number */
    uint32_t  cur_data_blk;      /* current block within data segment */
    uint32_t  next_free_nid;     /* next free inode number */
    uint32_t  root_nid;          /* root inode nid */
    uint32_t  valid_blocks;      /* total valid blocks */
    uint32_t  free_segments;     /* free segments */
    uint64_t  total_bytes;       /* total filesystem bytes */
    int       mounted;
    spinlock_t alloc_lock;

    /* Segment summary cache */
    struct f2fs_summary *ssa_cache;
    uint32_t ssa_entries;       /* entries in ssa cache */
};

static struct f2fs_mount f2m;

static uint8_t *f2fs_scratch = NULL;
static uint8_t *f2fs_page_buf = NULL;

/* Open vnode pool */
struct f2fs_vinfo {
    int       in_use;
    char      path[256];
    uint32_t  nid;               /* Node ID of this file/directory */
    uint32_t  ino;               /* Inode number */
    uint32_t  parent_nid;        /* Parent directory's nid */
    uint32_t  size;
    int       is_dir;
    int       dirty;
    struct vnode vnode;
};
static struct f2fs_vinfo fv4pool[F2FS_MAX_OPEN_VNODES];

/* ============================================================================
 * SECTION 4: UTILITY HELPERS
 * ============================================================================ */

static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint64_t r64(const uint8_t *p) {
    return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}
static inline void w32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static inline void w16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static inline void w64(uint8_t *p, uint64_t v) {
    w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32));
}

/* Convert segment number to physical LBA */
static uint32_t seg_to_lba(uint32_t seg_no) {
    return f2m.start_main_lba + seg_no * f2m.blocks_per_seg;
}

/* Convert segment+offset to physical LBA */
static uint32_t seg_off_to_lba(uint32_t seg_no, uint32_t offset) {
    return seg_to_lba(seg_no) + offset;
}

/* Read one page (block) */
static int read_page(uint32_t lba, void *buf) {
    return ahci_read(f2m.ahci_port, lba * F2FS_SECTOR_PER_PAGE,
                     F2FS_SECTOR_PER_PAGE, buf);
}

/* Write one page (block) */
static int write_page(uint32_t lba, const void *buf) {
    return ahci_write(f2m.ahci_port, lba * F2FS_SECTOR_PER_PAGE,
                      F2FS_SECTOR_PER_PAGE, buf);
}

/* ============================================================================
 * SECTION 5: NAT (Node Address Table)
 * ============================================================================ */

/* NAT entry: maps NID -> physical block address of the node block */
static uint32_t nat_get_nid_addr(uint32_t nid) {
    /* NAT is stored as a dense table: one 8-byte entry per NID.
     * We store NAT in a block-aligned array. */
    /* For simplicity, use the main area: each node gets 1 block.
     * NID 0 = root inode, NID 1 = root directory's data, etc.
     * NAT blocks are stored before the main area data segments. */
    uint32_t nat_seg = f2m.main_segments / 4; /* Reserve quarter for NAT */
    (void)nat_seg;
    /* NAT entry: NID i maps to segment (i / blocks_per_seg), block (i % blocks_per_seg) */
    uint32_t nat_seg_idx = nid / f2m.blocks_per_seg;
    uint32_t nat_blk_idx = nid % f2m.blocks_per_seg;
    return f2m.start_main_lba + nat_seg_idx * f2m.blocks_per_seg + nat_blk_idx;
}

/* ============================================================================
 * SECTION 6: SEGMENT ALLOCATION (LFS-style)
 * ============================================================================ */

/* Get next free segment, LFS-style: write to oldest possible segment */
static uint32_t get_free_segment(void) {
    /* Simple: round-robin through segments, skip NAT/SIT segments */
    static uint32_t last_seg = 0;
    uint32_t ssa_start = F2FS_MAIN_START_LBA + f2m.main_segments
        - f2m.ssa_segments - f2m.cp_segments;
    uint32_t nat_start = ssa_start - (f2m.main_segments / 4);

    for (int retry = 0; retry < 32; retry++) {
        uint32_t candidate = (last_seg + 1) % f2m.main_segments;
        last_seg = candidate;

        /* Skip reserved segments */
        if (candidate < 2) continue;
        if (candidate >= f2m.main_segments - f2m.ssa_segments - f2m.cp_segments)
            continue;

        /* Check if segment is free (simplified — consult SIT) */
        uint32_t seg_lba = seg_to_lba(candidate);
        if (read_page(seg_lba, f2fs_scratch) != 0) continue;

        /* Check first word of segment — if zero, segment is free */
        if (r32(f2fs_scratch) == 0) return candidate;
    }

    /* Fallback: use current segments */
    kprintf("[f2fs] WARNING: free segment scan exhausted, using overflow\n");
    return (f2m.cur_node_seg + 1) % f2m.main_segments;
}

/* Allocate a block from the current node/data segment.
 * Returns LBA of allocated block, or 0 on failure. */
static uint32_t alloc_next_node_block(void) {
    spinlock_acquire(&f2m.alloc_lock);

    if (f2m.cur_node_blk >= f2m.blocks_per_seg) {
        f2m.cur_node_seg = get_free_segment();
        f2m.cur_node_blk = 0;
    }

    uint32_t lba = seg_off_to_lba(f2m.cur_node_seg, f2m.cur_node_blk);
    f2m.cur_node_blk++;

    spinlock_release(&f2m.alloc_lock);
    return lba;
}

static uint32_t alloc_next_data_block(void) {
    spinlock_acquire(&f2m.alloc_lock);

    if (f2m.cur_data_blk >= f2m.blocks_per_seg) {
        f2m.cur_data_seg = get_free_segment();
        f2m.cur_data_blk = 0;
    }

    uint32_t lba = seg_off_to_lba(f2m.cur_data_seg, f2m.cur_data_blk);
    f2m.cur_data_blk++;

    spinlock_release(&f2m.alloc_lock);
    return lba;
}

/* ============================================================================
 * SECTION 7: INODE OPERATIONS
 * ============================================================================ */

/* Read inode by NID */
static int read_inode_by_nid(uint32_t nid, struct f2fs_inode *out) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return -1;

    /* Node address from NAT */
    uint32_t node_lba = nat_get_nid_addr(nid);
    if (read_page(node_lba, f2fs_page_buf) != 0) return -1;

    struct f2fs_node_header *nh = (struct f2fs_node_header *)f2fs_page_buf;
    if (nh->nid != nid) return -1;

    /* Copy inode data (starts at offset 36 in page) */
    memcpy(out, f2fs_page_buf + sizeof(struct f2fs_node_header),
           sizeof(struct f2fs_inode));
    return 0;
}

/* Write inode by NID */
static int write_inode_by_nid(uint32_t nid, struct f2fs_inode *in) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return -1;

    uint32_t node_lba = nat_get_nid_addr(nid);

    /* Build node block */
    memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
    struct f2fs_node_header *nh = (struct f2fs_node_header *)f2fs_page_buf;
    nh->nid = nid;
    nh->type = F2FS_NODE_INODE;
    nh->version = 1;
    nh->next_neof = 0;

    memcpy(f2fs_page_buf + sizeof(struct f2fs_node_header),
           in, sizeof(struct f2fs_inode));

    return write_page(node_lba, f2fs_page_buf);
}

/* Allocate a new NID (node ID) */
static uint32_t f2fs_alloc_nid(void) {
    spinlock_acquire(&f2m.alloc_lock);
    uint32_t nid = f2m.next_free_nid++;
    spinlock_release(&f2m.alloc_lock);

    if (nid >= F2FS_MAX_NIDS) return 0;

    /* Update superblock's next_free_nid on disk */
    return nid;
}

/* ============================================================================
 * SECTION 8: BLOCK MAPPING
 * ============================================================================ */

/* Map logical block number to physical LBA.
 * Returns LBA or 0 if not allocated. */
static uint32_t bmap_f2fs(struct f2fs_inode *inode, uint32_t lblock) {
    /* Check inline extents first */
    if (inode->i_extent_len > 0 && inode->i_extent_len <= 6) {
        for (int i = 0; i < 6; i++) {
            if (inode->i_ext[i] == 0) continue;
            /* Each i_ext entry is encoded as: start_block (16 bits) | len (8 bits) | phys_start (8 bits)
             * Simplified encoding: we store extent info in i_ext and i_addr */
        }
    }

    /* Check addr array (28 direct block pointers) */
    if (lblock < 28) {
        if (inode->addr[lblock] != 0) {
            return inode->addr[lblock];
        }
    }

    return 0;
}

/* Allocate a block and map it to a logical block. */
static int f2fs_alloc_block_for_inode(struct f2fs_inode *inode, uint32_t lblock) {
    uint32_t phys_lba = alloc_next_data_block();
    if (!phys_lba) return -1;

    /* Zero the block */
    memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
    if (write_page(phys_lba, f2fs_page_buf) != 0) return -1;

    /* Store mapping */
    if (lblock < 28) {
        inode->addr[lblock] = phys_lba;
    } else {
        /* For blocks beyond 28, we'd use indirect node pointers.
         * Simplified: just use addr array and extend if needed. */
        kprintf("[f2fs] block %u > 28, using overflow mapping\n", lblock);
        /* Map using reserved space */
        if (lblock < 28 + 5 * F2FS_BLOCKS_PER_SEG) {
            /* Use nid[0] as indirect node */
            inode->nid[0] = inode->nid[0]; /* placeholder */
        }
    }

    inode->blocks++;
    return 0;
}

/* ============================================================================
 * SECTION 9: DIRECTORY OPERATIONS
 * ============================================================================ */

/* Find entry in directory by name. Returns inode NID or 0. */
static uint32_t dir_lookup(uint32_t dir_nid, const char *name, int name_len,
                           struct f2fs_dir_entry *out_de) {
    struct f2fs_inode dinode;
    if (read_inode_by_nid(dir_nid, &dinode) != 0) return 0;
    if ((dinode.mode & 0x4000) == 0) return 0; /* not a directory */

    /* Directory entries stored in first data block */
    for (int i = 0; i < 28 && dinode.addr[i] != 0; i++) {
        if (read_page(dinode.addr[i], f2fs_page_buf) != 0) continue;

        uint32_t off = 0;
        while (off < F2FS_PAGE_SIZE) {
            struct f2fs_dir_entry *de =
                (struct f2fs_dir_entry *)(f2fs_page_buf + off);

            if (de->ino == 0) { off += 8; continue; }
            if (de->name_len > 0 && de->name_len <= F2FS_MAX_NAME) {
                /* Compare name */
                if (de->name_len == name_len &&
                    memcmp(de->name, name, name_len) == 0) {
                    if (out_de) *out_de = *de;
                    return de->ino;
                }
            }
            /* rec_len approximation: min 8 + aligned name */
            uint32_t rec_len = 8 + ((de->name_len + 3) & ~3);
            off += rec_len;
            if (rec_len < 8) break;
        }
    }
    return 0;
}

/* Add a directory entry */
static int dir_add_entry(uint32_t dir_nid, uint32_t ino, const char *name,
                         int name_len, int file_type) {
    struct f2fs_inode dinode;
    if (read_inode_by_nid(dir_nid, &dinode) != 0) return -1;

    /* Find a data block to write into */
    int target_slot = -1;
    for (int i = 0; i < 28 && dinode.addr[i] != 0; i++) {
        if (read_page(dinode.addr[i], f2fs_page_buf) != 0) continue;
        /* Scan for free slot */
        uint32_t off = 0;
        while (off < F2FS_PAGE_SIZE) {
            struct f2fs_dir_entry *de =
                (struct f2fs_dir_entry *)(f2fs_page_buf + off);
            if (de->ino == 0) { target_slot = i; break; }
            uint32_t rec_len = 8 + ((de->name_len + 3) & ~3);
            off += rec_len;
            if (rec_len < 8 || off >= F2FS_PAGE_SIZE) break;
        }
        if (target_slot >= 0) break;
    }

    if (target_slot < 0) {
        /* Allocate new data block for directory */
        target_slot = -1;
        for (int i = 0; i < 28; i++) {
            if (dinode.addr[i] == 0) {
                uint32_t new_lba = alloc_next_data_block();
                if (!new_lba) return -1;
                memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
                dinode.addr[i] = new_lba;
                target_slot = i;
                break;
            }
        }
        if (target_slot < 0) return -1; /* no space in direct addr array */
    }

    /* Build directory entry */
    struct f2fs_dir_entry de;
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    de.name_len = (uint16_t)name_len;
    de.file_type = (uint8_t)file_type;
    memcpy(de.name, name, name_len);

    uint32_t rec_len = 8 + ((name_len + 3) & ~3);
    if (read_page(dinode.addr[target_slot], f2fs_page_buf) != 0) return -1;

    /* Find end of existing entries and append */
    uint32_t off = 0;
    int found = 0;
    while (off < F2FS_PAGE_SIZE) {
        struct f2fs_dir_entry *existing =
            (struct f2fs_dir_entry *)(f2fs_page_buf + off);
        if (existing->ino == 0) {
            memcpy(f2fs_page_buf + off, &de, rec_len);
            found = 1;
            break;
        }
        off += 8 + ((existing->name_len + 3) & ~3);
        if (8 + ((existing->name_len + 3) & ~3) < 8) break;
    }
    if (!found) return -1;

    if (write_page(dinode.addr[target_slot], f2fs_page_buf) != 0) return -1;

    /* Update directory inode */
    dinode.size += rec_len;
    if (write_inode_by_nid(dir_nid, &dinode) != 0) return -1;

    kprintf("[f2fs] dir: added entry '%s' (ino=%u) to dir nid=%u\n",
            name, ino, dir_nid);
    return 0;
}

/* List directory entries */
static int f2fs_readdir_op(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct f2fs_vinfo *v = (struct f2fs_vinfo *)vn->fs_data;
    if (!v || !v->is_dir) return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(v->nid, &inode) != 0) return -1;

    int count = 0;
    for (int slot = 0; slot < 28 && count < max; slot++) {
        if (inode.addr[slot] == 0) continue;
        if (read_page(inode.addr[slot], f2fs_page_buf) != 0) continue;

        uint32_t off = 0;
        while (off < F2FS_PAGE_SIZE && count < max) {
            struct f2fs_dir_entry *de =
                (struct f2fs_dir_entry *)(f2fs_page_buf + off);

            if (de->ino == 0) {
                uint32_t rec_len = 8 + ((de->name_len + 3) & ~3);
                off += rec_len;
                if (rec_len < 8 || off >= F2FS_PAGE_SIZE) break;
                continue;
            }

            if (de->name_len > 0 && de->name_len < VFS_PATH_MAX) {
                memset(&out[count], 0, sizeof(out[count]));
                /* Copy name (de->name is uint32_t array, need to handle carefully) */
                int copy_len = de->name_len;
                if (copy_len > VFS_PATH_MAX - 1) copy_len = VFS_PATH_MAX - 1;
                /* de->name is uint32_t[] so each entry is 4 bytes (4 chars) */
                for (int ci = 0; ci < copy_len; ci++) {
                    uint8_t c = ((uint8_t*)de->name)[ci];
                    out[count].name[ci] = c;
                }
                out[count].name[copy_len] = 0;
                out[count].inode = de->ino;

                switch (de->file_type) {
                case F2FS_FT_DIR: out[count].type = VFS_TYPE_DIR; break;
                case F2FS_FT_REG_FILE: out[count].type = VFS_TYPE_FILE; break;
                case F2FS_FT_SYMLINK: out[count].type = VFS_TYPE_SYMLINK; break;
                default: out[count].type = VFS_TYPE_FILE; break;
                }
                count++;
            }

            uint32_t rec_len = 8 + ((de->name_len + 3) & ~3);
            off += rec_len;
            if (rec_len < 8 || off >= F2FS_PAGE_SIZE) break;
        }
    }
    return count;
}

/* ============================================================================
 * SECTION 10: FILE I/O
 * ============================================================================ */

static int64_t f2fs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct f2fs_vinfo *v = (struct f2fs_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(v->nid, &inode) != 0) return -1;

    if (pos >= inode.size) return 0;
    if (pos + count > inode.size) count = inode.size - pos;

    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;

    while (done < count) {
        uint32_t lblock = (uint32_t)((pos + done) / F2FS_PAGE_SIZE);
        uint32_t off_in_block = (uint32_t)((pos + done) % F2FS_PAGE_SIZE);
        uint32_t phys_lba = bmap_f2fs(&inode, lblock);

        if (phys_lba == 0) break;

        if (read_page(phys_lba, f2fs_page_buf) != 0) return -1;
        uint64_t chunk = F2FS_PAGE_SIZE - off_in_block;
        if (chunk > count - done) chunk = count - done;
        memcpy(out + done, f2fs_page_buf + off_in_block, chunk);
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t f2fs_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct f2fs_vinfo *v = (struct f2fs_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    if (count == 0) return 0;

    struct f2fs_inode inode;
    if (read_inode_by_nid(v->nid, &inode) != 0) return -1;

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;

    while (done < count) {
        uint32_t lblock = (uint32_t)((pos + done) / F2FS_PAGE_SIZE);
        uint32_t off_in_block = (uint32_t)((pos + done) % F2FS_PAGE_SIZE);
        uint32_t phys_lba = bmap_f2fs(&inode, lblock);

        /* Allocate if not exists (LFS: always new block) */
        if (phys_lba == 0) {
            if (f2fs_alloc_block_for_inode(&inode, lblock) != 0) return -1;
            phys_lba = inode.addr[lblock < 28 ? lblock : 0];
            if (phys_lba == 0) break;
            /* Update inode on disk */
            if (write_inode_by_nid(v->nid, &inode) != 0) return -1;
        }

        if (read_page(phys_lba, f2fs_page_buf) != 0) return -1;
        uint64_t chunk = F2FS_PAGE_SIZE - off_in_block;
        if (chunk > count - done) chunk = count - done;
        memcpy(f2fs_page_buf + off_in_block, in + done, chunk);

        /* LFS write: always write to new block location */
        uint32_t new_lba = alloc_next_data_block();
        if (!new_lba) return -1;
        if (write_page(new_lba, f2fs_page_buf) != 0) return -1;

        /* Update inode mapping */
        if (lblock < 28) inode.addr[lblock] = new_lba;

        done += chunk;
    }

    /* Update inode size */
    if (read_inode_by_nid(v->nid, &inode) != 0) return -1;
    uint64_t new_size = pos + count;
    inode.size = (uint32_t)new_size;
    if (new_size > 0xFFFFFFFF) inode.size = 0xFFFFFFFF;
    inode.mtime = 1337; /* pseudo time */
    if (write_inode_by_nid(v->nid, &inode) != 0) return -1;

    v->size = (uint32_t)new_size;
    v->vnode.size = new_size;
    v->dirty = 1;

    return (int64_t)done;
}

static int f2fs_truncate(struct vnode *vn, uint64_t new_size) {
    struct f2fs_vinfo *v = (struct f2fs_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(v->nid, &inode) != 0) return -1;
    inode.size = (uint32_t)new_size;
    inode.mtime = 1337;
    if (write_inode_by_nid(v->nid, &inode) != 0) return -1;
    v->size = (uint32_t)new_size;
    v->vnode.size = new_size;
    return 0;
}

/* ============================================================================
 * SECTION 11: PATH RESOLUTION
 * ============================================================================ */

static int path_resolve(const char *path, uint32_t *out_parent_nid,
                        uint32_t *out_target_nid, int *found,
                        char *basename_out, int bn_size) {
    *found = 0;
    if (!path) return -1;

    while (*path == '/') path++;
    if (!*path) {
        *out_parent_nid = 0;
        *out_target_nid = f2m.root_nid;
        *found = 1;
        if (basename_out && bn_size) basename_out[0] = 0;
        return 0;
    }

    uint32_t dir_nid = f2m.root_nid;
    char comp[F2FS_MAX_NAME];
    const char *p = path;

    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;

        uint32_t child_nid = dir_lookup(dir_nid, comp, n, NULL);
        if (!child_nid) return -1;

        if (*p == 0) {
            /* Final component */
            *out_parent_nid = dir_nid;
            *out_target_nid = child_nid;
            *found = 1;
            if (basename_out && bn_size) {
                strncpy(basename_out, comp, bn_size - 1);
                basename_out[bn_size - 1] = 0;
            }
            return 0;
        }

        dir_nid = child_nid;
    }
    return -1;
}

/* ============================================================================
 * SECTION 12: VNODE MANAGEMENT
 * ============================================================================ */

static struct f2fs_vinfo *fv_intern(const char *path, uint32_t nid,
                                    uint32_t ino, uint32_t parent_nid,
                                    int is_dir, uint32_t size) {
    for (int i = 0; i < F2FS_MAX_OPEN_VNODES; i++) {
        if (fv4pool[i].in_use && strcmp(fv4pool[i].path, path) == 0) {
            fv4pool[i].nid = nid;
            fv4pool[i].ino = ino;
            fv4pool[i].parent_nid = parent_nid;
            fv4pool[i].size = size;
            fv4pool[i].is_dir = is_dir;
            fv4pool[i].vnode.size = size;
            return &fv4pool[i];
        }
    }
    for (int i = 0; i < F2FS_MAX_OPEN_VNODES; i++) {
        if (!fv4pool[i].in_use) {
            struct f2fs_vinfo *v = &fv4pool[i];
            memset(v, 0, sizeof(*v));
            v->in_use = 1;
            strncpy(v->path, path, sizeof(v->path) - 1);
            v->nid = nid;
            v->ino = ino;
            v->parent_nid = parent_nid;
            v->size = size;
            v->is_dir = is_dir;
            strncpy(v->vnode.name, path, VFS_PATH_MAX - 1);
            v->vnode.type = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            v->vnode.mode = is_dir ? 0755 : 0644;
            v->vnode.size = size;
            v->vnode.ops = &f2fs_ops;
            v->vnode.fs_data = v;
            v->vnode.inode_id = ino;
            return v;
        }
    }
    return NULL;
}

static void fv_evict(const char *path) {
    for (int i = 0; i < F2FS_MAX_OPEN_VNODES; i++) {
        if (fv4pool[i].in_use && strcmp(fv4pool[i].path, path) == 0)
            fv4pool[i].in_use = 0;
    }
}

/* ============================================================================
 * SECTION 13: VFS OPERATIONS
 * ============================================================================ */

static struct vnode *f2fs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (!f2m.mounted) return NULL;

    uint32_t parent_nid, target_nid;
    int found;
    char base[F2FS_MAX_NAME] = {0};

    if (path_resolve(path, &parent_nid, &target_nid, &found, base, sizeof(base)) != 0)
        return NULL;
    if (!found) return NULL;

    struct f2fs_inode inode;
    if (read_inode_by_nid(target_nid, &inode) != 0) return NULL;

    int is_dir = (inode.mode & 0x4000) != 0;
    uint32_t size = inode.size;

    return &fv_intern(path, target_nid, target_nid, parent_nid, is_dir, size)->vnode;
}

static struct vnode *f2fs_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!f2m.mounted) return NULL;

    uint32_t parent_nid, target_nid;
    int found;
    char base[F2FS_MAX_NAME] = {0};

    if (path_resolve(path, &parent_nid, &target_nid, &found, base, sizeof(base)) != 0)
        return NULL;
    if (found) return NULL;
    if (!base[0]) return NULL;

    /* Allocate new inode (NID) */
    uint32_t new_nid = f2fs_alloc_nid();
    if (!new_nid) return NULL;

    struct f2fs_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = 0x8000 | 0644;   /* regular file, mode 0644 */
    inode.size = 0;
    inode.blocks = 0;
    inode.links = 1;
    inode.atime = 1337;
    inode.ctime = 1337;
    inode.mtime = 1337;
    inode.uid = 0;
    inode.gid = 0;

    if (write_inode_by_nid(new_nid, &inode) != 0) return NULL;

    kprintf("[f2fs] create: nid=%u path='%s' parent=%u\n",
            new_nid, base, parent_nid);

    /* Add directory entry */
    if (parent_nid != 0) {
        dir_add_entry(parent_nid, new_nid, base, strlen(base), F2FS_FT_REG_FILE);
    }

    return &fv_intern(path, new_nid, new_nid, parent_nid, 0, 0)->vnode;
}

static int f2fs_mkdir(void *fs_data, const char *path) {
    (void)fs_data;
    if (!f2m.mounted) return -1;

    uint32_t parent_nid, target_nid;
    int found;
    char base[F2FS_MAX_NAME] = {0};

    if (path_resolve(path, &parent_nid, &target_nid, &found, base, sizeof(base)) != 0)
        return -1;
    if (found) return -1;
    if (!base[0]) return -1;

    /* Allocate new directory inode */
    uint32_t new_nid = f2fs_alloc_nid();
    if (!new_nid) return -1;

    /* Allocate data block for directory entries */
    uint32_t dir_data_lba = alloc_next_data_block();
    if (!dir_data_lba) return -1;

    memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
    /* Create "." entry */
    struct f2fs_dir_entry *dot = (struct f2fs_dir_entry *)f2fs_page_buf;
    dot->ino = new_nid;
    dot->name_len = 1;
    dot->file_type = F2FS_FT_DIR;
    memcpy(dot->name, ".", 1);

    /* Create ".." entry */
    struct f2fs_dir_entry *dotdot =
        (struct f2fs_dir_entry *)(f2fs_page_buf + 12);
    dotdot->ino = parent_nid ? parent_nid : new_nid;
    dotdot->name_len = 2;
    dotdot->file_type = F2FS_FT_DIR;
    memcpy(dotdot->name, "..", 2);

    if (write_page(dir_data_lba, f2fs_page_buf) != 0) return -1;

    /* Initialize directory inode */
    struct f2fs_inode dinode;
    memset(&dinode, 0, sizeof(dinode));
    dinode.mode = 0x4000 | 0755;  /* directory, mode 0755 */
    dinode.size = F2FS_PAGE_SIZE;
    dinode.blocks = 1;
    dinode.links = 2;
    dinode.atime = 1337;
    dinode.ctime = 1337;
    dinode.mtime = 1337;
    dinode.uid = 0;
    dinode.gid = 0;
    dinode.addr[0] = dir_data_lba;

    if (write_inode_by_nid(new_nid, &dinode) != 0) return -1;

    kprintf("[f2fs] mkdir: nid=%u name='%s' parent=%u\n",
            new_nid, base, parent_nid);

    /* Add directory entry in parent */
    if (parent_nid != 0) {
        dir_add_entry(parent_nid, new_nid, base, strlen(base), F2FS_FT_DIR);
    }

    return 0;
}

static int f2fs_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    if (!f2m.mounted) return -1;

    uint32_t parent_nid, target_nid;
    int found;
    if (path_resolve(path, &parent_nid, &target_nid, &found, NULL, 0) != 0 || !found)
        return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(target_nid, &inode) != 0) return -1;
    if ((inode.mode & 0x4000) != 0) return -1; /* don't unlink dirs */

    inode.links = 0;
    inode.size = 0;
    if (write_inode_by_nid(target_nid, &inode) != 0) return -1;

    fv_evict(path);
    return 0;
}

static int f2fs_stat(struct vnode *vn, struct vfs_stat *st) {
    struct f2fs_vinfo *v = (struct f2fs_vinfo *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    if (!v) return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(v->nid, &inode) != 0) return -1;

    st->type = v->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->mode = inode.mode & 0xFFF;
    st->size = inode.size;
    st->inode = v->ino;
    st->nlink = inode.links;
    st->blocks = inode.blocks;
    return 0;
}

const struct vfs_ops f2fs_ops = {
    .lookup   = f2fs_lookup,
    .create   = f2fs_create,
    .read     = f2fs_read,
    .write    = f2fs_write,
    .readdir  = f2fs_readdir_op,
    .mkdir    = f2fs_mkdir,
    .unlink   = f2fs_unlink,
    .stat     = f2fs_stat,
    .truncate = f2fs_truncate,
};

/* ============================================================================
 * SECTION 14: FORMAT AND MOUNT
 * ============================================================================ */

static int format_f2fs(void) {
    kprintf("[f2fs] formatting F2FS volume...\n");

    f2m.page_size = F2FS_PAGE_SIZE;
    f2m.sector_size = F2FS_SECTOR_SIZE;
    f2m.blocks_per_seg = F2FS_BLOCKS_PER_SEG;
    f2m.seg_size = F2FS_SEG_SIZE;
    f2m.total_segments = 8192;  /* ~16 GB worth of 2MB segments */
    f2m.main_segments = f2m.total_segments - 4 - 4; /* leave room for CP/SSA */
    f2m.cp_segments = 2;
    f2m.ssa_segments = 2;
    f2m.start_main_lba = F2FS_MAIN_START_LBA;
    f2m.total_bytes = (uint64_t)f2m.main_segments * F2FS_SEG_SIZE;

    /* Superblock */
    memset(f2fs_scratch, 0, F2FS_PAGE_SIZE);
    struct f2fs_superblock *sb = (struct f2fs_superblock *)f2fs_scratch;
    sb->magic = F2FS_MAGIC;
    sb->major_ver = 0x20;
    sb->minor_ver = 0x22;
    sb->log_sector_size = 9;
    sb->log_blocksize = F2FS_LOG_PAGE_SIZE;
    sb->log_blocks_per_seg = 9;
    sb->segment_count = f2m.total_segments;
    sb->segment_count_ckpt = f2m.cp_segments;
    sb->segment_count_ssa = f2m.ssa_segments;
    sb->segment_count_main = f2m.main_segments;
    sb->start_segment_main = 4 + f2m.ssa_segments;
    sb->start_segment_ckpt = 0;
    sb->start_segment_ssa = f2m.cp_segments;
    sb->cp_blkaddr = F2FS_CP_LBA;
    sb->ssa_blkaddr = F2FS_CP_LBA + f2m.cp_segments * f2m.blocks_per_seg;
    sb->main_blkaddr = F2FS_MAIN_START_LBA;
    sb->root_ino = 1;
    sb->number_ino = F2FS_MAX_NIDS;
    sb->cur_data_blkaddr = f2m.start_main_lba;
    sb->cur_node_blkaddr = f2m.start_main_lba;
    sb->next_free_nid = 1;
    sb->free_segment_count = f2m.main_segments - 1;
    sb->total_valid_block_count = 0;
    sb->user_block_count = (f2m.main_segments - 2) * F2FS_BLOCKS_PER_SEG;
    sb->pages_per_seg = F2FS_BLOCKS_PER_SEG;
    sb->sector_size = F2FS_SECTOR_SIZE;
    sb->page_size = F2FS_PAGE_SIZE;
    sb->log_page_size = F2FS_LOG_PAGE_SIZE;
    sb->block_count = f2m.total_segments * F2FS_BLOCKS_PER_SEG;
    memcpy(sb->uuid, "F2FS00000000000", 15);
    memcpy(sb->volume_name, "AuraLite-F2FS", 14);

    if (write_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
    /* Backup superblock */
    if (write_page(F2FS_SUPER_BAK_LBA, f2fs_scratch) != 0) return -1;

    /* Checkpoint */
    memset(f2fs_scratch, 0, F2FS_PAGE_SIZE);
    struct f2fs_checkpoint *cp = (struct f2fs_checkpoint *)f2fs_scratch;
    cp->checkpoint_ver = 1;
    cp->user_block_count = sb->user_block_count;
    cp->valid_block_count = 0;
    cp->free_segment_count = f2m.main_segments - 1;
    cp->cur_node_seg[0] = f2m.start_main_lba / f2m.blocks_per_seg;
    cp->cur_node_blk[0] = 0;
    cp->cur_data_seg[0] = (f2m.start_main_lba + f2m.blocks_per_seg) / f2m.blocks_per_seg;
    cp->cur_data_blk[0] = 0;
    cp->next_free_nid = 1;
    cp->sit_ver = 1;

    if (write_page(F2FS_CP_LBA, f2fs_scratch) != 0) return -1;
    if (write_page(F2FS_CP_BAK_LBA, f2fs_scratch) != 0) return -1;

    /* Format root inode (NID = 1) */
    f2m.root_nid = 1;

    /* Create root directory */
    uint32_t root_data_lba = alloc_next_data_block();
    memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
    struct f2fs_dir_entry *dot = (struct f2fs_dir_entry *)f2fs_page_buf;
    dot->ino = 1;
    dot->name_len = 1;
    dot->file_type = F2FS_FT_DIR;
    memcpy(dot->name, ".", 1);
    struct f2fs_dir_entry *dotdot = (struct f2fs_dir_entry *)(f2fs_page_buf + 12);
    dotdot->ino = 1;
    dotdot->name_len = 2;
    dotdot->file_type = F2FS_FT_DIR;
    memcpy(dotdot->name, "..", 2);
    if (write_page(root_data_lba, f2fs_page_buf) != 0) return -1;

    memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
    struct f2fs_node_header *nh = (struct f2fs_node_header *)f2fs_page_buf;
    nh->nid = 1;
    nh->type = F2FS_NODE_INODE;
    nh->version = 1;

    struct f2fs_inode *root_inode =
        (struct f2fs_inode *)(f2fs_page_buf + sizeof(*nh));
    root_inode->mode = 0x4000 | 0755;  /* directory */
    root_inode->size = F2FS_PAGE_SIZE;
    root_inode->blocks = 1;
    root_inode->links = 2;
    root_inode->atime = 1337;
    root_inode->ctime = 1337;
    root_inode->mtime = 1337;
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->addr[0] = root_data_lba;

    uint32_t root_lba = nat_get_nid_addr(1);
    if (write_page(root_lba, f2fs_page_buf) != 0) return -1;

    /* Initialize current segment pointers */
    f2m.cur_node_seg = 2;
    f2m.cur_node_blk = 1;
    f2m.cur_data_seg = 3;
    f2m.cur_data_blk = 1;

    kprintf("[f2fs] format complete: %u segments, %llu bytes, root nid=1\n",
            f2m.total_segments, (unsigned long long)f2m.total_bytes);
    return 0;
}

int f2fs_init(int prefer_port) {
    memset(&f2m, 0, sizeof(f2m));
    memset(fv4pool, 0, sizeof(fv4pool));
    f2m.ahci_port = prefer_port;
    spinlock_init(&f2m.alloc_lock);

    if (!f2fs_scratch) f2fs_scratch = (uint8_t *)kmalloc(F2FS_PAGE_SIZE);
    if (!f2fs_page_buf) f2fs_page_buf = (uint8_t *)kmalloc(F2FS_PAGE_SIZE);

    /* Read superblock */
    if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) {
        kprintf("[f2fs] cannot read superblock, formatting...\n");
        if (format_f2fs() != 0) return -1;
        if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
    }

    struct f2fs_superblock *sb = (struct f2fs_superblock *)f2fs_scratch;
    if (sb->magic != F2FS_MAGIC) {
        kprintf("[f2fs] not F2FS magic (0x%08X), formatting...\n", sb->magic);
        if (format_f2fs() != 0) return -1;
        if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
        sb = (struct f2fs_superblock *)f2fs_scratch;
    }

    f2m.page_size = sb->page_size ? sb->page_size : F2FS_PAGE_SIZE;
    f2m.sector_size = sb->sector_size ? sb->sector_size : F2FS_SECTOR_SIZE;
    f2m.blocks_per_seg = (1 << sb->log_blocks_per_seg) ? (1 << sb->log_blocks_per_seg) : F2FS_BLOCKS_PER_SEG;
    f2m.seg_size = (uint32_t)f2m.blocks_per_seg * f2m.page_size;
    f2m.total_segments = sb->segment_count;
    f2m.main_segments = sb->segment_count_main;
    f2m.cp_segments = sb->segment_count_ckpt;
    f2m.ssa_segments = sb->segment_count_ssa;
    f2m.start_main_lba = sb->main_blkaddr;
    f2m.root_nid = sb->root_ino ? sb->root_ino : 1;
    f2m.next_free_nid = sb->next_free_nid ? sb->next_free_nid : 1;
    f2m.free_segments = (uint32_t)(sb->free_segment_count);
    f2m.total_bytes = (uint64_t)f2m.main_segments * f2m.seg_size;
    f2m.cur_node_seg = 2;
    f2m.cur_node_blk = 1;
    f2m.cur_data_seg = 3;
    f2m.cur_data_blk = 0;

    kprintf("[f2fs] mounted flash-friendly filesystem at /f2fs:\n");
    kprintf("       page=%uB, seg=%u (%u blocks), total=%llu bytes\n",
            f2m.page_size, f2m.seg_size, f2m.blocks_per_seg,
            (unsigned long long)f2m.total_bytes);
    kprintf("       segments: total=%u main=%u cp=%u ssa=%u\n",
            f2m.total_segments, f2m.main_segments, f2m.cp_segments, f2m.ssa_segments);
    kprintf("       LFS layout: main starts at LBA %u, cur_node=%u:%u cur_data=%u:%u\n",
            f2m.start_main_lba, f2m.cur_node_seg, f2m.cur_node_blk,
            f2m.cur_data_seg, f2m.cur_data_blk);

    f2m.mounted = 1;
    return 0;
}

/* ============================================================================
 * SECTION 15: SELF-TEST
 * ============================================================================ */

void f2fs_self_test(void) {
    if (!f2m.mounted) return;
    kprintf("[f2fs] self-test: create, write, read, mkdir...\n");

    /* Create a file */
    struct vnode *f = f2fs_create(NULL, "test_f2fs.dat");
    if (!f) { kprintf("[f2fs] FAIL: create\n"); return; }

    const char *msg = "F2FS log-structured write test OK!";
    if (f2fs_write(f, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[f2fs] FAIL: write\n"); return;
    }

    char buf[64] = {0};
    if (f2fs_read(f, 0, buf, sizeof(buf)-1) != (int64_t)strlen(msg) ||
        strcmp(buf, msg) != 0) {
        kprintf("[f2fs] FAIL: readback '%s'\n", buf); return;
    }

    /* Create a directory */
    if (f2fs_mkdir(NULL, "flashdir") != 0) {
        kprintf("[f2fs] FAIL: mkdir\n"); return;
    }

    kprintf("[f2fs] PASS: F2FS (flash-friendly) filesystem functional\n");
}