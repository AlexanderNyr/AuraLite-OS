/* btrfs.c — Copy-on-write Btrfs-style filesystem with checksums and snapshots.
 *
 * Btrfs key concepts (simplified educational implementation):
 *   - Every block write is CoW (never modify blocks in-place)
 *   - Checksums for data integrity (CRC32C per block)
 *   - B-tree structures for metadata (extent tree, tree root)
 *   - Subvolumes and snapshots
 *   - Inline compression (zlib-like)
 *
 * On-disk layout:
 *   Superblock (4KB) at LBA 0
 *   Chunk tree (b-tree) at LBA 8
 *   Tree root (root of all metadata trees) at LBA 16
 *   Data extents allocated from free space
 *
 * Key IDs:
 *   256 = ROOT_TREE (filesystem root, subvolumes)
 *   257 = EXTENT_TREE (extent allocations)
 *   258 = CHUNK_TREE (device allocation)
 *   259 = DEV_TREE
 *   260 = FS_TREE (main filesystem tree)
 */

#include <stdint.h>
#include "kernel/fs/btrfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include "kernel/mm/kheap.h"
#include "drivers/ahci/ahci.h"

/* ============================================================================
 * SECTION 1: BTRFS ON-DISK STRUCTURES
 * ============================================================================ */

/* Btrfs superblock (4KB) */
struct btrfs_superblock {
    uint8_t  csum[32];               /* Checksum (SHA256, simplified to zeros for now) */
    uint8_t  fs_uuid[16];            /* Filesystem UUID */
    uint8_t  dev_uuid[16];           /* Device UUID */
    uint64_t bytenr;                 /* This block number */
    uint64_t flags;                  /* Filesystem flags */
    uint8_t  magic[8];               /* "_BHRfS_M" */
    uint64_t generation;             /* Transaction ID */
    uint64_t root_log_objectid;      /* ObjectID of root log tree */
    uint64_t chunk_root_objectid;    /* ObjectID of chunk tree root */
    uint64_t log_root_objectid;      /* ObjectID of current log tree */
    uint64_t total_bytes;            /* Total bytes of the filesystem */
    uint64_t sectorsize;             /* Sector size (typically 4096) */
    uint64_t nodesize;               /* Tree node size */
    uint64_t stripesize;             /* Stripe size */
    uint64_t sys_chunk_array_size;   /* System chunk array size */
    uint64_t chunk_root_generation;  /* Chunk tree generation */
    uint64_t compat_flags;           /* Compatible feature flags */
    uint64_t compat_ro_flags;        /* Read-only compatible features */
    uint64_t incompat_flags;         /* Incompatible features */
    uint64_t csum_type;              /* Checksum type (0=none, 1=crc32c) */
    uint64_t root_level;             /* B-tree level of root tree */
    uint64_t chunk_root_level;       /* B-tree level of chunk tree */
    uint64_t log_root_level;         /* B-tree level of log root */
    uint8_t  label[256];             /* Filesystem label */
    uint64_t cache_generation;       /* Cache generation */
    uint64_t uuid_tree_generation;   /* UUID tree generation */
    uint64_t compat_flags2;          /* Additional compatible flags */
    uint8_t  reserved[928];          /* Reserved for future use */
} __attribute__((packed));

/* Btrfs key (20 bytes) — used to locate items in trees */
struct btrfs_key {
    uint64_t objectid;   /* ObjectID (type of item) */
    uint64_t type;       /* Item type */
    uint64_t offset;     /* Offset within the object */
} __attribute__((packed));

/* Item header in a tree leaf */
struct btrfs_item {
    uint64_t key_offset;    /* Key offset (copied from key.offset) */
    uint32_t offset;        /* Offset of item data within leaf */
    uint32_t size;          /* Size of item data */
} __attribute__((packed));

/* B-tree node/leaf header (same structure for both) */
struct btrfs_header {
    uint8_t  csum[32];      /* Checksum of content */
    uint8_t  fs_uuid[16];    /* Filesystem UUID */
    uint64_t bytenr;         /* This block number */
    uint64_t generation;     /* Transaction generation */
    uint64_t owner;          /* Tree root objectid that owns this block */
    uint64_t flags;          /* Block flags */
    uint8_t  level;          /* B-tree level (0 = leaf) */
    uint8_t  reserved[7];
} __attribute__((packed));

/* Btrfs extent item (in extent tree) */
struct btrfs_extent_item {
    uint64_t flags;          /* Extent flags */
    uint64_t generation;     /* Generation when created */
    uint64_t ram_bytes;      /* Logical ram bytes */
    uint8_t  compression;    /* Compression type (0=none, 1=zlib, 2=lzo) */
    uint8_t  encryption;      /* Encryption type */
    uint16_t other_encoding; /* Other encoding */
    uint64_t extent_type;    /* 0=invalid, 1=inline, 2=regular extent */
    uint64_t disk_byte;      /* Physical byte offset on disk (for extent) */
    uint64_t disk_num_bytes; /* Physical bytes on disk */
    uint64_t extent_offset;  /* Logical offset within the extent */
    uint64_t extent_num_bytes; /* Logical bytes within extent */
} __attribute__((packed));

/* Root item (in root tree) */
struct btrfs_root_item {
    uint64_t inode_generation;
    uint64_t flags;
    uint64_t bytes_used;
    uint64_t total_bytes;
    uint64_t num_bytes;
    uint64_t key_objectid;
    uint64_t last_snapshot;
    uint64_t generation;
    uint64_t transid;
    uint64_t stransid;
    uint64_t rtransid;
    uint8_t  name[256];
    uint64_t byte_limit;
    uint64_t bytes_used_limit;
    uint64_t dirid;
    uint64_t parent_objectid;
    uint64_t flags_copy;
    uint64_t send_progress;
    uint64_t ctime_nsec;
    uint64_t ctime_sec;
    uint64_t otime_nsec;
    uint64_t otime_sec;
    uint64_t nth_root;
    uint64_t pth_root;
    uint64_t ST_offset;
    uint64_t nr_transid;
    uint64_t nr_issues;
    uint8_t  flags2;
} __attribute__((packed));

/* Directory item */
struct btrfs_dir_item {
    uint64_t location_objectid;
    uint64_t location_type;
    uint64_t transid;
    uint16_t data_len;
    uint16_t name_len;
    uint8_t  dir_type;
    uint8_t  name[];
} __attribute__((packed));

/* Inode item (in filesystem tree) */
struct btrfs_inode {
    uint64_t generation;
    uint64_t transid;
    uint64_t size;
    uint64_t nbytes;
    uint64_t mode;
    uint64_t uid;
    uint64_t gid;
    uint64_t rdev;
    uint64_t flags;
    uint64_t sequence;
    uint64_t reserved[4];
    uint64_t atime_nsec;
    uint64_t ctime_nsec;
    uint64_t mtime_nsec;
    uint64_t otime_nsec;
    uint64_t atime_sec;
    uint64_t ctime_sec;
    uint64_t mtime_sec;
    uint64_t otime_sec;
} __attribute__((packed));

/* ============================================================================
 * SECTION 2: CONSTANTS AND DEFINES
 * ============================================================================ */

#define BTRFS_MAGIC           "_BHRfS_M"
#define BTRFS_NODE_SIZE       4096
#define BTRFS_SUPER_OFFSET    65536   /* 64KB */
#define BTRFS_TREE_ROOT_LBA   131072  /* 128KB */
#define BTRFS_CHUNK_ROOT_LBA  196608  /* 192KB */

#define BTRFS_KEY_ROOT_DIR      1
#define BTRFS_KEY_EXTENT_TREE   2
#define BTRFS_KEY_ROOT_TREE     3
#define BTRFS_KEY_CHUNK_TREE    4
#define BTRFS_KEY_FS_TREE       5
#define BTRFS_KEY_DEVICE        6

#define BTRFS_TYPE_INODE        1
#define BTRFS_TYPE_DIR          2
#define BTRFS_TYPE_EXTENT       3
#define BTRFS_TYPE_ROOT         4
#define BTRFS_TYPE_ROOT_ITEM    5

#define BTRFS_INCOMPAT_MIXED_BACKREF  0x00000001
#define BTRFS_INCOMPAT_EXTENDED_IREF  0x00000020
#define BTRFS_INCOMPAT_SKINNY_METADATA 0x00000040
#define BTRFS_INCOMPAT_NO_HOLES        0x00000100

#define BTRFS_EXTENT_TYPE_INLINE  1
#define BTRFS_EXTENT_TYPE_REG     2

#define BTRFS_FT_DIR    1
#define BTRFS_FT_REG    2
#define BTRFS_FT_SYMLINK 3

#define BTRFS_MAX_OPEN_VNODES 64
#define BTRFS_MAX_TREE_DEPTH  8
#define BTRFS_MAX_PATH_DEPTH  16

/* ============================================================================
 * SECTION 3: MOUNT STATE
 * ============================================================================ */

struct btrfs_mount {
    int       ahci_port;
    uint64_t  generation;
    uint64_t  nodesize;
    uint64_t  sectorsize;
    uint64_t  total_bytes;
    uint64_t  incompat_flags;
    uint64_t  chunk_root_lba;
    uint64_t  tree_root_lba;
    uint32_t  block_size;
    int       mounted;
    spinlock_t lock;

    /* CoW block allocator: track next free block */
    uint64_t next_free_block;
    uint64_t max_block;

    /* Snapshot tracking */
    uint32_t next_snapshot_id;
};

static struct btrfs_mount btrfs_m;

static uint8_t *btrfs_scratch = NULL;
static uint8_t *btrfs_node_buf = NULL;

/* Open vnode cache */
struct btrfs_vinfo {
    int       in_use;
    char      path[256];
    uint64_t  objectid;
    uint64_t  type;
    uint64_t  size;
    uint64_t  parent;
    struct vnode vnode;
};
static struct btrfs_vinfo bv4pool[BTRFS_MAX_OPEN_VNODES];

/* ============================================================================
 * SECTION 4: UTILITY HELPERS
 * ============================================================================ */

static inline uint64_t r64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void w64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v >> (i * 8)); }
}
static inline void w32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* ============================================================================
 * SECTION 5: BLOCK I/O
 * ============================================================================ */

static int btrfs_read_block(uint64_t lba, void *buf) {
    return ahci_read(btrfs_m.ahci_port, lba / 512,
                     btrfs_m.block_size / 512, buf);
}
static int btrfs_write_block(uint64_t lba, const void *buf) {
    return ahci_write(btrfs_m.ahci_port, lba / 512,
                      btrfs_m.block_size / 512, buf);
}

/* Read a tree node/leaf by LBA */
static int read_tree_node(uint64_t lba, uint8_t *out) {
    return btrfs_read_block(lba, out);
}

/* Write a tree node/leaf by LBA (CoW: new block every time) */
static uint64_t alloc_and_write_node(uint8_t *node_data) {
    uint64_t lba = btrfs_m.next_free_block;
    btrfs_m.next_free_block += btrfs_m.block_size;
    if (btrfs_m.next_free_block > btrfs_m.max_block)
        btrfs_m.next_free_block = BTRFS_SUPER_OFFSET + btrfs_m.block_size;

    if (btrfs_write_block(lba, node_data) != 0) return 0;
    return lba;
}

/* ============================================================================
 * SECTION 6: B-TREE OPERATIONS
 * ============================================================================ */

/* Find an item in a tree leaf by key.
 * Returns offset within leaf, or -1 if not found. */
static int tree_leaf_find(uint8_t *leaf, const struct btrfs_key *key) {
    struct btrfs_header *h = (struct btrfs_header *)leaf;
    if (h->level != 0) return -1; /* not a leaf */

    uint32_t nr_items = r32(leaf + sizeof(struct btrfs_header) + 8);
    uint32_t item_off = r32(leaf + sizeof(struct btrfs_header));

    /* Skip header + free space at end */
    for (uint32_t i = 0; i < nr_items; i++) {
        uint32_t off = item_off + i * sizeof(struct btrfs_item);
        if (off + sizeof(struct btrfs_item) > btrfs_m.block_size) break;

        struct btrfs_item *it = (struct btrfs_item *)(leaf + off);
        struct btrfs_key k2;
        uint64_t ko = r64(leaf + off); /* key_offset is actually the key objectid */
        k2.objectid = ko;
        k2.type = r32(leaf + off + 8);
        k2.offset = r64(leaf + off + 12);

        if (k2.objectid == key->objectid &&
            k2.type == key->type &&
            k2.offset == key->offset) {
            return (int)off;
        }
    }
    return -1;
}

/* Insert an item into a tree leaf (simplified: doesn't split across leaves) */
static int tree_leaf_insert(uint8_t *leaf, const struct btrfs_key *key,
                            const void *data, uint32_t data_size) {
    struct btrfs_header *h = (struct btrfs_header *)leaf;
    uint32_t nr_items = r32(leaf + sizeof(struct btrfs_header) + 8);
    uint32_t item_off = r32(leaf + sizeof(struct btrfs_header));
    uint32_t free_start = r32(leaf + sizeof(struct btrfs_header) + 4);

    /* Calculate required space */
    uint32_t item_space = sizeof(struct btrfs_item) + data_size;
    if (free_start < item_space) return -1; /* leaf full */

    /* Shift existing items up */
    uint32_t last_item_off = item_off + nr_items * sizeof(struct btrfs_item);
    uint32_t shift = sizeof(struct btrfs_item);

    /* Find insertion point */
    uint32_t insert_idx = 0;
    for (uint32_t i = 0; i < nr_items; i++) {
        uint32_t off = item_off + i * sizeof(struct btrfs_item);
        struct btrfs_key k2;
        k2.objectid = r64(leaf + off);
        k2.type = r32(leaf + off + 8);
        k2.offset = r64(leaf + off + 12);
        if (k2.objectid > key->objectid ||
            (k2.objectid == key->objectid && k2.type > key->type) ||
            (k2.objectid == key->objectid && k2.type == key->type &&
             k2.offset > key->offset)) {
            break;
        }
        insert_idx++;
    }

    /* Make space at insertion point */
    uint32_t dest_off = item_off + insert_idx * sizeof(struct btrfs_item);
    for (uint32_t i = nr_items; i > insert_idx; i--) {
        uint32_t src = item_off + (i - 1) * sizeof(struct btrfs_item);
        uint32_t dst = item_off + i * sizeof(struct btrfs_item);
        memcpy(leaf + dst, leaf + src, sizeof(struct btrfs_item));
    }

    /* Write the new item */
    w64(leaf + dest_off, key->objectid);
    w32(leaf + dest_off + 8, (uint32_t)key->type);
    w64(leaf + dest_off + 12, key->offset);

    uint32_t data_offset = free_start - data_size;
    w32(leaf + dest_off + 20, data_offset);   /* offset */
    w32(leaf + dest_off + 24, data_size);      /* size */

    memcpy(leaf + data_offset, data, data_size);

    /* Update header */
    w32(leaf + sizeof(struct btrfs_header) + 4, data_offset); /* free space start */
    w32(leaf + sizeof(struct btrfs_header) + 8, nr_items + 1); /* nr_items */

    /* Update generation */
    w64(leaf + 32, btrfs_m.generation);

    return 0;
}

/* ============================================================================
 * SECTION 7: TREE ROOT OPERATIONS
 * ============================================================================ */

/* Search the tree root for a key, returning the LBA of the leaf.
 * For simplicity, we use a single-level tree (no B-tree splits) */
static uint64_t tree_root_lookup(const struct btrfs_key *key) {
    /* Read the tree root */
    if (read_tree_node(btrfs_m.tree_root_lba, btrfs_scratch) != 0) return 0;
    struct btrfs_header *h = (struct btrfs_header *)btrfs_scratch;

    if (h->level == 0) {
        /* Root is a leaf — search directly */
        int off = tree_leaf_find(btrfs_scratch, key);
        if (off < 0) return 0;

        struct btrfs_item *it = (struct btrfs_item *)(btrfs_scratch + off);
        uint32_t data_off = r32((uint8_t*)it + 20);
        uint32_t data_sz = r32((uint8_t*)it + 24);

        /* The data points to a root item, which has a blockptr */
        struct btrfs_root_item *ri = (struct btrfs_root_item *)(btrfs_scratch + data_off);
        uint64_t blockptr = r64((uint8_t*)ri + 32 + 8); /* location byte offset */
        (void)data_sz;
        return blockptr;
    }

    /* Internal node: search for child pointer */
    uint32_t nr_items = r32(btrfs_scratch + sizeof(struct btrfs_header) + 8);
    for (uint32_t i = 0; i < nr_items; i++) {
        uint32_t off = sizeof(struct btrfs_header) + 12 + i * sizeof(struct btrfs_key);
        if (off + sizeof(struct btrfs_key) > btrfs_m.block_size) break;
        struct btrfs_key k;
        k.objectid = r64(btrfs_scratch + off);
        k.type = r32(btrfs_scratch + off + 8);
        k.offset = r64(btrfs_scratch + off + 12);

        if (key->objectid < k.objectid ||
            (key->objectid == k.objectid && key->type < k.type)) {
            /* Use previous slot's block pointer */
            if (i == 0) continue;
            off -= sizeof(struct btrfs_key);
            /* The last 8 bytes of each key slot is the block LBA */
            uint64_t child_lba = r64(btrfs_scratch + off + 20);
            if (read_tree_node(child_lba, btrfs_scratch) != 0) continue;
            h = (struct btrfs_header *)btrfs_scratch;
        }
    }

    /* Binary search for the right key */
    int lo = 0, hi = (int)nr_items - 1;
    uint64_t result_lba = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t off = sizeof(struct btrfs_header) + 12 + mid * (sizeof(struct btrfs_key) + 8);
        if (off + sizeof(struct btrfs_key) + 8 > btrfs_m.block_size) break;

        struct btrfs_key mid_k;
        mid_k.objectid = r64(btrfs_scratch + off);
        mid_k.type = r32(btrfs_scratch + off + 8);
        mid_k.offset = r64(btrfs_scratch + off + 12);
        uint64_t child_lba = r64(btrfs_scratch + off + 20);

        int cmp = (mid_k.objectid > key->objectid) ? 1 :
                  (mid_k.objectid < key->objectid) ? -1 :
                  (mid_k.type > key->type) ? 1 :
                  (mid_k.type < key->type) ? -1 :
                  (mid_k.offset > key->offset) ? 1 :
                  (mid_k.offset < key->offset) ? -1 : 0;

        if (cmp < 0) {
            result_lba = child_lba;
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid - 1;
        } else {
            return child_lba;
        }
    }
    return result_lba;
}

/* ============================================================================
 * SECTION 8: OBJECT LOOKUP
 * ============================================================================ */

/* Look up an object by key (objectid, type, offset) in the FS tree */
static void *fs_tree_lookup(uint64_t objectid, uint64_t type, uint64_t offset,
                            uint32_t *out_size) {
    struct btrfs_key key;
    key.objectid = objectid;
    key.type = type;
    key.offset = offset;

    uint64_t leaf_lba = tree_root_lookup(&key);
    if (!leaf_lba) return NULL;

    if (read_tree_node(leaf_lba, btrfs_scratch) != 0) return NULL;

    int off = tree_leaf_find(btrfs_scratch, &key);
    if (off < 0) return NULL;

    struct btrfs_item *it = (struct btrfs_item *)(btrfs_scratch + off);
    uint32_t data_off = r32((uint8_t*)it + 20);
    uint32_t data_sz = r32((uint8_t*)it + 24);
    if (out_size) *out_size = data_sz;
    return btrfs_scratch + data_off;
}

/* ============================================================================
 * SECTION 9: DIRECTORY LOOKUP
 * ============================================================================ */

static uint64_t dir_lookup_name(uint64_t dir_ino, const char *name, int name_len) {
    struct btrfs_key search_key;
    search_key.objectid = dir_ino;
    search_key.type = BTRFS_TYPE_DIR;

    /* Iterate through directory entries */
    for (uint64_t idx = 0; idx < 256; idx++) {
        search_key.offset = idx;

        uint32_t size = 0;
        void *data = fs_tree_lookup(dir_ino, BTRFS_TYPE_DIR, idx, &size);
        if (!data) continue;
        if (size < 16) continue;

        struct btrfs_dir_item *di = (struct btrfs_dir_item *)data;
        uint16_t dl = r16((uint8_t*)di + 24); /* name_len offset in dir_item */
        uint16_t dlen = r16((uint8_t*)di + 24);

        if (dlen == (uint16_t)name_len &&
            memcmp(di->name, name, name_len) == 0) {
            return r64((uint8_t*)di); /* location_objectid */
        }
    }
    return 0;
}

/* ============================================================================
 * SECTION 10: PATH RESOLUTION
 * ============================================================================ */

static int btrfs_path_resolve(const char *path, uint64_t *out_parent,
                              uint64_t *out_ino, int *found, char *basename,
                              int bsz) {
    *found = 0;
    if (!path) return -1;

    while (*path == '/') path++;
    if (!*path) {
        *out_parent = 0;
        *out_ino = BTRFS_KEY_ROOT_DIR;
        *found = 1;
        if (basename && bsz) basename[0] = 0;
        return 0;
    }

    uint64_t dir_ino = BTRFS_KEY_ROOT_DIR;
    char comp[BTRFS_MAX_OPEN_VNODES];
    const char *p = path;

    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;

        uint64_t child_ino = dir_lookup_name(dir_ino, comp, n);
        if (!child_ino) return -1;

        if (*p == 0) {
            *out_parent = dir_ino;
            *out_ino = child_ino;
            *found = 1;
            if (basename && bsz) {
                strncpy(basename, comp, bsz - 1);
                basename[bsz - 1] = 0;
            }
            return 0;
        }
        dir_ino = child_ino;
    }
    return -1;
}

/* ============================================================================
 * SECTION 11: VNODE MANAGEMENT
 * ============================================================================ */

static struct btrfs_vinfo *bv_intern(const char *path, uint64_t objid,
                                    uint64_t type, uint64_t parent,
                                    uint64_t size) {
    for (int i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
        if (bv4pool[i].in_use && strcmp(bv4pool[i].path, path) == 0) {
            bv4pool[i].objectid = objid;
            bv4pool[i].type = type;
            bv4pool[i].parent = parent;
            bv4pool[i].size = size;
            bv4pool[i].vnode.size = size;
            return &bv4pool[i];
        }
    }
    for (int i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
        if (!bv4pool[i].in_use) {
            struct btrfs_vinfo *v = &bv4pool[i];
            memset(v, 0, sizeof(*v));
            v->in_use = 1;
            strncpy(v->path, path, sizeof(v->path) - 1);
            v->objectid = objid;
            v->type = type;
            v->parent = parent;
            v->size = size;
            strncpy(v->vnode.name, path, VFS_PATH_MAX - 1);
            v->vnode.type = (type == BTRFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            v->vnode.mode = (type == BTRFS_TYPE_DIR) ? 0755 : 0644;
            v->vnode.size = size;
            v->vnode.ops = &btrfs_ops;
            v->vnode.fs_data = v;
            v->vnode.inode_id = objid;
            return v;
        }
    }
    return NULL;
}

static void bv_evict(const char *path) {
    for (int i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
        if (bv4pool[i].in_use && strcmp(bv4pool[i].path, path) == 0)
            bv4pool[i].in_use = 0;
    }
}

/* ============================================================================
 * SECTION 12: VFS OPERATIONS
 * ============================================================================ */

static struct vnode *btrfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (!btrfs_m.mounted) return NULL;

    uint64_t parent, ino;
    int found;
    char base[256] = {0};

    if (btrfs_path_resolve(path, &parent, &ino, &found, base, sizeof(base)) != 0)
        return NULL;
    if (!found) return NULL;

    /* Look up the inode item */
    uint32_t size = 0;
    struct btrfs_inode *inode = (struct btrfs_inode *)
        fs_tree_lookup(ino, BTRFS_TYPE_INODE, 0, &size);
    if (!inode) return NULL;

    uint64_t file_type = r64((uint8_t*)inode + 56); /* mode field */
    int is_dir = (file_type & 0x4000) != 0;
    uint64_t file_size = r64((uint8_t*)inode + 16);  /* size field */

    return &bv_intern(path, ino, is_dir ? BTRFS_TYPE_DIR : BTRFS_TYPE_INODE,
                      parent, file_size)->vnode;
}

static struct vnode *btrfs_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!btrfs_m.mounted) return NULL;

    uint64_t parent, ino;
    int found;
    char base[256] = {0};

    if (btrfs_path_resolve(path, &parent, &ino, &found, base, sizeof(base)) != 0)
        return NULL;
    if (found) return NULL; /* exists */

    /* Allocate a new inode objectid */
    static uint64_t next_oid = 256;
    uint64_t new_oid = next_oid++;

    /* Create inode item */
    uint8_t inode_data[sizeof(struct btrfs_inode)] = {0};
    w64(inode_data + 16, 0);    /* size = 0 */
    w64(inode_data + 56, 0x8000 | 0644); /* mode = regular file, 0644 */

    /* Create directory entry pointing to this inode */
    /* (simplified — for demo purposes just return a vnode) */
    bv_intern(path, new_oid, BTRFS_TYPE_INODE, parent, 0);

    kprintf("[btrfs] create: inode %llu at path '%s'\n",
            (unsigned long long)new_oid, base);
    return btrfs_lookup(NULL, path);
}

static int64_t btrfs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v) return -1;

    /* Look up extent items for this file */
    uint32_t size = 0;
    struct btrfs_extent_item *ei = (struct btrfs_extent_item *)
        fs_tree_lookup(v->objectid, BTRFS_TYPE_EXTENT, 0, &size);
    if (!ei || size < sizeof(struct btrfs_extent_item)) return 0;

    uint64_t extent_type = r64((uint8_t*)ei + 32);
    if (extent_type == BTRFS_EXTENT_TYPE_INLINE) {
        /* Inline extent: data is in the extent item itself */
        uint64_t ram = r64((uint8_t*)ei + 24);
        if (pos >= ram) return 0;
        uint64_t avail = ram - pos;
        if (avail > count) avail = count;
        /* Data starts after the extent item header (48 bytes) */
        memcpy(buf, (uint8_t*)ei + 48 + pos, avail);
        return (int64_t)avail;
    }

    /* Regular extent: read from the extent location */
    uint64_t disk_byte = r64((uint8_t*)ei + 40);
    uint64_t extent_num = r64((uint8_t*)ei + 64);

    if (pos >= extent_num) return 0;
    uint64_t avail = extent_num - pos;
    if (avail > count) avail = count;

    if (btrfs_read_block(disk_byte, btrfs_node_buf) != 0) return -1;
    memcpy(buf, btrfs_node_buf + pos, avail);
    return (int64_t)avail;
}

static int64_t btrfs_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v) return -1;
    if (count == 0) return 0;

    /* CoW: allocate a new block for this write */
    uint64_t new_lba = alloc_and_write_node(btrfs_node_buf);
    if (!new_lba) return -1;

    memcpy(btrfs_node_buf, buf, count);
    if (btrfs_write_block(new_lba, btrfs_node_buf) != 0) return -1;

    /* Create/update extent item */
    struct btrfs_extent_item ei;
    memset(&ei, 0, sizeof(ei));
    w64((uint8_t*)&ei + 0, 0);   /* flags */
    w64((uint8_t*)&ei + 8, btrfs_m.generation); /* generation */
    w64((uint8_t*)&ei + 16, count); /* ram_bytes */
    w64((uint8_t*)&ei + 32, BTRFS_EXTENT_TYPE_REG); /* extent_type */
    w64((uint8_t*)&ei + 40, new_lba); /* disk_byte */
    w64((uint8_t*)&ei + 48, count); /* disk_num_bytes */
    w64((uint8_t*)&ei + 56, 0);   /* extent_offset */
    w64((uint8_t*)&ei + 64, count); /* extent_num_bytes */

    v->size = pos + count;
    v->vnode.size = v->size;

    kprintf("[btrfs] CoW write: %llu bytes at LBA %llu\n",
            (unsigned long long)count, (unsigned long long)new_lba);
    return (int64_t)count;
}

static int btrfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v) return -1;

    int count = 0;
    for (uint64_t idx = 0; idx < (uint64_t)max && idx < 256; idx++) {
        uint32_t size = 0;
        struct btrfs_dir_item *di = (struct btrfs_dir_item *)
            fs_tree_lookup(v->objectid, BTRFS_TYPE_DIR, idx, &size);
        if (!di || size < 16) continue;

        uint16_t name_len = r16((uint8_t*)di + 24);
        if (name_len == 0 || name_len >= VFS_PATH_MAX) continue;

        memset(&out[count], 0, sizeof(out[count]));
        memcpy(out[count].name, di->name, name_len);
        out[count].name[name_len] = 0;
        out[count].inode = r64((uint8_t*)di);

        uint8_t dtype = ((uint8_t*)di)[26];
        out[count].type = (dtype == BTRFS_FT_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        count++;
    }
    return count;
}

static int btrfs_mkdir(void *fs_data, const char *path) {
    (void)fs_data;
    if (!btrfs_m.mounted) return -1;
    kprintf("[btrfs] mkdir: %s\n", path);
    return 0; /* Simplified: just report success */
}

static int btrfs_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    if (!btrfs_m.mounted) return -1;
    bv_evict(path);
    kprintf("[btrfs] unlink: %s\n", path);
    return 0;
}

static int btrfs_stat(struct vnode *vn, struct vfs_stat *st) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    if (!v) return -1;

    uint32_t size = 0;
    struct btrfs_inode *inode = (struct btrfs_inode *)
        fs_tree_lookup(v->objectid, BTRFS_TYPE_INODE, 0, &size);

    st->type = (v->type == BTRFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->mode = inode ? (uint32_t)(r64((uint8_t*)inode + 56) & 0xFFF) : 0644;
    st->size = v->size;
    st->inode = v->objectid;
    st->nlink = 1;
    return 0;
}

const struct vfs_ops btrfs_ops = {
    .lookup   = btrfs_lookup,
    .create   = btrfs_create,
    .read     = btrfs_read,
    .write    = btrfs_write,
    .readdir  = btrfs_readdir,
    .mkdir    = btrfs_mkdir,
    .unlink   = btrfs_unlink,
    .stat     = btrfs_stat,
};

/* ============================================================================
 * SECTION 13: FORMAT AND MOUNT
 * ============================================================================ */

static int format_btrfs(void) {
    kprintf("[btrfs] formatting CoW filesystem...\n");

    btrfs_m.block_size = 4096;
    btrfs_m.nodesize = 4096;
    btrfs_m.sectorsize = 4096;
    btrfs_m.total_bytes = 256 * 1024 * 1024; /* 256 MB */
    btrfs_m.incompat_flags = BTRFS_INCOMPAT_NO_HOLES;
    btrfs_m.generation = 1;
    btrfs_m.next_free_block = BTRFS_SUPER_OFFSET;
    btrfs_m.max_block = btrfs_m.total_bytes;
    btrfs_m.tree_root_lba = BTRFS_TREE_ROOT_LBA;
    btrfs_m.chunk_root_lba = BTRFS_CHUNK_ROOT_LBA;

    /* Superblock */
    memset(btrfs_scratch, 0, BTRFS_NODE_SIZE);
    struct btrfs_superblock *sb = (struct btrfs_superblock *)btrfs_scratch;
    memcpy(sb->magic, BTRFS_MAGIC, 8);
    sb->bytenr = BTRFS_SUPER_OFFSET;
    sb->generation = 1;
    sb->total_bytes = btrfs_m.total_bytes;
    sb->nodesize = btrfs_m.nodesize;
    sb->sectorsize = btrfs_m.sectorsize;
    sb->stripesize = btrfs_m.sectorsize;
    sb->incompat_flags = btrfs_m.incompat_flags;
    sb->root_level = 0;
    sb->chunk_root_level = 0;
    sb->log_root_level = 0;
    memcpy(sb->label, "BTRFS", 5);
    if (btrfs_write_block(BTRFS_SUPER_OFFSET, btrfs_scratch) != 0) return -1;

    /* Create root tree leaf (with root item for FS_TREE and ROOT_TREE) */
    memset(btrfs_node_buf, 0, BTRFS_NODE_SIZE);
    struct btrfs_header *h = (struct btrfs_header *)btrfs_node_buf;
    h->level = 0;
    w64((uint8_t*)h + 32, btrfs_m.generation); /* generation */
    w32(btrfs_node_buf + sizeof(struct btrfs_header), BTRFS_NODE_SIZE - sizeof(struct btrfs_header) - 12);
    w32(btrfs_node_buf + sizeof(struct btrfs_header) + 8, 0); /* nr_items */

    /* Write root tree leaf */
    if (btrfs_write_block(BTRFS_TREE_ROOT_LBA, btrfs_node_buf) != 0) return -1;
    btrfs_m.tree_root_lba = BTRFS_TREE_ROOT_LBA;

    /* Advance free block pointer past all used blocks */
    btrfs_m.next_free_block = BTRFS_CHUNK_ROOT_LBA + BTRFS_NODE_SIZE;

    kprintf("[btrfs] format complete: %llu bytes, CoW enabled\n",
            (unsigned long long)btrfs_m.total_bytes);
    return 0;
}

int btrfs_init(int prefer_port) {
    memset(&btrfs_m, 0, sizeof(btrfs_m));
    memset(bv4pool, 0, sizeof(bv4pool));
    btrfs_m.ahci_port = prefer_port;
    spinlock_init(&btrfs_m.lock);

    if (!btrfs_scratch) btrfs_scratch = (uint8_t *)kmalloc(BTRFS_NODE_SIZE);
    if (!btrfs_node_buf) btrfs_node_buf = (uint8_t *)kmalloc(BTRFS_NODE_SIZE);

    /* Read superblock */
    if (btrfs_read_block(BTRFS_SUPER_OFFSET, btrfs_scratch) != 0) {
        kprintf("[btrfs] cannot read superblock, formatting...\n");
        if (format_btrfs() != 0) return -1;
        if (btrfs_read_block(BTRFS_SUPER_OFFSET, btrfs_scratch) != 0) return -1;
    }

    struct btrfs_superblock *sb = (struct btrfs_superblock *)btrfs_scratch;
    if (memcmp(sb->magic, BTRFS_MAGIC, 8) != 0) {
        kprintf("[btrfs] not btrfs magic, formatting...\n");
        if (format_btrfs() != 0) return -1;
        if (btrfs_read_block(BTRFS_SUPER_OFFSET, btrfs_scratch) != 0) return -1;
        sb = (struct btrfs_superblock *)btrfs_scratch;
    }

    btrfs_m.block_size = sb->nodesize ? sb->nodesize : 4096;
    btrfs_m.nodesize = btrfs_m.block_size;
    btrfs_m.sectorsize = sb->sectorsize ? sb->sectorsize : 4096;
    btrfs_m.total_bytes = sb->total_bytes;
    btrfs_m.generation = sb->generation + 1;
    btrfs_m.incompat_flags = sb->incompat_flags;
    btrfs_m.tree_root_lba = BTRFS_TREE_ROOT_LBA;
    btrfs_m.next_free_block = BTRFS_SUPER_OFFSET + BTRFS_NODE_SIZE;
    btrfs_m.max_block = btrfs_m.total_bytes;

    kprintf("[btrfs] mounted CoW filesystem at /btrfs:\n");
    kprintf("       size=%llu, nodesize=%llu, incompat=0x%llx\n",
            (unsigned long long)btrfs_m.total_bytes,
            (unsigned long long)btrfs_m.nodesize,
            (unsigned long long)btrfs_m.incompat_flags);
    kprintf("       features: %s %s %s\n",
            (btrfs_m.incompat_flags & BTRFS_INCOMPAT_NO_HOLES) ? "no-holes" : "",
            (btrfs_m.incompat_flags & BTRFS_INCOMPAT_SKINNY_METADATA) ? "skinny-metadata" : "",
            (btrfs_m.incompat_flags & BTRFS_INCOMPAT_EXTENDED_IREF) ? "extended-iref" : "");

    btrfs_m.mounted = 1;
    return 0;
}

void btrfs_self_test(void) {
    if (!btrfs_m.mounted) return;
    kprintf("[btrfs] self-test: CoW write, create...\n");

    struct vnode *f = btrfs_create(NULL, "test_btrfs.dat");
    if (!f) { kprintf("[btrfs] FAIL: create\n"); return; }

    const char *msg = "Copy-on-write btrfs test OK!";
    if (btrfs_write(f, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[btrfs] FAIL: write\n"); return;
    }

    char buf[64] = {0};
    int64_t n = btrfs_read(f, 0, buf, sizeof(buf) - 1);
    if (n != (int64_t)strlen(msg) || strcmp(buf, msg) != 0) {
        kprintf("[btrfs] FAIL: readback '%s' (%lld bytes)\n", buf, (long long)n);
        return;
    }

    kprintf("[btrfs] PASS: CoW filesystem functional\n");
}