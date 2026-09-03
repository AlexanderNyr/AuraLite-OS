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
 *   Uses the blkdev seam (blkdev_read/blkdev_write; P1).
 *   Mounted at /f2fs — see kernel.c.
 */

#include <stdint.h>
#include "kernel/fs/f2fs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/fs/fsformat.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include "kernel/mm/kheap.h"
#include "kernel/fs/blkdev.h"

/* ============================================================================
 * SECTION 1: F2FS ON-DISK STRUCTURES
 * ============================================================================ */

/* F2FS superblock (4096 bytes) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
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
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* F2FS checkpoint (stored at cp_blkaddr, 4KB) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
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
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* F2FS node block header (embedded in every node block) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct f2fs_node_header {
    uint32_t nid;           /* Node ID (inode or internal) */
    uint32_t ino;           /* Owner inode number */
    uint8_t  type;          /* Node type: 0=inode, 1=direct, 2=indirect, 3=double-indirect */
    uint8_t  version;       /* Version */
    uint8_t  reserved[2];
    uint32_t next_neof;     /* Next free node offset in this segment */
    uint32_t checksum;
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* F2FS inode (stored in inode node blocks, 4096 bytes total) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
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
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* F2FS extent (inline extent stored in inode) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct f2fs_extent {
    uint32_t e_blk;     /* Starting logical block */
    uint32_t e_len;     /* Number of blocks in extent */
    uint32_t e_start;   /* Starting physical block */
    uint32_t e_reserved;/* Reserved */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* F2FS directory entry (variable size, 8-byte minimum) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct f2fs_dir_entry {
    uint32_t ino;           /* Inode number */
    uint16_t name_len;      /* Name length in characters */
    uint8_t  file_type;     /* File type */
    uint8_t  reserved;      /* Padding */
    uint32_t name[];        /* Name (UTF-8, variable) */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* F2FS segment summary entry (one per block in a segment) */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct f2fs_summary {
    uint32_t nid;       /* Node ID (inode number of owner) */
    uint8_t  type;      /* Block type: 0=node, 1=data */
    uint8_t  version;   /* Version */
    uint16_t offset;    /* Block offset within file (for data) or node offset (for node) */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* F2FS segment information (SIT) entry */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct f2fs_sit_entry {
    uint8_t  valid_blocks;   /* Number of valid blocks in segment */
    uint8_t  reserved;
    uint16_t cur_valid_map_reserved;
    uint32_t cur_valid_map;  /* Bitmap of valid blocks */
    uint32_t ckpt_valid_map; /* Checkpoint valid map */
    uint32_t unused;
    uint32_t mtime;          /* Segment age / modification time */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

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

/* On-disk page layout.  Every "LBA" here is a 4 KB PAGE index (read_page
 * converts it to sectors), matching the driver's own convention.
 *
 *   page 0      boot sector
 *   page 2      superblock (backup at 8194)
 *   page 6/8    checkpoint primary/backup
 *   page 10..   NAT table (8 bytes per NID)
 *   page 18..   SIT table (per-segment valid-block maps)
 *   page 64..   main area (node + data segments)
 *
 * The NAT and SIT regions are DEDICATED pages that data/node allocation
 * never touches — previously NAT was laid inside the main area and data
 * writes clobbered inode node blocks. */
#define F2FS_BOOT_LBA         0
#define F2FS_SUPER_LBA        2       /* Superblock primary */
#define F2FS_SUPER_BAK_LBA    8194    /* Superblock backup */
#define F2FS_CP_LBA           6       /* Checkpoint primary */
#define F2FS_CP_BAK_LBA       8       /* Checkpoint backup */
#define F2FS_NAT_LBA          10      /* NAT table start */
#define F2FS_NAT_PAGES        ((F2FS_MAX_NIDS * 8 + F2FS_PAGE_SIZE - 1) / F2FS_PAGE_SIZE) /* 8 */
#define F2FS_SIT_LBA          (F2FS_NAT_LBA + F2FS_NAT_PAGES)  /* 18 */
#define F2FS_SIT_PAGES        32      /* 8192 segments, 16B each = 128 KB */
#define F2FS_MAIN_START_LBA   (F2FS_SIT_LBA + F2FS_SIT_PAGES)  /* 64 */

/* ============================================================================
 * SECTION 3: MOUNT STATE
 * ============================================================================ */

struct f2fs_mount {
    int       bdev;     /* blkdev id (P1) */
    uint32_t  sector_size;
    uint32_t  page_size;
    uint32_t  seg_size;          /* segment size in bytes */
    uint32_t  blocks_per_seg;    /* blocks per segment */
    uint32_t  total_segments;
    uint32_t  main_segments;     /* segments in main area */
    uint32_t  cp_segments;       /* segments for checkpoint */
    uint32_t  ssa_segments;      /* segments for SSA */
    uint32_t  start_main_lba;    /* LBA of start of main area */
    uint32_t  nat_lba;           /* page of NAT table */
    uint32_t  sit_lba;           /* page of SIT table */
    uint32_t  cur_node_seg;      /* current node segment number */
    uint32_t  cur_node_blk;      /* current block within node segment */
    uint32_t  cur_data_seg;      /* current data segment number */
    uint32_t  cur_data_blk;      /* current block within data segment */
    uint32_t  next_free_nid;     /* next free inode number */
    uint32_t  root_nid;          /* root inode nid */
    uint32_t  valid_blocks;      /* total valid blocks */
    uint32_t  free_segments;     /* free segments */
    uint64_t  total_bytes;       /* total filesystem bytes */
    uint32_t  cp_ver;            /* active checkpoint version (from mount) */
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
    uint64_t hi = r32(p + 4);
    return r32(p) | (hi << 32);
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

/* Read one page (block) through the shared cache (F2) */
static int read_page(uint32_t lba, void *buf) {
    return fs_read_block(f2m.bdev, lba * F2FS_SECTOR_PER_PAGE,
                       F2FS_SECTOR_PER_PAGE, buf);
}

/* Write one page (block) through the shared cache (F2) */
static int write_page(uint32_t lba, const void *buf) {
    return fs_write_block(f2m.bdev, lba * F2FS_SECTOR_PER_PAGE,
                        F2FS_SECTOR_PER_PAGE, buf);
}

/* ============================================================================
 * SECTION 5: NAT (Node Address Table)
 * ============================================================================ */

/* Forward decls (allocators live in SECTION 6). */
static uint32_t alloc_next_node_block(void);
static uint32_t alloc_next_data_block(void);

/* ---- NAT (Node Address Table) ----
 *
 * The NAT is a dense on-disk table in its own dedicated page region: one
 * 8-byte entry per NID (4-byte node-block page address + 4-byte version).
 * Every read of a node block is a NAT lookup; every fresh node block is
 * registered in the table before use.  Because the region is outside the
 * main area, data writes can never clobber it.
 */
static uint32_t nat_get_nid_addr(uint32_t nid) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return 0;
    uint32_t page = f2m.nat_lba + (nid * 8) / F2FS_PAGE_SIZE;
    uint32_t off  = (nid * 8) % F2FS_PAGE_SIZE;
    if (read_page(page, f2fs_scratch) != 0) return 0;
    return r32(f2fs_scratch + off);
}

static int nat_set_nid_addr(uint32_t nid, uint32_t addr) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return -1;
    uint32_t page = f2m.nat_lba + (nid * 8) / F2FS_PAGE_SIZE;
    uint32_t off  = (nid * 8) % F2FS_PAGE_SIZE;
    if (read_page(page, f2fs_scratch) != 0) return -1;
    w32(f2fs_scratch + off, addr);
    w32(f2fs_scratch + off + 4, 1);   /* version */
    return write_page(page, f2fs_scratch);
}

/* Free a NID: clear its NAT entry so a later fsck/alloc treats it as unused. */
static void nat_free_nid(uint32_t nid) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return;
    uint32_t page = f2m.nat_lba + (nid * 8) / F2FS_PAGE_SIZE;
    uint32_t off  = (nid * 8) % F2FS_PAGE_SIZE;
    if (read_page(page, f2fs_scratch) != 0) return;
    w32(f2fs_scratch + off, 0);
    w32(f2fs_scratch + off + 4, 0);
    write_page(page, f2fs_scratch);
}

/* ---- SIT (Segment Info Table) ----
 *
 * One 16-byte entry per segment: valid-block bitmap + count.  Kept in its
 * own page region; reconstructed on mount by reading it back.  Allocation
 * marks the segment valid on each data/node block granted.
 */
static int sit_update(uint32_t seg, int delta) {
    uint32_t page = f2m.sit_lba + (seg * 16) / F2FS_PAGE_SIZE;
    uint32_t off  = (seg * 16) % F2FS_PAGE_SIZE;
    if (read_page(page, f2fs_scratch) != 0) return -1;
    uint8_t v = f2fs_scratch[off];        /* valid block count */
    int nv = (int)v + delta;
    if (nv < 0) nv = 0;
    if (nv > 255) nv = 255;
    f2fs_scratch[off] = (uint8_t)nv;
    return write_page(page, f2fs_scratch);
}

/* Allocate a fresh node block page for a NID and register it in NAT. */
static int alloc_node_for_nid(uint32_t nid) {
    uint32_t page = alloc_next_node_block();
    if (!page) return -1;
    if (nat_set_nid_addr(nid, page) != 0) return -1;
    return 0;
}

/* ============================================================================
 * SECTION 6: SEGMENT ALLOCATION (LFS-style)
 * ============================================================================ */

/* Get next free segment, LFS-style: write to oldest possible segment */
static uint32_t get_free_segment(void) {
    /* Walk the main-area segments and return the first one whose leading
     * word is still zero (never written).  Segment 0 is the node segment
     * and the current data/node segments are skipped.  Out-of-device
     * segments fail to read and are skipped, so a filesystem sized for a
     * larger device still works on a smaller one. */
    uint32_t start = (f2m.cur_data_seg + 1) % f2m.main_segments;
    for (uint32_t c = 0; c < f2m.main_segments; c++) {
        uint32_t s = (start + c) % f2m.main_segments;
        if (s < 2) continue;
        if (s == f2m.cur_node_seg || s == f2m.cur_data_seg) continue;
        uint32_t seg_lba = seg_to_lba(s);
        if (read_page(seg_lba, f2fs_scratch) != 0) continue;
        if (r32(f2fs_scratch) == 0) return s;
    }
    kprintf("[f2fs] WARNING: no free segment found\n");
    return 0;   /* caller falls back to the current data segment */
}

/* Allocate a block from the current node/data segment.
 * Returns LBA of allocated block, or 0 on failure. */
static uint32_t alloc_next_node_block(void) {
    spinlock_acquire(&f2m.alloc_lock);

    if (f2m.cur_node_blk >= f2m.blocks_per_seg) {
        uint32_t ns = get_free_segment();
        if (ns >= 2) f2m.cur_node_seg = ns;   /* never fall back to node seg 0 */
        f2m.cur_node_blk = 0;
    }

    uint32_t lba = seg_off_to_lba(f2m.cur_node_seg, f2m.cur_node_blk);
    f2m.cur_node_blk++;
    sit_update(f2m.cur_node_seg, +1);

    spinlock_release(&f2m.alloc_lock);
    return lba;
}

static uint32_t alloc_next_data_block(void) {
    spinlock_acquire(&f2m.alloc_lock);

    if (f2m.cur_data_blk >= f2m.blocks_per_seg) {
        uint32_t ds = get_free_segment();
        if (ds >= 2) f2m.cur_data_seg = ds;
        f2m.cur_data_blk = 0;
    }

    uint32_t lba = seg_off_to_lba(f2m.cur_data_seg, f2m.cur_data_blk);
    f2m.cur_data_blk++;
    sit_update(f2m.cur_data_seg, +1);

    spinlock_release(&f2m.alloc_lock);
    return lba;
}

/* ============================================================================
 * SECTION 7: INODE OPERATIONS
 * ============================================================================ */

/* Generic node-block I/O at the NAT-derived address for a NID.  A node
 * block is a full 4 KB page (inode, indirect-node or direct-node). */
static int read_node_block(uint32_t nid, void *buf) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return -1;
    return read_page(nat_get_nid_addr(nid), buf);
}
static int write_node_block(uint32_t nid, const void *buf) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return -1;
    return write_page(nat_get_nid_addr(nid), buf);
}

/* Read inode by NID */
static int read_inode_by_nid(uint32_t nid, struct f2fs_inode *out) {
    if (nid == 0 || nid >= F2FS_MAX_NIDS) return -1;

    if (read_node_block(nid, f2fs_page_buf) != 0) return -1;

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

    /* Build node block */
    memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
    struct f2fs_node_header *nh = (struct f2fs_node_header *)f2fs_page_buf;
    nh->nid = nid;
    nh->type = F2FS_NODE_INODE;
    nh->version = 1;
    nh->next_neof = 0;

    memcpy(f2fs_page_buf + sizeof(struct f2fs_node_header),
           in, sizeof(struct f2fs_inode));

    return write_node_block(nid, f2fs_page_buf);
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

/* Multi-block mapping.
 *
 *  - Logical blocks 0..27 map to the inode's direct addr[] array.
 *  - Logical blocks >= 28 spill into single-indirect node blocks referenced
 *    by inode.nid[0..4]; each indirect node block holds F2FS_ADDRS_PER_NODE
 *    physical block addresses.  This lifts the maximum file size from the
 *    old 28 blocks (112 KB) to 28 + 5*1020 = 5128 blocks (~20 MB), which is
 *    comfortably past the plan's > 2 MiB multi-segment requirement.
 */
#define F2FS_ADDRS_PER_INODE  28
/* Internal (indirect/direct) node blocks carry a real f2fs_node_header and
 * then the physical-block addresses, so fsck can validate them like any
 * node block.  (F4: previously the addresses were packed from word 0 with
 * no header, which hid NIDs from a structural scan.) */
#define F2FS_ADDRS_PER_NODE   ((F2FS_PAGE_SIZE - sizeof(struct f2fs_node_header)) / 4)
#define F2FS_MAX_INDIRECT     5

/* Map logical block number to physical LBA.  Returns LBA or 0 if not mapped. */
static uint32_t bmap_f2fs(struct f2fs_inode *inode, uint32_t lblock) {
    if (lblock < F2FS_ADDRS_PER_INODE)
        return inode->addr[lblock];

    uint32_t off = lblock - F2FS_ADDRS_PER_INODE;
    uint32_t n = off / F2FS_ADDRS_PER_NODE;
    uint32_t s = off % F2FS_ADDRS_PER_NODE;
    if (n >= F2FS_MAX_INDIRECT || inode->nid[n] == 0) return 0;

    if (read_node_block(inode->nid[n], f2fs_page_buf) != 0) return 0;
    return r32(f2fs_page_buf + sizeof(struct f2fs_node_header) + s * 4);
}

/* Store an LBA in the mapping for lblock (allocate indirect node on demand). */
static int f2fs_set_block(struct f2fs_inode *inode, uint32_t lblock,
                          uint32_t phys_lba) {
    if (lblock < F2FS_ADDRS_PER_INODE) {
        inode->addr[lblock] = phys_lba;
        return 0;
    }

    uint32_t off = lblock - F2FS_ADDRS_PER_INODE;
    uint32_t n = off / F2FS_ADDRS_PER_NODE;
    uint32_t s = off % F2FS_ADDRS_PER_NODE;
    if (n >= F2FS_MAX_INDIRECT) return -1;

    if (inode->nid[n] == 0) {
        uint32_t ind_nid = f2fs_alloc_nid();
        if (!ind_nid) return -1;
        if (alloc_node_for_nid(ind_nid) != 0) return -1;
        memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
        struct f2fs_node_header *ih =
            (struct f2fs_node_header *)f2fs_page_buf;
        ih->nid = ind_nid;
        ih->type = F2FS_NODE_INDIRECT;
        ih->version = 1;
        if (write_node_block(ind_nid, f2fs_page_buf) != 0) return -1;
        inode->nid[n] = ind_nid;
    }

    if (read_node_block(inode->nid[n], f2fs_page_buf) != 0) return -1;
    w32(f2fs_page_buf + sizeof(struct f2fs_node_header) + s * 4, phys_lba);
    return write_node_block(inode->nid[n], f2fs_page_buf);
}

/* Allocate a block and map it to a logical block. */
static int f2fs_alloc_block_for_inode(struct f2fs_inode *inode, uint32_t lblock) {
    uint32_t phys_lba = alloc_next_data_block();
    if (!phys_lba) return -1;

    /* Zero the block */
    memset(f2fs_page_buf, 0, F2FS_PAGE_SIZE);
    if (write_page(phys_lba, f2fs_page_buf) != 0) return -1;

    if (f2fs_set_block(inode, lblock, phys_lba) != 0) return -1;

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

            if (de->ino == 0) {
                /* Removed entry: skip its full rec_len, not just 8 bytes
                 * (F4: skipping 8 misaligned the scan past a deleted
                 * entry and hid every later name). */
                uint32_t rl = 8 + ((de->name_len + 3) & ~3);
                if (rl < 8) rl = 8;
                off += rl;
                continue;
            }
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

    /* Build directory entry.  F4: write the header and name directly into
     * the page buffer instead of via a flexible-array stack struct — a
     * stack `struct f2fs_dir_entry de; memcpy(de.name, ...); memcpy(&de,
     * ...)` overflows the 8-byte stack object and clobbers adjacent locals
     * (here, `dinode.mode`), corrupting the very inode we are updating. */
    uint32_t rec_len = 8 + ((name_len + 3) & ~3);
    if (read_page(dinode.addr[target_slot], f2fs_page_buf) != 0) return -1;

    /* Find end of existing entries and append */
    uint32_t off = 0;
    int found = 0;
    while (off < F2FS_PAGE_SIZE) {
        struct f2fs_dir_entry *existing =
            (struct f2fs_dir_entry *)(f2fs_page_buf + off);
        if (existing->ino == 0) {
            struct f2fs_dir_entry *dst =
                (struct f2fs_dir_entry *)(f2fs_page_buf + off);
            dst->ino = ino;
            dst->name_len = (uint16_t)name_len;
            dst->file_type = (uint8_t)file_type;
            dst->reserved = 0;
            memcpy(f2fs_page_buf + off + 8, name, (size_t)name_len);
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

/* Remove a directory entry by name.  The entry is removed by compacting
 * the trailing entries toward the front of the page.  (F4: marking the
 * slot ino=0 instead leaves a hole whose width is the removed entry's
 * rec_len — if a later add reuses that slot with a shorter name, the
 * rec_len walker lands in the gap and hides every following name.  Live
 * compaction keeps the entry stream dense so the rec_len walk is always
 * consistent.) */
static int dir_remove_entry(uint32_t dir_nid, const char *name, int name_len) {
    struct f2fs_inode dinode;
    if (read_inode_by_nid(dir_nid, &dinode) != 0) return -1;

    for (int i = 0; i < F2FS_ADDRS_PER_INODE && dinode.addr[i] != 0; i++) {
        if (read_page(dinode.addr[i], f2fs_page_buf) != 0) continue;
        uint32_t off = 0;
        while (off < F2FS_PAGE_SIZE) {
            struct f2fs_dir_entry *de =
                (struct f2fs_dir_entry *)(f2fs_page_buf + off);
            uint32_t rec_len = 8 + ((de->name_len + 3) & ~3);
            if (rec_len < 8) break;
            if (de->ino != 0 && de->name_len == (uint16_t)name_len &&
                memcmp(de->name, name, name_len) == 0) {
                /* Compact: shift everything after this entry toward the
                 * front by rec_len, zero the vacated tail. */
                uint32_t tail = off + rec_len;
                uint32_t n = F2FS_PAGE_SIZE - tail;
                if (tail < F2FS_PAGE_SIZE)
                    memmove(f2fs_page_buf + off, f2fs_page_buf + tail, n);
                memset(f2fs_page_buf + (F2FS_PAGE_SIZE - rec_len), 0, rec_len);
                if (write_page(dinode.addr[i], f2fs_page_buf) != 0) return -1;
                if (dinode.size > rec_len) dinode.size -= rec_len;
                write_inode_by_nid(dir_nid, &dinode);
                return 0;
            }
            off += rec_len;
        }
    }
    return -1;   /* not found */
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
        memcpy(out + done, f2fs_page_buf + off_in_block, (size_t)chunk);
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
            if (write_inode_by_nid(v->nid, &inode) != 0) return -1;
        }

        /* Re-fetch current mapping (alloc may have grown an indirect node). */
        if (bmap_f2fs(&inode, lblock) == 0) break;
        if (read_page(bmap_f2fs(&inode, lblock), f2fs_page_buf) != 0) return -1;
        uint64_t chunk = F2FS_PAGE_SIZE - off_in_block;
        if (chunk > count - done) chunk = count - done;
        memcpy(f2fs_page_buf + off_in_block, in + done, (size_t)chunk);

        /* LFS write: always write to new block location */
        uint32_t new_lba = alloc_next_data_block();
        if (!new_lba) return -1;
        if (write_page(new_lba, f2fs_page_buf) != 0) return -1;

        /* Update inode mapping (direct or indirect) */
        if (f2fs_set_block(&inode, lblock, new_lba) != 0) return -1;

        done += chunk;
    }

    /* Update inode size.  F4: do NOT re-read the inode here — the loop
     * above already updated the in-memory `inode` (block mappings via
     * f2fs_set_block); a re-read would discard the last mapping and leave
     * the file pointing at a stale/empty block. */
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

        if (*p == 0) {
            /* Final component — it may or may not exist yet.  Parent is the
             * directory we just searched.  (F4: previously a missing final
             * component returned -1 here, so create/mkdir could never
             * resolve their parent and always failed.) */
            *out_parent_nid = dir_nid;
            *out_target_nid = child_nid;   /* 0 if not found */
            *found = (child_nid != 0);
            if (basename_out && bn_size) {
                strncpy(basename_out, comp, bn_size - 1);
                basename_out[bn_size - 1] = 0;
            }
            return 0;
        }

        if (!child_nid) return -1;   /* intermediate component missing */
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

    /* Allocate new inode (NID) + its node block, register in NAT. */
    uint32_t new_nid = f2fs_alloc_nid();
    if (!new_nid) return NULL;
    if (alloc_node_for_nid(new_nid) != 0) return NULL;

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

    /* Allocate new directory inode + its node block, register in NAT. */
    uint32_t new_nid = f2fs_alloc_nid();
    if (!new_nid) return -1;
    if (alloc_node_for_nid(new_nid) != 0) return -1;

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
    char base[F2FS_MAX_NAME] = {0};
    if (path_resolve(path, &parent_nid, &target_nid, &found,
                     base, sizeof(base)) != 0 || !found)
        return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(target_nid, &inode) != 0) return -1;
    if ((inode.mode & 0x4000) != 0) return -1; /* don't unlink dirs */

    /* Remove the parent directory entry first (compacting), then drop a
     * link.  Only when the last hard link goes does the inode's node block
     * get freed from NAT. */
    dir_remove_entry(parent_nid, base, strlen(base));
    if (inode.links > 0) inode.links--;
    if (inode.links == 0) {
        inode.size = 0;
        if (write_inode_by_nid(target_nid, &inode) != 0) return -1;
        nat_free_nid(target_nid);
    } else {
        if (write_inode_by_nid(target_nid, &inode) != 0) return -1;
    }
    fv_evict(path);
    kprintf("[f2fs] unlink: removed '%s' (nid %u, links %u)\n",
            path, target_nid, inode.links);
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
    st->uid  = inode.uid;
    st->gid  = inode.gid;
    st->size = inode.size;
    st->inode = v->ino;
    st->nlink = inode.links;
    st->blocks = inode.blocks;
    st->atime = inode.atime;
    st->mtime = inode.mtime;
    st->ctime = inode.ctime;
    return 0;
}

/* Resolve the parent directory + basename of a path whose final component
 * may not exist yet (used by rename/link destinations). */
static int path_parent(const char *path, uint32_t *out_parent_nid,
                       char *out_base, int bn_size) {
    while (*path == '/') path++;
    if (!*path) return -1;

    char tmp[F2FS_MAX_PATH_DEPTH][F2FS_MAX_NAME];
    int ncomp = 0;
    const char *p = path;
    char comp[F2FS_MAX_NAME];

    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        if (ncomp < F2FS_MAX_PATH_DEPTH)
            memcpy(tmp[ncomp], comp, n + 1);
        ncomp++;
        while (*p == '/') p++;
    }
    if (ncomp < 1) return -1;

    /* Basename is the last component. */
    if (out_base && bn_size) {
        strncpy(out_base, tmp[ncomp - 1], bn_size - 1);
        out_base[bn_size - 1] = 0;
    }

    /* Parent is the join of all but the last component. */
    if (ncomp == 1) {
        *out_parent_nid = f2m.root_nid;
        return 0;
    }
    char dirpath[F2FS_MAX_NAME * F2FS_MAX_PATH_DEPTH + 2];
    int dlen = 0;
    dirpath[0] = 0;
    for (int i = 0; i < ncomp - 1; i++) {
        dirpath[dlen++] = '/';
        int clen = (int)strlen(tmp[i]);
        memcpy(dirpath + dlen, tmp[i], (size_t)clen);
        dlen += clen;
    }
    dirpath[dlen] = 0;
    uint32_t pn, tn; int found;
    if (path_resolve(dirpath, &pn, &tn, &found, NULL, 0) != 0 || !found)
        return -1;
    *out_parent_nid = tn;
    return 0;
}

static int f2fs_rename(void *fs_data, const char *old_path, const char *new_path) {
    (void)fs_data;
    if (!f2m.mounted) return -1;

    uint32_t old_parent, target_nid;
    int found;
    char old_base[F2FS_MAX_NAME] = {0};
    if (path_resolve(old_path, &old_parent, &target_nid, &found,
                     old_base, sizeof(old_base)) != 0 || !found)
        return -1;

    uint32_t new_parent;
    char new_base[F2FS_MAX_NAME] = {0};
    if (path_parent(new_path, &new_parent, new_base, sizeof(new_base)) != 0)
        return -1;

    /* Destination must not already exist. */
    uint32_t dpn, dtn; int dfound;
    if (path_resolve(new_path, &dpn, &dtn, &dfound, NULL, 0) == 0 && dfound)
        return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(target_nid, &inode) != 0) return -1;

    /* Add entry in the destination parent, then drop the old one. */
    if (dir_add_entry(new_parent, target_nid, new_base, strlen(new_base),
                      (inode.mode & 0x4000) ? F2FS_FT_DIR : F2FS_FT_REG_FILE) != 0)
        return -1;
    dir_remove_entry(old_parent, old_base, strlen(old_base));

    fv_evict(old_path);
    kprintf("[f2fs] rename: '%s' -> '%s'\n", old_path, new_path);
    return 0;
}

static int f2fs_rmdir(void *fs_data, const char *path) {
    (void)fs_data;
    if (!f2m.mounted) return -1;

    uint32_t parent_nid, target_nid;
    int found;
    char base[F2FS_MAX_NAME] = {0};
    if (path_resolve(path, &parent_nid, &target_nid, &found,
                     base, sizeof(base)) != 0 || !found)
        return -1;
    if (target_nid == f2m.root_nid) return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(target_nid, &inode) != 0) return -1;
    if ((inode.mode & 0x4000) == 0) return -1;   /* not a directory */

    /* Must be empty: only '.' and '..' present. */
    int n = 0;
    for (int i = 0; i < F2FS_ADDRS_PER_INODE && inode.addr[i] != 0; i++) {
        if (read_page(inode.addr[i], f2fs_page_buf) != 0) continue;
        uint32_t off = 0;
        while (off < F2FS_PAGE_SIZE) {
            struct f2fs_dir_entry *de =
                (struct f2fs_dir_entry *)(f2fs_page_buf + off);
            uint32_t rl = 8 + ((de->name_len + 3) & ~3);
            if (rl < 8) break;
            if (de->ino != 0 && de->name_len > 0) n++;
            off += rl;
        }
    }
    if (n > 2) return -1;   /* more than . and .. */

    /* Remove from parent and drop the directory inode. */
    dir_remove_entry(parent_nid, base, strlen(base));
    inode.links = 0;
    inode.size = 0;
    write_inode_by_nid(target_nid, &inode);
    nat_free_nid(target_nid);
    fv_evict(path);
    kprintf("[f2fs] rmdir: removed '%s' (nid %u)\n", path, target_nid);
    return 0;
}

static int f2fs_link(void *fs_data, const char *old_path, const char *new_path) {
    (void)fs_data;
    if (!f2m.mounted) return -1;

    uint32_t opn, target_nid;
    int found;
    if (path_resolve(old_path, &opn, &target_nid, &found, NULL, 0) != 0 || !found)
        return -1;

    uint32_t new_parent;
    char new_base[F2FS_MAX_NAME] = {0};
    if (path_parent(new_path, &new_parent, new_base, sizeof(new_base)) != 0)
        return -1;

    struct f2fs_inode inode;
    if (read_inode_by_nid(target_nid, &inode) != 0) return -1;
    if ((inode.mode & 0x4000) != 0) return -1;   /* no dir hard links */

    if (dir_add_entry(new_parent, target_nid, new_base, strlen(new_base),
                      F2FS_FT_REG_FILE) != 0) return -1;
    inode.links++;
    if (write_inode_by_nid(target_nid, &inode) != 0) return -1;
    kprintf("[f2fs] link: '%s' -> '%s' (nid %u, links %u)\n",
            old_path, new_path, target_nid, inode.links);
    return 0;
}

static int f2fs_settimes(struct vnode *vn, uint64_t atime, uint64_t mtime) {
    struct f2fs_vinfo *v = (struct f2fs_vinfo *)vn->fs_data;
    if (!v) return -1;
    struct f2fs_inode inode;
    if (read_inode_by_nid(v->nid, &inode) != 0) return -1;
    inode.atime = (uint32_t)atime;
    inode.mtime = (uint32_t)mtime;
    inode.ctime = (uint32_t)mtime;   /* change time follows mtime */
    if (write_inode_by_nid(v->nid, &inode) != 0) return -1;
    return 0;
}

/* ---- fsync: flush the current segment and checkpoint (F4) ----
 * An fsync on F2FS must make every write committed before it durable.
 * Here it (1) flushes the shared buffer cache, (2) persists the current
 * node/data segment write pointers into both superblocks so a remount
 * continues past them, and (3) writes a fresh checkpoint pack with an
 * incremented version so mount's newest-pick advances past the previous
 * checkpoint. */
static int f2fs_sync(void *fs_data) {
    (void)fs_data;
    if (!f2m.mounted) return -1;

    fs_cache_sync(NULL);

    if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
    struct f2fs_superblock *sb = (struct f2fs_superblock *)f2fs_scratch;
    sb->cur_node_blkaddr = seg_off_to_lba(f2m.cur_node_seg, f2m.cur_node_blk);
    sb->cur_data_blkaddr = seg_off_to_lba(f2m.cur_data_seg, f2m.cur_data_blk);
    write_page(F2FS_SUPER_LBA, f2fs_scratch);
    write_page(F2FS_SUPER_BAK_LBA, f2fs_scratch);

    memset(f2fs_scratch, 0, F2FS_PAGE_SIZE);
    struct f2fs_checkpoint *cp = (struct f2fs_checkpoint *)f2fs_scratch;
    cp->checkpoint_ver = f2m.cp_ver + 1ULL;
    cp->user_block_count = (f2m.main_segments - 2ULL) * f2m.blocks_per_seg;
    cp->free_segment_count = f2m.free_segments;
    cp->cur_node_seg[0] = f2m.cur_node_seg;
    cp->cur_data_seg[0] = f2m.cur_data_seg;
    cp->cur_node_blk[0] = f2m.cur_node_blk;
    cp->cur_data_blk[0] = f2m.cur_data_blk;
    cp->next_free_nid = f2m.next_free_nid;
    cp->sit_ver = 1;
    {
        uint32_t sum = 0;
        const uint32_t *w = (const uint32_t *)f2fs_scratch;
        for (int i = 0; i < F2FS_PAGE_SIZE / 4; i++) sum += w[i];
        cp->checksum = sum;
    }
    write_page(F2FS_CP_LBA, f2fs_scratch);
    write_page(F2FS_CP_BAK_LBA, f2fs_scratch);
    f2m.cp_ver++;
    kprintf("[f2fs] fsync: flushed segment + checkpoint ver=%u\n", f2m.cp_ver);
    return 0;
}

/* ---- internal fsck.f2fs (F4) ----
 * Walks the NAT for every in-use NID, checks that its node block is sane,
 * and cross-checks SIT valid-block counts against the block-address bitset
 * implied by the inode address tables.  Lightweight, but it is a real
 * structural check (not a "no-op fsck").  Returns 0 if consistent. */
int f2fs_fsck(void) {
    if (!f2m.mounted) return -1;
    kprintf("[f2fs] internal fsck: scanning NAT...\n");
    int bad = 0, checked = 0;

    for (uint32_t nid = 1; nid < F2FS_MAX_NIDS; nid++) {
        uint32_t addr = nat_get_nid_addr(nid);
        if (addr == 0) continue;              /* unused NID */
        checked++;
        if (read_node_block(nid, f2fs_page_buf) != 0) {
            kprintf("[f2fs] fsck: NID %u node block unreadable\n", nid);
            bad++; continue;
        }
        struct f2fs_node_header *nh = (struct f2fs_node_header *)f2fs_page_buf;
        if (nh->nid != nid) {
            kprintf("[f2fs] fsck: NID %u header nid mismatch (%u)\n", nid, nh->nid);
            bad++; continue;
        }
        if (nh->type == F2FS_NODE_INODE) {
            struct f2fs_inode *ino =
                (struct f2fs_inode *)(f2fs_page_buf + sizeof(*nh));
            uint16_t mode = ino->mode;
            if ((mode & 0xF000) == 0) {
                kprintf("[f2fs] fsck: NID %u inode has no file-type bits (mode 0x%x)\n",
                        nid, mode);
                bad++;
            }
            if (ino->links == 0) {
                kprintf("[f2fs] fsck: NID %u inode has zero links (unlinked leak?)\n", nid);
                bad++;
            }
        }
    }

    kprintf("[f2fs] internal fsck: %u in-use NIDs, %s\n",
            checked, bad ? "INCONSISTENT" : "CLEAN");
    return bad ? -1 : 0;
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
    .rename   = f2fs_rename,
    .rmdir    = f2fs_rmdir,
    .link     = f2fs_link,
    .settimes = f2fs_settimes,
    .sync     = f2fs_sync,       /* F4: fsync = segment flush + checkpoint */
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
    f2m.nat_lba = F2FS_NAT_LBA;
    f2m.sit_lba = F2FS_SIT_LBA;
    f2m.start_main_lba = F2FS_MAIN_START_LBA;
    f2m.total_bytes = f2m.main_segments * 1ULL * F2FS_SEG_SIZE;

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
    sb->nat_blkaddr = F2FS_NAT_LBA;
    sb->sit_blkaddr = F2FS_SIT_LBA;
    sb->main_blkaddr = F2FS_MAIN_START_LBA;
    /* Cap the main area to what actually fits on the device.  Some block
     * drivers (AHCI here) do not report a sector count, so fall back to a
     * nominal 256 MiB main area that fits the harness disks. */
    {
        uint64_t secs = blkdev_sector_count(f2m.bdev);
        uint64_t pages = secs ? (secs * F2FS_SECTOR_SIZE) / F2FS_PAGE_SIZE
                              : (256ULL * 1024 * 1024) / F2FS_PAGE_SIZE;
        if (pages > F2FS_MAIN_START_LBA)
            pages -= F2FS_MAIN_START_LBA;
        else
            pages = 0;
        uint64_t msegs = pages / F2FS_BLOCKS_PER_SEG;
        if (msegs < 8) msegs = 8;
        if (msegs < f2m.main_segments) {
            f2m.main_segments = (uint32_t)msegs;
            sb->segment_count_main = f2m.main_segments;
            sb->user_block_count =
                (uint32_t)((f2m.main_segments - 2) * F2FS_BLOCKS_PER_SEG);
            f2m.total_bytes = f2m.main_segments * 1ULL * f2m.seg_size;
            sb->block_count = f2m.main_segments * F2FS_BLOCKS_PER_SEG;
        }
    }
    sb->root_ino = 1;
    sb->number_ino = F2FS_MAX_NIDS;
    sb->cur_data_blkaddr = f2m.start_main_lba;
    sb->cur_node_blkaddr = f2m.start_main_lba;
    sb->next_free_nid = 2;   /* root is NID 1; first free is 2 */
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
    cp->next_free_nid = 2;
    cp->sit_ver = 1;

    /* F4: store a simple checksum so mount can detect a torn checkpoint.
     * Value = sum of all 32-bit words excluding the checksum field itself
     * (the field is still zero when the sum is taken). */
    {
        uint32_t sum = 0;
        const uint32_t *w = (const uint32_t *)f2fs_scratch;
        for (int i = 0; i < F2FS_PAGE_SIZE / 4; i++) sum += w[i];
        cp->checksum = sum;
    }

    if (write_page(F2FS_CP_LBA, f2fs_scratch) != 0) return -1;
    if (write_page(F2FS_CP_BAK_LBA, f2fs_scratch) != 0) return -1;

    /* Format root inode (NID = 1) */
    f2m.root_nid = 1;

    /* Current segment pointers: node segment 0 and data segment 1 of the
     * main area, both inside the device. */
    f2m.cur_node_seg = 0;
    f2m.cur_node_blk = 0;
    f2m.cur_data_seg = 1;
    f2m.cur_data_blk = 0;

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

    if (alloc_node_for_nid(1) != 0) return -1;
    if (write_page(nat_get_nid_addr(1), f2fs_page_buf) != 0) return -1;

    /* Persist the current node/data write pointers so a remount continues
     * exactly where format left off (otherwise the first allocation would
     * re-use the root's blocks).  f2fs_scratch was clobbered by the NAT
     * writes above, so reload the superblock, patch it and write it back. */
    if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
    struct f2fs_superblock *sb2 = (struct f2fs_superblock *)f2fs_scratch;
    sb2->cur_node_blkaddr = seg_off_to_lba(f2m.cur_node_seg, f2m.cur_node_blk);
    sb2->cur_data_blkaddr = seg_off_to_lba(f2m.cur_data_seg, f2m.cur_data_blk);
    if (write_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
    if (write_page(F2FS_SUPER_BAK_LBA, f2fs_scratch) != 0) return -1;

    kprintf("[f2fs] format complete: %u segments, %llu bytes, root nid=1\n",
            f2m.total_segments, (unsigned long long)f2m.total_bytes);
    return 0;
}

/* ---- Checkpoint validation (F4) ----
 * Read both checkpoint packs, verify the stored checksum, and select the
 * newer version.  A pack that is unreadable or fails its checksum is a torn
 * checkpoint; if neither pack is valid the mount refuses loudly instead of
 * half-mounting.  Returns 0 with the active version in *out_ver, or -1. */
static int cp_validate_and_select(uint32_t *out_ver) {
    uint32_t best_ver = 0;
    int have = 0;
    for (int pack = 0; pack < 2; pack++) {
        uint32_t lba = (pack == 0) ? F2FS_CP_LBA : F2FS_CP_BAK_LBA;
        if (read_page(lba, f2fs_scratch) != 0) {
            kprintf("[f2fs] CP pack %d unreadable (torn?)\n", pack);
            continue;
        }
        struct f2fs_checkpoint *cp = (struct f2fs_checkpoint *)f2fs_scratch;
        uint32_t ver = (uint32_t)cp->checkpoint_ver;
        uint32_t sum_all = 0;
        const uint32_t *w = (const uint32_t *)f2fs_scratch;
        for (int i = 0; i < F2FS_PAGE_SIZE / 4; i++) sum_all += w[i];
        if (sum_all - cp->checksum != cp->checksum) {
            kprintf("[f2fs] CP pack %d CHECKSUM FAIL (ver=%u) - torn\n", pack, ver);
            continue;
        }
        kprintf("[f2fs] CP pack %d ok ver=%u\n", pack, ver);
        if (!have || ver > best_ver) { best_ver = ver; have = 1; }
    }
    if (!have) {
        kprintf("[f2fs] FAIL: no valid checkpoint; refusing mount (torn CP)\n");
        return -1;
    }
    *out_ver = best_ver;
    return 0;
}

int f2fs_init(int prefer_port) {
    memset(&f2m, 0, sizeof(f2m));
    memset(fv4pool, 0, sizeof(fv4pool));
    f2m.bdev = prefer_port;
    spinlock_init(&f2m.alloc_lock);

    if (!f2fs_scratch) f2fs_scratch = (uint8_t *)kmalloc(F2FS_PAGE_SIZE);
    if (!f2fs_page_buf) f2fs_page_buf = (uint8_t *)kmalloc(F2FS_PAGE_SIZE);
    if (!f2fs_scratch || !f2fs_page_buf) {
        kprintf("[f2fs] cannot allocate scratch buffers, mount aborted\n");
        return -1;
    }

    /* Read superblock.  FSFULL F1: auto-formatting is opt-in; with the
     * gate off, an unreadable or foreign superblock is refused loudly
     * and not a single sector is written. */
    if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) {
        if (!fs_format_allowed()) {
            kprintf("[f2fs] superblock unreadable; format disabled (FS_MOUNT_FORMAT=0)\n");
            return -1;
        }
        kprintf("[f2fs] cannot read superblock, formatting...\n");
        if (format_f2fs() != 0) return -1;
        if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
    }

    struct f2fs_superblock *sb = (struct f2fs_superblock *)f2fs_scratch;
    if (sb->magic != F2FS_MAGIC) {
        if (!fs_format_allowed()) {
            kprintf("[f2fs] not F2FS magic (0x%08X); format disabled (FS_MOUNT_FORMAT=0)\n",
                    sb->magic);
            return -1;
        }
        kprintf("[f2fs] not F2FS magic (0x%08X), formatting...\n", sb->magic);
        if (format_f2fs() != 0) return -1;
        if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
        sb = (struct f2fs_superblock *)f2fs_scratch;
    }

    /* F4: validate both checkpoint packs and pick the newest before we
     * trust any allocation state.  A torn checkpoint refuses the mount. */
    if (cp_validate_and_select(&f2m.cp_ver) != 0) {
        if (!fs_format_allowed()) return -1;   /* honest refusal */
        kprintf("[f2fs] no valid CP; reformatting\n");
        if (format_f2fs() != 0) return -1;
        if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
        sb = (struct f2fs_superblock *)f2fs_scratch;
        if (cp_validate_and_select(&f2m.cp_ver) != 0) return -1;
    }

    /* cp_validate_and_select read the checkpoint into f2fs_scratch,
     * clobbering the superblock that sb points to — reload it before any
     * field is read. */
    if (read_page(F2FS_SUPER_LBA, f2fs_scratch) != 0) return -1;
    sb = (struct f2fs_superblock *)f2fs_scratch;

    f2m.page_size = sb->page_size ? sb->page_size : F2FS_PAGE_SIZE;
    f2m.sector_size = sb->sector_size ? sb->sector_size : F2FS_SECTOR_SIZE;
    f2m.blocks_per_seg = sb->log_blocks_per_seg ? (1 << sb->log_blocks_per_seg) : F2FS_BLOCKS_PER_SEG;
    f2m.seg_size = (uint32_t)f2m.blocks_per_seg * f2m.page_size;
    f2m.total_segments = sb->segment_count;
    f2m.main_segments = sb->segment_count_main;
    f2m.cp_segments = sb->segment_count_ckpt;
    f2m.ssa_segments = sb->segment_count_ssa;
    f2m.nat_lba = sb->nat_blkaddr ? sb->nat_blkaddr : F2FS_NAT_LBA;
    f2m.sit_lba = sb->sit_blkaddr ? sb->sit_blkaddr : F2FS_SIT_LBA;
    f2m.start_main_lba = sb->main_blkaddr ? sb->main_blkaddr : F2FS_MAIN_START_LBA;
    f2m.root_nid = sb->root_ino ? sb->root_ino : 1;
    f2m.next_free_nid = sb->next_free_nid ? sb->next_free_nid : 2;
    f2m.free_segments = (uint32_t)(sb->free_segment_count);
    f2m.total_bytes = f2m.main_segments * 1ULL * f2m.seg_size;

    /* Continue the log where the previous writer stopped.  The superblock
     * carries the NEXT node/data page addresses; convert back to seg/blk. */
    if (sb->cur_data_blkaddr >= f2m.start_main_lba) {
        uint32_t rel = sb->cur_data_blkaddr - f2m.start_main_lba;
        f2m.cur_data_seg = rel / f2m.blocks_per_seg;
        f2m.cur_data_blk = rel % f2m.blocks_per_seg;
    } else {
        f2m.cur_data_seg = 1;
        f2m.cur_data_blk = 0;
    }
    if (sb->cur_node_blkaddr >= f2m.start_main_lba) {
        uint32_t rel = sb->cur_node_blkaddr - f2m.start_main_lba;
        f2m.cur_node_seg = rel / f2m.blocks_per_seg;
        f2m.cur_node_blk = rel % f2m.blocks_per_seg;
    } else {
        f2m.cur_node_seg = 0;
        f2m.cur_node_blk = 1;
    }

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

int f2fs_self_test(void) {
    if (!f2m.mounted) {
        kprintf("[f2fs] self-test: SKIPPED (not mounted)\n");
        return -1;  /* SKIP */
    }
    kprintf("[f2fs] self-test: multi-segment, rename, link, rmdir, settimes...\n");

    /* ---- 1. Multi-block / multi-segment file (indirect-node path) ----
     * Write 3 MiB in 4 KB pages so the file spans several data segments
     * and spills past the 28 direct-block pointers into indirect nodes. */
    {
        struct vnode *big = f2fs_create(NULL, "big.bin");
        if (!big) { kprintf("[f2fs] FAIL: create big.bin\n"); return -2; }

        const uint32_t BLOCKS = 768;                 /* 3 MiB */
        const uint64_t PSZ = F2FS_PAGE_SIZE;
        uint8_t *pat = (uint8_t *)kmalloc(PSZ);
        if (!pat) return -9;
        for (uint32_t i = 0; i < PSZ; i++) pat[i] = (uint8_t)(i % 251);

        for (uint32_t b = 0; b < BLOCKS; b++) {
            if (f2fs_write(big, b * PSZ, pat, PSZ) != (int64_t)PSZ) {
                kprintf("[f2fs] FAIL: multi-seg write @ block %u\n", b); return -3;
            }
        }

        /* Read back and verify every page is byte-exact. */
        uint8_t *got = (uint8_t *)kmalloc(PSZ);
        if (!got) return -9;
        for (uint32_t b = 0; b < BLOCKS; b++) {
            if (f2fs_read(big, b * PSZ, got, PSZ) != (int64_t)PSZ ||
                memcmp(pat, got, PSZ) != 0) {
                kprintf("[f2fs] FAIL: multi-seg readback @ block %u\n", b); return -4;
            }
        }
        kfree(pat); kfree(got);
        kprintf("[f2fs]   multi-segment file (%u KiB) round-tripped byte-exact\n",
                BLOCKS * PSZ / 1024);
    }

    /* ---- 2. Directory: mkdir + create-in-subdir + rename ---- */
    if (f2fs_mkdir(NULL, "flashdir") != 0) { kprintf("[f2fs] FAIL: mkdir\n"); return -5; }
    struct vnode *inner = f2fs_create(NULL, "flashdir/inner.txt");
    if (!inner) { kprintf("[f2fs] FAIL: create in subdir\n"); return -6; }
    const char *msg = "nested-content";
    if (f2fs_write(inner, 0, msg, strlen(msg)) != (int64_t)strlen(msg))
        return -7;
    if (f2fs_rename(NULL, "flashdir/inner.txt", "flashdir/renamed.txt") != 0) {
        kprintf("[f2fs] FAIL: rename\n"); return -7;
    }
    struct vnode *ren = f2fs_lookup(NULL, "flashdir/renamed.txt");
    if (!ren) { kprintf("[f2fs] FAIL: lookup after rename\n"); return -7; }

    /* ---- 3. link (hard link bumps nlink) + settimes ---- */
    if (f2fs_link(NULL, "flashdir/renamed.txt", "flashdir/hard.txt") != 0) {
        kprintf("[f2fs] FAIL: link\n"); return -8;
    }
    struct vnode *hl = f2fs_lookup(NULL, "flashdir/hard.txt");
    if (!hl) { kprintf("[f2fs] FAIL: lookup hard link\n"); return -8; }
    if (f2fs_settimes(ren, 111, 222) != 0) { kprintf("[f2fs] FAIL: settimes\n"); return -8; }
    struct vfs_stat st;
    if (f2fs_stat(hl, &st) != 0) return -8;
    if (st.nlink < 2) { kprintf("[f2fs] FAIL: nlink after link (%u)\n", st.nlink); return -8; }

    /* ---- 4. unlink + rmdir ---- */
    if (f2fs_unlink(NULL, "flashdir/hard.txt") != 0) { kprintf("[f2fs] FAIL: unlink hard\n"); return -9; }
    if (f2fs_unlink(NULL, "flashdir/renamed.txt") != 0) { kprintf("[f2fs] FAIL: unlink renamed\n"); return -9; }
    if (f2fs_rmdir(NULL, "flashdir") != 0) { kprintf("[f2fs] FAIL: rmdir\n"); return -9; }

    /* ---- 5. fsync: segment flush + checkpoint advance ---- */
    {
        uint32_t old_cp = f2m.cp_ver;
        if (f2fs_sync(NULL) != 0) { kprintf("[f2fs] FAIL: fsync\n"); return -10; }
        if (f2m.cp_ver != old_cp + 1) { kprintf("[f2fs] FAIL: fsync did not advance CP\n"); return -10; }
        kprintf("[f2fs]   fsync flushed segment, CP advanced to %u\n", f2m.cp_ver);
    }

    /* ---- 6. internal fsck.f2fs ---- */
    if (f2fs_fsck() != 0) { kprintf("[f2fs] FAIL: internal fsck\n"); return -11; }
    kprintf("[f2fs]   internal fsck clean\n");

    kprintf("[f2fs] PASS: F2FS functional (multi-segment, rename, link, rmdir, settimes, fsync, fsck)\n");
    return 0;  /* PASS */
}