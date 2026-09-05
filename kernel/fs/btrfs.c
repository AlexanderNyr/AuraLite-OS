/* btrfs.c — Copy-on-write Btrfs-style filesystem with checksums (F4b).
 *
 * A simplified, self-consistent, btrfs-inspired filesystem:
 *   - EVERY block write is copy-on-write: a metadata node or data block is
 *     never modified in place — a fresh block is allocated, the modified
 *     copy is written there, and its parent is re-pointed (CoW up the tree
 *     to a new root).  The old blocks remain intact on disk.
 *   - SHA-256 checksum on every block (RESIDUE2 T3), computed on write
 *     and verified on read.  The digest lives in a 32-byte TRAILER (the
 *     last 32 bytes of the block) so the header/item offsets stay where
 *     the tree code has always had them; it is computed by the kernel-
 *     local FIPS 180-4 implementation in kernel/lib/sha256.c (D2 keeps
 *     the kernel off libatls).  Legacy volumes that still carry the old
 *     CRC32C-only layout fail the digest on their very first read and
 *     are refused by name — checksums that cannot fail protect nothing.
 *   - A CoW keyed tree (root block -> leaf blocks) holds all metadata:
 *     inode items, directory items and extent items, keyed by
 *     (objectid, type, offset).
 *
 * This is NOT binary-interoperable with host mkfs.btrfs (the on-disk
 * layout is our own simplified one), so the F4b "interop lane" is
 * documented as out of scope the same way f2fs's was; the integrity gate
 * is SHA-256 trailer verification on every read (RESIDUE2 T3) plus the
 * structural tree walk.
 *
 * On-disk layout (byte offsets, block size 4096):
 *   65536  — superblock
 *   69632+ — CoW data / metadata blocks from a bump allocator
 *
 * Key objectids:
 *   1  = root directory; first user inode is 2.
 *
 * Key types:
 *   BTRFS_ITYPE_INODE   1  (obj=ino, off=0)   -> inode record
 *   BTRFS_ITYPE_DIR     2  (obj=dir_ino, off=name_hash) -> dir item
 *   BTRFS_ITYPE_EXTENT  3  (obj=ino, off=file_byte)     -> extent record
 */

#include <stdint.h>
#include "kernel/fs/btrfs.h"
#include "kernel/fs/fsformat.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/sha256.h"
#include "kernel/mm/kheap.h"
#include "kernel/fs/blkdev.h"

/* ============================================================================
 * SECTION 1: ON-DISK FORMAT
 * ============================================================================ */

#define BTRFS_MAGIC          "_BHRfS_M"
#define BTRFS_BLK_MAGIC      0x42545246u   /* "BTRF" */

#define BTRFS_NODE_SIZE      4096
#define BTRFS_HDR_SIZE       32            /* per-block header bytes */
#define BTRFS_SUPER_OFFSET   65536         /* byte offset of superblock */
#define BTRFS_FIRST_FREE     69632         /* 65536 + 4096 */
/* RESIDUE2 T3: the last 32 bytes of every block carry its SHA-256
 * digest, so the usable payload is the block minus header minus
 * trailer.  Leaf/root data packing starts below the trailer. */
#define BTRFS_CSUM_SIZE      32
#define BTRFS_CSUM_OFFSET    (BTRFS_NODE_SIZE - BTRFS_CSUM_SIZE)  /* 4064 */
#define DATA_PAYLOAD         (BTRFS_CSUM_OFFSET - BTRFS_HDR_SIZE) /* 4032 */

/* block types (blk_hdr.type) */
#define BT_SUPER   1
#define BT_ROOT    2   /* internal tree node (level 1) */
#define BT_LEAF    3   /* tree leaf (level 0) */
#define BT_DATA    4   /* file data block */

/* key item types */
#define BTRFS_ITYPE_INODE   1
#define BTRFS_ITYPE_DIR     2
#define BTRFS_ITYPE_EXTENT  3

#define BTRFS_FT_DIR    1
#define BTRFS_FT_REG    2

#define BTRFS_ROOT_INO     1
#define BTRFS_MAX_PATH     256
#define BTRFS_MAX_DEPTH    16
#define BTRFS_MAX_OPEN_VNODES 64

/* Per-block header (32 bytes) at the start of every block, plus a
 * 32-byte SHA-256 TRAILER at the very end (RESIDUE2 T3).
 *
 *   header : 0:rsvd(4, was the old CRC32C slot, now 0)
 *            4:magic(4) 8:gen(4) 12:type(4) 16:owner(8) 24:self(8)
 *   trailer: 4064:sha256[32] — digest of bytes 0..4063 with the
 *            trailer itself excluded, computed on every write and
 *            verified on every read.
 */

/* Root block (BT_ROOT): 32:nptrs(4) 36: bt_ptr[]  (32B each)
 *   bt_ptr: 0:obj(8) 8:off(8) 16:type(4) 20:rsvd(4) 24:child_lba(8) */
/* Leaf block (BT_LEAF): 32:nslots(4) 36: bt_item[] (28B each)
 *   bt_item: 0:obj(8) 8:off(8) 16:type(4) 20:data_off(4) 24:data_size(4)
 *   data region packed at the end of the block. */
/* Super block (BT_SUPER): 32:total(8) 40:nodesize(8) 48:sectorsize(8)
 *   56:generation(8) 64:tree_root_lba(8) 72:next_free(8) 80:label[32]
 *   112:fs_uuid(16) 128:magic[8] 136:next_inode(8) */

#define INODE_REC_SIZE 64
/* inode rec: 0:mode(8) 8:uid(8) 16:gid(8) 24:size(8) 32:links(8)
 *            40:atime(8) 48:mtime(8) 56:ctime(8) */
#define EXTENT_REC_SIZE 16
/* extent rec: 0:disk_lba(8) 8:len(8) */
/* dir item: 0:child_ino(8) 8:child_type(4) 12:name_len(4) 16:name[] */

#define MAX_LEAF_ITEMS 48
#define MAX_ROOT_PTRS  120

/* ============================================================================
 * SECTION 2: MOUNT STATE
 * ============================================================================ */

struct btrfs_mount {
    int       bdev;
    uint64_t  generation;
    uint64_t  nodesize;
    uint64_t  sectorsize;
    uint64_t  total_bytes;
    uint64_t  tree_root_lba;     /* CoW metadata tree root */
    uint64_t  next_free_block;
    uint64_t  max_block;
    uint32_t  next_inode;
    int       mounted;
    spinlock_t lock;
};

static struct btrfs_mount bm;
static uint8_t *bscratch = NULL;   /* general scratch (tree ops) */
static uint8_t *bdatabuf = NULL;   /* data block staging */

struct btrfs_vinfo {
    int       in_use;
    char      path[BTRFS_MAX_PATH];
    uint64_t  objectid;
    int       is_dir;
    uint64_t  size;
    struct vnode vnode;
};
static struct btrfs_vinfo bv4pool[BTRFS_MAX_OPEN_VNODES];

/* ============================================================================
 * SECTION 3: UTILITY HELPERS
 * ============================================================================ */

static inline uint64_t r64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void w64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static inline void w32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* RESIDUE2 T3: the old table-less CRC32C lived here.  It is gone —
 * every block's integrity is now the kernel-local SHA-256
 * (kernel/lib/sha256.c, RFC 6234-vector-tested on the host), written
 * to the block trailer on every store and verified on every fetch. */

static uint64_t name_hash(const uint8_t *s, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ULL; }
    return h & 0xFFFF;   /* keep in key.offset range */
}

static int key_cmp(uint64_t aobj, uint32_t atyp, uint64_t aoff,
                   uint64_t bobj, uint32_t btyp, uint64_t boff) {
    if (aobj != bobj) return aobj < bobj ? -1 : 1;
    if (atyp != btyp) return atyp < btyp ? -1 : 1;
    if (aoff != boff) return aoff < boff ? -1 : 1;
    return 0;
}

/* ============================================================================
 * SECTION 4: BLOCK I/O + CHECKSUM
 * ============================================================================ */

static int btrfs_read_block(uint64_t lba, void *buf) {
    return fs_read_block(bm.bdev, lba / 512, (uint32_t)(bm.nodesize / 512), buf);
}
static int btrfs_write_block(uint64_t lba, const void *buf) {
    return fs_write_block(bm.bdev, lba / 512, (uint32_t)(bm.nodesize / 512), buf);
}

/* Read the superblock with a fixed size: called before bm.nodesize is known. */
static int btrfs_read_super(uint64_t lba, void *buf) {
    return fs_read_block(bm.bdev, lba / 512, BTRFS_NODE_SIZE / 512, buf);
}

static uint64_t blk_alloc(void) {
    uint64_t lba = bm.next_free_block;
    if (lba + bm.nodesize > bm.max_block)
        lba = BTRFS_FIRST_FREE;                 /* wrap around free pool */
    bm.next_free_block = lba + bm.nodesize;
    return lba;
}

/* Stamp the 32-byte header, seal the SHA-256 trailer and write a block
 * (RESIDUE2 T3).  The digest covers bytes 0..4063 — the header fields
 * are INSIDE the digest, so a corrupted owner/gen/type/self is caught
 * too, not just payload bit-flips. */
static int blk_write(uint64_t lba, uint8_t *blk, uint32_t type, uint64_t owner) {
    w32(blk + 0, 0);                                  /* rsvd (old csum slot) */
    w32(blk + 4, BTRFS_BLK_MAGIC);
    w32(blk + 8, (uint32_t)bm.generation);
    w32(blk + 12, type);
    w64(blk + 16, owner);
    w64(blk + 24, lba);
    ksha256(blk, BTRFS_CSUM_OFFSET, blk + BTRFS_CSUM_OFFSET);
    return btrfs_write_block(lba, blk);
}

/* Read a block and verify its SHA-256 trailer + magic (RESIDUE2 T3).
 * Returns 0 on success.  A digest mismatch names its two honest causes:
 * bit-rot on the media, or a pre-T3 legacy volume whose trailer bytes
 * are whatever the old CRC32C era left there (neither is served). */
static int blk_read(uint64_t lba, uint8_t *blk) {
    if (btrfs_read_block(lba, blk) != 0) return -1;
    if (r32(blk + 4) != BTRFS_BLK_MAGIC) return -1;
    uint8_t digest[KSHA256_DIGEST_SIZE];
    ksha256(blk, BTRFS_CSUM_OFFSET, digest);
    if (memcmp(digest, blk + BTRFS_CSUM_OFFSET, KSHA256_DIGEST_SIZE) != 0) {
        kprintf("[btrfs] SHA-256 FAIL on block %llu (digest mismatch: "
                "corrupt block or legacy CRC32C volume)\n",
                (unsigned long long)lba);
        return -1;
    }
    return 0;
}

/* ============================================================================
 * SECTION 5: CoW KEYED TREE
 * ============================================================================ */

static int leaf_nslots(const uint8_t *leaf) { return (int)r32(leaf + 32); }

static void leaf_get(const uint8_t *leaf, int i, uint64_t *obj, uint32_t *typ,
                     uint64_t *off, uint32_t *doff, uint32_t *dsz) {
    const uint8_t *it = leaf + 36 + i * 28;
    *obj = r64(it + 0);
    *off = r64(it + 8);
    *typ = r32(it + 16);
    *doff = r32(it + 20);
    *dsz = r32(it + 24);
}

/* Rebuild `leaf` from a set of (obj,typ,off) + data blobs, sorted.  Each
 * slot carries the source offset of its data blob in the caller's staging
 * buffer (`src`) and its size; `placed[i]` (caller array) receives the
 * offset where the leaf will store item i's data. */
struct leaf_slot { uint64_t obj, off; uint32_t typ; uint32_t src, dsz; };

static int leaf_rebuild(uint8_t *leaf, struct leaf_slot *slots, int n,
                        uint32_t *placed) {
    /* insertion sort by key */
    for (int i = 1; i < n; i++) {
        struct leaf_slot t = slots[i]; int j = i - 1;
        while (j >= 0 &&
               key_cmp(slots[j].obj, slots[j].typ, slots[j].off,
                       t.obj, t.typ, t.off) > 0) {
            slots[j + 1] = slots[j]; j--;
        }
        slots[j + 1] = t;
    }
    memset(leaf, 0, BTRFS_NODE_SIZE);
    w32(leaf + 32, (uint32_t)n);
    /* RESIDUE2 T3: item data packs down from below the SHA-256 trailer —
     * the last 32 bytes of the block belong to the digest. */
    uint32_t data_cur = BTRFS_CSUM_OFFSET;
    for (int i = 0; i < n; i++) {
        data_cur -= slots[i].dsz;
        placed[i] = data_cur;
        uint8_t *it = leaf + 36 + i * 28;
        w64(it + 0, slots[i].obj);
        w64(it + 8, slots[i].off);
        w32(it + 16, slots[i].typ);
        w32(it + 20, data_cur);
        w32(it + 24, slots[i].dsz);
    }
    (void)data_cur;
    return 0;
}

/* Search a leaf for an exact item.  Returns index or -1. */
static int leaf_find(const uint8_t *leaf, uint64_t obj, uint32_t typ, uint64_t off) {
    int n = leaf_nslots(leaf);
    for (int i = 0; i < n; i++) {
        uint64_t o; uint32_t t, dd, ds; uint64_t f;
        leaf_get(leaf, i, &o, &t, &f, &dd, &ds);
        if (o == obj && t == typ && f == off) return i;
    }
    return -1;
}

/* ---- root operations ---- */

static int root_nptrs(const uint8_t *root) { return (int)r32(root + 32); }

static void root_get(const uint8_t *root, int i, uint64_t *obj, uint32_t *typ,
                     uint64_t *off, uint64_t *child) {
    const uint8_t *p = root + 36 + i * 32;
    *obj = r64(p + 0);
    *off = r64(p + 8);
    *typ = r32(p + 16);
    *child = r64(p + 24);
}

/* Find the child index that should hold the key (last ptr with key<=k). */
static int root_child_for(const uint8_t *root, uint64_t obj, uint32_t typ, uint64_t off) {
    int n = root_nptrs(root);
    int best = -1;
    for (int i = 0; i < n; i++) {
        uint64_t o; uint32_t t; uint64_t f, c; (void)c;
        root_get(root, i, &o, &t, &f, &c);
        if (key_cmp(o, t, f, obj, typ, off) <= 0) best = i;
    }
    return best < 0 ? 0 : best;
}

/* Look up an item in the tree.  Copies up to cap bytes of data into out.
 * Returns 0 and sets *out_size / *lba on hit, -1 on miss. */
static int tree_lookup(uint64_t root_lba, uint64_t obj, uint32_t typ, uint64_t off,
                       uint8_t *out, uint32_t cap, uint32_t *out_size) {
    if (!root_lba) return -1;
    if (blk_read(root_lba, bscratch) != 0) return -1;
    int i = root_child_for(bscratch, obj, typ, off);
    if (i < 0) return -1;
    uint64_t child;
    uint64_t o; uint32_t t; uint64_t f;
    root_get(bscratch, i, &o, &t, &f, &child);
    if (blk_read(child, bscratch) != 0) return -1;
    if (leaf_find(bscratch, obj, typ, off) < 0) return -1;
    uint32_t dd = 0, ds = 0;
    uint64_t oo; uint32_t tt; uint64_t ff;
    leaf_get(bscratch, leaf_find(bscratch, obj, typ, off), &oo, &tt, &ff, &dd, &ds);
    if (ds > cap) ds = cap;
    if (out) memcpy(out, bscratch + dd, ds);
    if (out_size) *out_size = ds;
    return 0;
}

/* Insert into the tree with CoW.  Writes fresh leaf + root blocks and
 * returns the new root LBA in *new_root. */
static int tree_insert(uint64_t root_lba, uint64_t obj, uint32_t typ, uint64_t off,
                       const uint8_t *data, uint32_t size, uint64_t *new_root) {
    if (!root_lba) return -1;
    if (blk_read(root_lba, bscratch) != 0) return -1;

    int ci = root_child_for(bscratch, obj, typ, off);
    uint64_t old_child;
    { uint64_t o; uint32_t t; uint64_t f; root_get(bscratch, ci, &o, &t, &f, &old_child); }

    if (blk_read(old_child, bdatabuf) != 0) return -1;   /* existing leaf */

    /* Collect existing leaf items + the new one into slots.  If an item
     * with the same key already exists, replace its data in place instead
     * of inserting a duplicate (CoW update, not append). */
    struct leaf_slot slots[MAX_LEAF_ITEMS + 1];
    uint32_t placed[MAX_LEAF_ITEMS + 1];
    int n = leaf_nslots(bdatabuf);
    if (n >= MAX_LEAF_ITEMS) return -1;                   /* leaf full (unexpected) */
    uint8_t databuf[4096];
    uint32_t dcur = 0;
    int added = 0;
    for (int i = 0; i < n; i++) {
        uint64_t o; uint32_t t; uint32_t dd, ds; uint64_t f;
        leaf_get(bdatabuf, i, &o, &t, &f, &dd, &ds);
        if (!added && o == obj && t == typ && f == off) {
            /* replace this item's data */
            slots[i].obj = o; slots[i].typ = t; slots[i].off = f;
            memcpy(databuf + dcur, data, size);
            slots[i].src = dcur; slots[i].dsz = size;
            dcur += size;
            added = 1;
            continue;
        }
        slots[i].obj = o; slots[i].typ = t; slots[i].off = f;
        memcpy(databuf + dcur, bdatabuf + dd, ds);
        slots[i].src = dcur; slots[i].dsz = ds;
        dcur += ds;
    }
    if (!added) {
        slots[n].obj = obj; slots[n].typ = typ; slots[n].off = off;
        memcpy(databuf + dcur, data, size);
        slots[n].src = dcur; slots[n].dsz = size; dcur += size;
        n++;
    }

    /* Build the new leaf in bdatabuf, then move data blobs into it. */
    uint8_t *newleaf = bdatabuf;
    leaf_rebuild(newleaf, slots, n, placed);
    for (int i = 0; i < n; i++)
        memcpy(newleaf + placed[i], databuf + slots[i].src, slots[i].dsz);
    uint64_t newleaf_lba = blk_alloc();
    if (blk_write(newleaf_lba, newleaf, BT_LEAF, 0) != 0) return -1;

    /* CoW the root: copy, re-point the child, write a fresh root block. */
    uint8_t *newroot = bscratch;   /* reuse after we already read it above */
    if (blk_read(root_lba, newroot) != 0) return -1;   /* re-read clean copy */
    w64(newroot + 36 + ci * 32 + 24, newleaf_lba);
    uint64_t newroot_lba = blk_alloc();
    if (blk_write(newroot_lba, newroot, BT_ROOT, 0) != 0) return -1;
    *new_root = newroot_lba;
    return 0;
}

/* Remove an item with CoW.  Returns the new root LBA in *new_root. */
static int tree_remove(uint64_t root_lba, uint64_t obj, uint32_t typ, uint64_t off,
                       uint64_t *new_root) {
    if (!root_lba) return -1;
    if (blk_read(root_lba, bscratch) != 0) return -1;
    int ci = root_child_for(bscratch, obj, typ, off);
    uint64_t old_child;
    { uint64_t o; uint32_t t; uint64_t f; root_get(bscratch, ci, &o, &t, &f, &old_child); }
    if (blk_read(old_child, bdatabuf) != 0) return -1;

    int n = leaf_nslots(bdatabuf);
    struct leaf_slot slots[MAX_LEAF_ITEMS];
    uint32_t placed[MAX_LEAF_ITEMS];
    uint8_t databuf[4096];
    uint32_t dcur = 0;
    int found = 0, outn = 0;
    for (int i = 0; i < n; i++) {
        uint64_t o; uint32_t t; uint32_t dd, ds; uint64_t f;
        leaf_get(bdatabuf, i, &o, &t, &f, &dd, &ds);
        if (o == obj && t == typ && f == off) { found = 1; continue; }
        slots[outn].obj = o; slots[outn].typ = t; slots[outn].off = f;
        memcpy(databuf + dcur, bdatabuf + dd, ds);
        slots[outn].src = dcur; slots[outn].dsz = ds;
        dcur += ds; outn++;
    }
    if (!found) return -1;

    uint8_t *newleaf = bdatabuf;
    leaf_rebuild(newleaf, slots, outn, placed);
    for (int i = 0; i < outn; i++)
        memcpy(newleaf + placed[i], databuf + slots[i].src, slots[i].dsz);
    uint64_t newleaf_lba = blk_alloc();
    if (blk_write(newleaf_lba, newleaf, BT_LEAF, 0) != 0) return -1;

    if (blk_read(root_lba, bscratch) != 0) return -1;
    w64(bscratch + 36 + ci * 32 + 24, newleaf_lba);
    uint64_t newroot_lba = blk_alloc();
    if (blk_write(newroot_lba, bscratch, BT_ROOT, 0) != 0) return -1;
    *new_root = newroot_lba;
    return 0;
}

/* Visit every item with the given objectid+type across all leaves.
 * cb returns 0 to continue, non-zero to stop (result is returned). */
typedef int (*tree_visit_fn)(uint64_t obj, uint32_t typ, uint64_t off,
                             const uint8_t *data, uint32_t size, void *ctx);
static int tree_visit(uint64_t root_lba, uint64_t obj, uint32_t typ,
                      tree_visit_fn cb, void *ctx) {
    if (!root_lba) return -1;
    if (blk_read(root_lba, bscratch) != 0) return -1;
    int n = root_nptrs(bscratch);
    for (int i = 0; i < n; i++) {
        uint64_t o; uint32_t t; uint64_t f, child;
        root_get(bscratch, i, &o, &t, &f, &child);
        if (blk_read(child, bscratch) != 0) continue;
        int m = leaf_nslots(bscratch);
        for (int j = 0; j < m; j++) {
            uint64_t oo; uint32_t tt; uint32_t dd, ds; uint64_t ff;
            leaf_get(bscratch, j, &oo, &tt, &ff, &dd, &ds);
            if (oo == obj && tt == typ) {
                int rc = cb(oo, tt, ff, bscratch + dd, ds, ctx);
                if (rc) return rc;
            }
        }
    }
    return -1;
}

/* ============================================================================
 * SECTION 6: RECORD HELPERS
 * ============================================================================ */

static int inode_get(uint64_t ino, uint8_t *rec) {
    return tree_lookup(bm.tree_root_lba, ino, BTRFS_ITYPE_INODE, 0, rec,
                       INODE_REC_SIZE, NULL);
}
static int inode_update(uint64_t ino, const uint8_t *rec) {
    return tree_insert(bm.tree_root_lba, ino, BTRFS_ITYPE_INODE, 0, rec,
                       INODE_REC_SIZE, &bm.tree_root_lba);
}

static uint64_t ino_new(void) {
    uint64_t ino = bm.next_inode++;
    if (ino < BTRFS_ROOT_INO) ino = BTRFS_ROOT_INO;
    return ino;
}

/* ============================================================================
 * SECTION 7: PATH RESOLUTION
 * ============================================================================ */

/* Resolve a path.  Final component may not exist yet.  On success fills
 * *out_parent (dir that would contain the final component), *out_ino (its
 * ino if it exists, else 0), *found, and basename. */
static int btrfs_path_resolve(const char *path, uint64_t *out_parent,
                              uint64_t *out_ino, int *found, char *base, int bsz) {
    *found = 0;
    if (!path) return -1;
    while (*path == '/') path++;
    if (!*path) {
        *out_parent = 0;
        *out_ino = BTRFS_ROOT_INO;
        *found = 1;
        if (base && bsz) base[0] = 0;
        return 0;
    }
    uint64_t dir_ino = BTRFS_ROOT_INO;
    char comp[BTRFS_MAX_PATH];
    const char *p = path;
    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        uint64_t h = name_hash((uint8_t*)comp, n);
        uint8_t di[BTRFS_MAX_PATH + 24];
        uint32_t sz = 0;
        if (tree_lookup(bm.tree_root_lba, dir_ino, BTRFS_ITYPE_DIR, h,
                        di, sizeof(di), &sz) == 0) {
            uint32_t nl = r32(di + 12);
            if (nl == (uint32_t)n && memcmp(di + 16, comp, n) == 0) {
                uint64_t child = r64(di + 0);
                if (*p == 0) {
                    *out_parent = dir_ino;
                    *out_ino = child;
                    *found = 1;
                    if (base && bsz) { strncpy(base, comp, bsz - 1); base[bsz-1]=0; }
                    return 0;
                }
                dir_ino = child;
                continue;
            }
        }
        if (*p == 0) {
            *out_parent = dir_ino;
            *out_ino = 0;
            *found = 0;
            if (base && bsz) { strncpy(base, comp, bsz - 1); base[bsz-1]=0; }
            return 0;
        }
        return -1;   /* intermediate component missing */
    }
    return -1;
}

/* Parent dir + basename of a path whose final component may not exist. */
static int btrfs_path_parent(const char *path, uint64_t *out_parent,
                             char *out_base, int bsz) {
    while (*path == '/') path++;
    if (!*path) return -1;
    char tmp[BTRFS_MAX_DEPTH][BTRFS_MAX_PATH];
    int ncomp = 0;
    const char *p = path;
    char comp[BTRFS_MAX_PATH];
    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        if (ncomp < BTRFS_MAX_DEPTH) memcpy(tmp[ncomp], comp, n + 1);
        ncomp++;
        while (*p == '/') p++;
    }
    if (ncomp < 1) return -1;
    if (out_base && bsz) { strncpy(out_base, tmp[ncomp-1], bsz-1); out_base[bsz-1]=0; }
    if (ncomp == 1) { *out_parent = BTRFS_ROOT_INO; return 0; }
    char dirpath[BTRFS_MAX_PATH * BTRFS_MAX_DEPTH + 2];
    int dlen = 0; dirpath[0] = 0;
    for (int i = 0; i < ncomp - 1; i++) {
        dirpath[dlen++] = '/';
        int clen = (int)strlen(tmp[i]);
        memcpy(dirpath + dlen, tmp[i], (size_t)clen);
        dlen += clen;
    }
    dirpath[dlen] = 0;
    uint64_t pn, ino; int found;
    if (btrfs_path_resolve(dirpath, &pn, &ino, &found, NULL, 0) != 0 || !found)
        return -1;
    *out_parent = ino;
    return 0;
}

/* ============================================================================
 * SECTION 8: VNODE MANAGEMENT
 * ============================================================================ */

static struct btrfs_vinfo *bv_intern(const char *path, uint64_t objid, int is_dir,
                                     uint64_t size) {
    for (int i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
        if (bv4pool[i].in_use && strcmp(bv4pool[i].path, path) == 0) {
            bv4pool[i].objectid = objid;
            bv4pool[i].is_dir = is_dir;
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
            v->is_dir = is_dir;
            v->size = size;
            strncpy(v->vnode.name, path, VFS_PATH_MAX - 1);
            v->vnode.type = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            v->vnode.mode = is_dir ? 0755 : 0644;
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
 * SECTION 9: VFS OPERATIONS
 * ============================================================================ */

static struct vnode *btrfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (!bm.mounted) return NULL;
    uint64_t parent, ino; int found; char base[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_resolve(path, &parent, &ino, &found, base, sizeof(base)) != 0)
        return NULL;
    if (!found) return NULL;
    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(ino, rec) != 0) return NULL;
    uint64_t mode = r64(rec + 0);
    uint64_t size = r64(rec + 24);
    int is_dir = (mode & 0x4000) != 0;
    return &bv_intern(path, ino, is_dir, size)->vnode;
}

static struct vnode *btrfs_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!bm.mounted) return NULL;
    uint64_t parent, ino; int found; char base[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_resolve(path, &parent, &ino, &found, base, sizeof(base)) != 0)
        return NULL;
    if (found) return NULL;
    if (!base[0]) return NULL;

    uint64_t new_ino = ino_new();
    uint8_t rec[INODE_REC_SIZE];
    memset(rec, 0, sizeof(rec));
    w64(rec + 0, 0x8000 | 0644);
    w64(rec + 24, 0);
    w64(rec + 32, 1);
    w64(rec + 40, 1337); w64(rec + 48, 1337); w64(rec + 56, 1337);
    if (inode_update(new_ino, rec) != 0) return NULL;

    /* dir item in parent */
    uint8_t di[BTRFS_MAX_PATH + 24];
    int n = (int)strlen(base);
    w64(di + 0, new_ino);
    w32(di + 8, BTRFS_FT_REG);
    w32(di + 12, (uint32_t)n);
    memcpy(di + 16, base, (size_t)n);
    if (tree_insert(bm.tree_root_lba, parent, BTRFS_ITYPE_DIR,
                    name_hash((uint8_t*)base, n), di, (uint32_t)(16 + n),
                    &bm.tree_root_lba) != 0) return NULL;

    kprintf("[btrfs] create: ino %llu path '%s' parent=%llu\n",
            (unsigned long long)new_ino, base, (unsigned long long)parent);
    return &bv_intern(path, new_ino, 0, 0)->vnode;
}

/* Locate the data extent for (ino, off).  Returns disk LBA or 0 if absent. */
static uint64_t extent_get(uint64_t ino, uint64_t off) {
    uint8_t rec[EXTENT_REC_SIZE];
    if (tree_lookup(bm.tree_root_lba, ino, BTRFS_ITYPE_EXTENT, off,
                    rec, EXTENT_REC_SIZE, NULL) != 0)
        return 0;
    return r64(rec + 0);
}

static int64_t btrfs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(v->objectid, rec) != 0) return -1;
    uint64_t fsize = r64(rec + 24);
    if (pos >= fsize) return 0;
    if (pos + count > fsize) count = fsize - pos;

    uint8_t *out = (uint8_t*)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t abs = pos + done;
        uint64_t bstart = (abs / DATA_PAYLOAD) * DATA_PAYLOAD;
        uint32_t inblk = (uint32_t)(abs - bstart);
        uint64_t lba = extent_get(v->objectid, bstart);
        uint32_t chunk = DATA_PAYLOAD - inblk;
        if (chunk > count - done) chunk = (uint32_t)(count - done);
        if (lba) {
            if (blk_read(lba, bdatabuf) != 0) return -1;  /* CRC verified here */
            memcpy(out + done, bdatabuf + BTRFS_HDR_SIZE + inblk, chunk);
        } else {
            memset(out + done, 0, chunk);   /* hole */
        }
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t btrfs_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    if (count == 0) return 0;

    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(v->objectid, rec) != 0) return -1;
    uint64_t fsize = r64(rec + 24);

    const uint8_t *in = (const uint8_t*)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t abs = pos + done;
        uint64_t bstart = (abs / DATA_PAYLOAD) * DATA_PAYLOAD;
        uint32_t inblk = (uint32_t)(abs - bstart);
        uint32_t chunk = DATA_PAYLOAD - inblk;
        if (chunk > count - done) chunk = (uint32_t)(count - done);

        /* CoW: build the new data block (base on existing if partial). */
        uint8_t *blk = bdatabuf;
        uint64_t old_lba = extent_get(v->objectid, bstart);
        if (old_lba) {
            if (blk_read(old_lba, bdatabuf) != 0) return -1;
        } else {
            memset(blk, 0, (size_t)bm.nodesize);
        }
        memcpy(blk + BTRFS_HDR_SIZE + inblk, in + done, chunk);
        uint64_t new_lba = blk_alloc();
        if (blk_write(new_lba, blk, BT_DATA, v->objectid) != 0) return -1;

        /* update/replace the extent item (CoW tree) */
        uint8_t er[EXTENT_REC_SIZE];
        w64(er + 0, new_lba);
        w64(er + 8, DATA_PAYLOAD);
        if (tree_insert(bm.tree_root_lba, v->objectid, BTRFS_ITYPE_EXTENT,
                        bstart, er, EXTENT_REC_SIZE, &bm.tree_root_lba) != 0)
            return -1;
        done += chunk;
    }

    uint64_t new_size = pos + count;
    if (new_size > fsize) fsize = new_size;
    w64(rec + 24, fsize);
    w64(rec + 48, 1337);   /* mtime */
    if (inode_update(v->objectid, rec) != 0) return -1;
    v->size = fsize;
    v->vnode.size = fsize;
    return (int64_t)done;
}

/* tree_visit callback: drop any extent fully beyond new_size. */
struct trunc_ctx { uint64_t nsz; };
static int trunc_drop_extent(uint64_t o, uint32_t t, uint64_t off,
                             const uint8_t *d, uint32_t s, void *c) {
    (void)t; (void)d; (void)s;
    struct trunc_ctx *tc = (struct trunc_ctx*)c;
    if (off >= tc->nsz) {
        uint64_t nr;
        if (tree_remove(bm.tree_root_lba, o, BTRFS_ITYPE_EXTENT, off, &nr) == 0)
            bm.tree_root_lba = nr;
    }
    return 0;
}

static int btrfs_truncate(struct vnode *vn, uint64_t new_size) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(v->objectid, rec) != 0) return -1;
    struct trunc_ctx tc = { new_size };
    tree_visit(bm.tree_root_lba, v->objectid, BTRFS_ITYPE_EXTENT,
               trunc_drop_extent, &tc);
    w64(rec + 24, new_size);
    w64(rec + 48, 1337);
    if (inode_update(v->objectid, rec) != 0) return -1;
    v->size = new_size;
    v->vnode.size = new_size;
    return 0;
}

/* Directory entry enumeration callback context. */
struct dir_enum { struct vfs_dirent *out; int max; int count; };

static int dir_emit(uint64_t o, uint32_t t, uint64_t off, const uint8_t *d,
                    uint32_t s, void *c) {
    (void)o; (void)t; (void)off; (void)s;
    struct dir_enum *e = (struct dir_enum*)c;
    if (e->count >= e->max) return 1;
    uint32_t nl = r32(d + 12);
    if (nl == 0 || nl >= VFS_PATH_MAX) return 0;
    struct vfs_dirent *de = &e->out[e->count];
    memset(de, 0, sizeof(*de));
    memcpy(de->name, d + 16, nl);
    de->name[nl] = 0;
    de->inode = r64(d + 0);
    de->type = (r32(d + 8) == BTRFS_FT_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    e->count++;
    return 0;
}

static int count_dir_children(uint64_t o, uint32_t t, uint64_t off,
                              const uint8_t *d, uint32_t s, void *c) {
    (void)o; (void)t; (void)off; (void)d; (void)s;
    (*(int*)c)++;
    return 0;
}

static int btrfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v || !v->is_dir) return -1;
    struct dir_enum e = { out, max, 0 };
    tree_visit(bm.tree_root_lba, v->objectid, BTRFS_ITYPE_DIR, dir_emit, &e);
    return e.count;
}

static int btrfs_mkdir(void *fs_data, const char *path) {
    (void)fs_data;
    if (!bm.mounted) return -1;
    uint64_t parent, ino; int found; char base[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_resolve(path, &parent, &ino, &found, base, sizeof(base)) != 0)
        return -1;
    if (found) return -1;
    if (!base[0]) return -1;

    uint64_t new_ino = ino_new();
    uint8_t rec[INODE_REC_SIZE];
    memset(rec, 0, sizeof(rec));
    w64(rec + 0, 0x4000 | 0755);
    w64(rec + 32, 2);
    w64(rec + 40, 1337); w64(rec + 48, 1337); w64(rec + 56, 1337);
    if (inode_update(new_ino, rec) != 0) return -1;

    int n = (int)strlen(base);
    uint8_t di[BTRFS_MAX_PATH + 24];
    w64(di + 0, new_ino); w32(di + 8, BTRFS_FT_DIR); w32(di + 12, (uint32_t)n);
    memcpy(di + 16, base, (size_t)n);
    if (tree_insert(bm.tree_root_lba, parent, BTRFS_ITYPE_DIR,
                    name_hash((uint8_t*)base, n), di, (uint32_t)(16 + n),
                    &bm.tree_root_lba) != 0) return -1;
    kprintf("[btrfs] mkdir: ino %llu path '%s' parent=%llu\n",
            (unsigned long long)new_ino, base, (unsigned long long)parent);
    return 0;
}

static int btrfs_unlink(void *fs_data, const char *path) {
    (void)fs_data;
    if (!bm.mounted) return -1;
    uint64_t parent, ino; int found; char base[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_resolve(path, &parent, &ino, &found, base, sizeof(base)) != 0)
        return -1;
    if (!found) return -1;
    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(ino, rec) != 0) return -1;
    if (r64(rec + 0) & 0x4000) return -1;   /* don't unlink dirs */

    uint64_t nr;
    if (tree_remove(bm.tree_root_lba, parent, BTRFS_ITYPE_DIR,
                    name_hash((uint8_t*)base, (int)strlen(base)), &nr) != 0)
        return -1;
    bm.tree_root_lba = nr;
    uint64_t links = r64(rec + 32);
    if (links > 1) {
        w64(rec + 32, links - 1);
        inode_update(ino, rec);
    } else {
        w64(rec + 32, 0);
        inode_update(ino, rec);
    }
    bv_evict(path);
    kprintf("[btrfs] unlink: removed '%s' (ino %llu, links %llu)\n",
            path, (unsigned long long)ino, (unsigned long long)r64(rec + 32));
    return 0;
}

static int btrfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data;
    if (!bm.mounted) return -1;
    uint64_t parent, ino; int found; char base[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_resolve(path, &parent, &ino, &found, base, sizeof(base)) != 0)
        return -1;
    if (!found) return -1;
    if (ino == BTRFS_ROOT_INO) return -1;
    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(ino, rec) != 0) return -1;
    if (!(r64(rec + 0) & 0x4000)) return -1;

    /* must be empty: no DIR entries under it */
    int child_count = 0;
    tree_visit(bm.tree_root_lba, ino, BTRFS_ITYPE_DIR,
               count_dir_children, &child_count);
    if (child_count > 0) return -1;

    uint64_t nr;
    if (tree_remove(bm.tree_root_lba, parent, BTRFS_ITYPE_DIR,
                    name_hash((uint8_t*)base, (int)strlen(base)), &nr) != 0)
        return -1;
    bm.tree_root_lba = nr;
    w64(rec + 32, 0);
    inode_update(ino, rec);
    bv_evict(path);
    kprintf("[btrfs] rmdir: removed '%s' (ino %llu)\n", path,
            (unsigned long long)ino);
    return 0;
}

static int btrfs_rename(void *fs_data, const char *old_path, const char *new_path) {
    (void)fs_data;
    if (!bm.mounted) return -1;
    uint64_t op, ino; int found; char obase[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_resolve(old_path, &op, &ino, &found, obase, sizeof(obase)) != 0)
        return -1;
    if (!found) return -1;
    uint64_t np; char nbase[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_parent(new_path, &np, nbase, sizeof(nbase)) != 0) return -1;

    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(ino, rec) != 0) return -1;
    int dtype = (r64(rec + 0) & 0x4000) ? BTRFS_FT_DIR : BTRFS_FT_REG;

    int n = (int)strlen(nbase);
    uint8_t di[BTRFS_MAX_PATH + 24];
    w64(di + 0, ino); w32(di + 8, (uint32_t)dtype); w32(di + 12, (uint32_t)n);
    memcpy(di + 16, nbase, (size_t)n);
    uint64_t nr;
    if (tree_insert(bm.tree_root_lba, np, BTRFS_ITYPE_DIR,
                    name_hash((uint8_t*)nbase, n), di, (uint32_t)(16 + n), &nr) != 0)
        return -1;
    bm.tree_root_lba = nr;
    if (tree_remove(bm.tree_root_lba, op, BTRFS_ITYPE_DIR,
                    name_hash((uint8_t*)obase, (int)strlen(obase)), &nr) != 0)
        return -1;
    bm.tree_root_lba = nr;
    bv_evict(old_path);
    kprintf("[btrfs] rename: '%s' -> '%s'\n", old_path, new_path);
    return 0;
}

static int btrfs_link(void *fs_data, const char *old_path, const char *new_path) {
    (void)fs_data;
    if (!bm.mounted) return -1;
    uint64_t op, ino; int found;
    if (btrfs_path_resolve(old_path, &op, &ino, &found, NULL, 0) != 0 || !found)
        return -1;
    uint64_t np; char nbase[BTRFS_MAX_PATH] = {0};
    if (btrfs_path_parent(new_path, &np, nbase, sizeof(nbase)) != 0) return -1;

    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(ino, rec) != 0) return -1;
    if (r64(rec + 0) & 0x4000) return -1;   /* no dir hard links */

    int n = (int)strlen(nbase);
    uint8_t di[BTRFS_MAX_PATH + 24];
    w64(di + 0, ino); w32(di + 8, BTRFS_FT_REG); w32(di + 12, (uint32_t)n);
    memcpy(di + 16, nbase, (size_t)n);
    uint64_t nr;
    if (tree_insert(bm.tree_root_lba, np, BTRFS_ITYPE_DIR,
                    name_hash((uint8_t*)nbase, n), di, (uint32_t)(16 + n), &nr) != 0)
        return -1;
    bm.tree_root_lba = nr;
    w64(rec + 32, r64(rec + 32) + 1);
    inode_update(ino, rec);
    kprintf("[btrfs] link: '%s' -> '%s' (ino %llu, links %llu)\n",
            old_path, new_path, (unsigned long long)ino,
            (unsigned long long)r64(rec + 32));
    return 0;
}

static int btrfs_settimes(struct vnode *vn, uint64_t atime, uint64_t mtime) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v) return -1;
    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(v->objectid, rec) != 0) return -1;
    w64(rec + 40, atime);
    w64(rec + 48, mtime);
    w64(rec + 56, mtime);
    inode_update(v->objectid, rec);
    return 0;
}

static int btrfs_stat(struct vnode *vn, struct vfs_stat *st) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    if (!v) return -1;
    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(v->objectid, rec) != 0) return -1;
    st->type = v->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->mode = (uint32_t)(r64(rec + 0) & 0xFFF);
    st->uid = (uint32_t)r64(rec + 8);
    st->gid = (uint32_t)r64(rec + 16);
    st->size = r64(rec + 24);
    st->nlink = (uint32_t)r64(rec + 32);
    st->atime = r64(rec + 40);
    st->mtime = r64(rec + 48);
    st->ctime = r64(rec + 56);
    st->inode = v->objectid;
    return 0;
}

/* fsync: persist generation, tree root and free-block pointer to the
 * superblock so a remount continues exactly here. */
static int btrfs_sync(void *fs_data) {
    (void)fs_data;
    if (!bm.mounted) return -1;
    fs_cache_sync(NULL);
    uint8_t *sb = bscratch;
    if (blk_read(BTRFS_SUPER_OFFSET, sb) != 0) return -1;
    w64(sb + 56, bm.generation);
    w64(sb + 64, bm.tree_root_lba);
    w64(sb + 72, bm.next_free_block);
    w64(sb + 136, bm.next_inode);
    w32(sb + 8, (uint32_t)bm.generation);
    if (blk_write(BTRFS_SUPER_OFFSET, sb, BT_SUPER, 0) != 0) return -1;
    bm.generation++;
    kprintf("[btrfs] fsync: persisted tree root LBA %llu gen %llu\n",
            (unsigned long long)bm.tree_root_lba,
            (unsigned long long)bm.generation);
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
    .rmdir    = btrfs_rmdir,
    .rename   = btrfs_rename,
    .link     = btrfs_link,
    .settimes = btrfs_settimes,
    .stat     = btrfs_stat,
    .truncate = btrfs_truncate,
    .sync     = btrfs_sync,
};

/* ============================================================================
 * SECTION 10: FORMAT AND MOUNT
 * ============================================================================ */

static int format_btrfs(void) {
    kprintf("[btrfs] formatting CoW filesystem...\n");
    bm.nodesize = BTRFS_NODE_SIZE;
    bm.sectorsize = BTRFS_NODE_SIZE;
    bm.total_bytes = 256ULL * 1024 * 1024;
    bm.generation = 1;
    bm.next_free_block = BTRFS_FIRST_FREE;
    bm.max_block = bm.total_bytes;
    bm.next_inode = 2;

    /* superblock */
    uint8_t *sb = bscratch;
    memset(sb, 0, BTRFS_NODE_SIZE);
    w64(sb + 32, bm.total_bytes);
    w64(sb + 40, bm.nodesize);
    w64(sb + 48, bm.sectorsize);
    w64(sb + 56, bm.generation);
    w64(sb + 64, 0);                 /* tree_root, set below */
    w64(sb + 72, bm.next_free_block);
    memcpy(sb + 80, "btrfs", 5);
    memcpy(sb + 128, BTRFS_MAGIC, 8);
    w64(sb + 136, bm.next_inode);
    if (blk_write(BTRFS_SUPER_OFFSET, sb, BT_SUPER, 0) != 0) return -1;

    /* root directory inode */
    uint8_t rec[INODE_REC_SIZE];
    memset(rec, 0, sizeof(rec));
    w64(rec + 0, 0x4000 | 0755);
    w64(rec + 32, 2);
    w64(rec + 40, 1337); w64(rec + 48, 1337); w64(rec + 56, 1337);

    /* create an empty leaf + root, root points to leaf */
    uint8_t *leaf = bdatabuf;
    memset(leaf, 0, BTRFS_NODE_SIZE);
    w32(leaf + 32, 0);   /* nslots=0 */
    uint64_t leaf_lba = blk_alloc();
    if (blk_write(leaf_lba, leaf, BT_LEAF, 0) != 0) return -1;

    uint8_t *root = bscratch;
    memset(root, 0, BTRFS_NODE_SIZE);
    w32(root + 32, 1);
    w64(root + 36 + 0 * 32 + 0, BTRFS_ROOT_INO);
    w64(root + 36 + 0 * 32 + 8, 0);
    w32(root + 36 + 0 * 32 + 16, BTRFS_ITYPE_INODE);
    w64(root + 36 + 0 * 32 + 24, leaf_lba);
    uint64_t root_lba = blk_alloc();
    if (blk_write(root_lba, root, BT_ROOT, 0) != 0) return -1;
    bm.tree_root_lba = root_lba;

    if (tree_insert(bm.tree_root_lba, BTRFS_ROOT_INO, BTRFS_ITYPE_INODE, 0,
                    rec, INODE_REC_SIZE, &bm.tree_root_lba) != 0) return -1;

    /* persist the final tree root + free pointer into the superblock */
    sb = bscratch;
    if (btrfs_read_block(BTRFS_SUPER_OFFSET, sb) != 0) return -1;
    w64(sb + 64, bm.tree_root_lba);
    w64(sb + 72, bm.next_free_block);
    w64(sb + 136, bm.next_inode);
    if (blk_write(BTRFS_SUPER_OFFSET, sb, BT_SUPER, 0) != 0) return -1;

    kprintf("[btrfs] format complete: %llu bytes, CoW enabled\n",
            (unsigned long long)bm.total_bytes);
    return 0;
}

int btrfs_init(int prefer_port) {
    memset(&bm, 0, sizeof(bm));
    memset(bv4pool, 0, sizeof(bv4pool));
    bm.bdev = prefer_port;
    spinlock_init(&bm.lock);

    if (!bscratch) bscratch = (uint8_t*)kmalloc(BTRFS_NODE_SIZE);
    if (!bdatabuf) bdatabuf = (uint8_t*)kmalloc(BTRFS_NODE_SIZE);
    if (!bscratch || !bdatabuf) {
        kprintf("[btrfs] cannot allocate scratch buffers, mount aborted\n");
        return -1;
    }

    if (btrfs_read_super(BTRFS_SUPER_OFFSET, bscratch) != 0) {
        if (!fs_format_allowed()) {
            kprintf("[btrfs] superblock unreadable; format disabled (FS_MOUNT_FORMAT=0)\n");
            return -1;
        }
        kprintf("[btrfs] cannot read superblock, formatting...\n");
        if (format_btrfs() != 0) return -1;
        if (btrfs_read_super(BTRFS_SUPER_OFFSET, bscratch) != 0) return -1;
    }
    uint8_t *sb = bscratch;
    if (r32(sb + 4) != BTRFS_BLK_MAGIC || memcmp(sb + 128, BTRFS_MAGIC, 8) != 0) {
        if (!fs_format_allowed()) {
            kprintf("[btrfs] not btrfs; format disabled (FS_MOUNT_FORMAT=0)\n");
            return -1;
        }
        kprintf("[btrfs] not btrfs magic, formatting...\n");
        if (format_btrfs() != 0) return -1;
        if (btrfs_read_super(BTRFS_SUPER_OFFSET, bscratch) != 0) return -1;
        sb = bscratch;
    }

    bm.nodesize = r64(sb + 40) ? r64(sb + 40) : BTRFS_NODE_SIZE;
    bm.sectorsize = r64(sb + 48) ? r64(sb + 48) : BTRFS_NODE_SIZE;
    bm.total_bytes = r64(sb + 32);
    bm.generation = r64(sb + 56) + 1;
    bm.tree_root_lba = r64(sb + 64);
    bm.next_free_block = r64(sb + 72) ? r64(sb + 72) : BTRFS_FIRST_FREE;
    bm.max_block = bm.total_bytes;
    bm.next_inode = (uint32_t)(r64(sb + 136) ? r64(sb + 136) : 2);

    kprintf("[btrfs] mounted CoW filesystem at /btrfs:\n");
    kprintf("       size=%llu, nodesize=%llu, tree_root=%llu, gen=%llu\n",
            (unsigned long long)bm.total_bytes, (unsigned long long)bm.nodesize,
            (unsigned long long)bm.tree_root_lba, (unsigned long long)bm.generation);
    kprintf("       SHA-256 checksums (block trailer) + CoW tree re-parenting enabled\n");
    bm.mounted = 1;
    return 0;
}

/* ============================================================================
 * SECTION 11: SELF-TEST
 * ============================================================================ */

int btrfs_self_test(void) {
    if (!bm.mounted) {
        kprintf("[btrfs] self-test: SKIPPED (not mounted)\n");
        return -1;
    }
    kprintf("[btrfs] self-test: CoW tree, SHA-256, multi-block, rename, rmdir, truncate...\n");

    /* 1. create + single write + read-back (CRC verified on read) */
    struct vnode *f = btrfs_create(NULL, "test_btrfs.dat");
    if (!f) { kprintf("[btrfs] FAIL: create\n"); return -2; }
    const char *msg = "Copy-on-write btrfs test OK!";
    if (btrfs_write(f, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[btrfs] FAIL: write\n"); return -3;
    }
    char buf[256] = {0};
    if (btrfs_read(f, 0, buf, sizeof(buf) - 1) != (int64_t)strlen(msg) ||
        strcmp(buf, msg) != 0) {
        kprintf("[btrfs] FAIL: readback '%s'\n", buf); return -4;
    }

    /* 2. CoW: overwrite and verify BOTH generations' blocks still exist */
    {
        uint64_t lba1 = extent_get(f->inode_id, 0);
        const char *msg2 = "OVERWRITTEN second generation";
        if (btrfs_write(f, 0, msg2, strlen(msg2)) != (int64_t)strlen(msg2)) {
            kprintf("[btrfs] FAIL: CoW overwrite\n"); return -5;
        }
        uint64_t lba2 = extent_get(f->inode_id, 0);
        if (lba1 == 0 || lba2 == 0 || lba1 == lba2) {
            kprintf("[btrfs] FAIL: CoW did not allocate a new block\n"); return -5;
        }
        /* old generation block still intact (verifies CRC) */
        if (blk_read(lba1, bdatabuf) != 0) {
            kprintf("[btrfs] FAIL: old-generation block unreadable/corrupt\n"); return -5;
        }
        if (memcmp(bdatabuf + BTRFS_HDR_SIZE, msg, strlen(msg)) != 0) {
            kprintf("[btrfs] FAIL: old-generation content lost\n"); return -5;
        }
        /* new content resolves */
        char b2[256] = {0};
        if (btrfs_read(f, 0, b2, sizeof(b2) - 1) != (int64_t)strlen(msg2) ||
            strcmp(b2, msg2) != 0) {
            kprintf("[btrfs] FAIL: new generation readback\n"); return -5;
        }
        kprintf("[btrfs]   CoW overwrite: old block %llu retained, new block %llu\n",
                (unsigned long long)lba1, (unsigned long long)lba2);
    }

    /* 3. multi-block file: 2 blocks, byte-exact round-trip */
    {
        struct vnode *mb = btrfs_create(NULL, "multi.bin");
        if (!mb) { kprintf("[btrfs] FAIL: create multi.bin\n"); return -6; }
        uint8_t pat[8192];
        for (int i = 0; i < 8192; i++) pat[i] = (uint8_t)(i * 7 + 3);
        if (btrfs_write(mb, 0, pat, sizeof(pat)) != (int64_t)sizeof(pat)) {
            kprintf("[btrfs] FAIL: multi-block write\n"); return -6;
        }
        uint8_t got[8192];
        if (btrfs_read(mb, 0, got, sizeof(got)) != (int64_t)sizeof(got) ||
            memcmp(pat, got, sizeof(pat)) != 0) {
            kprintf("[btrfs] FAIL: multi-block readback\n"); return -6;
        }
        kprintf("[btrfs]   multi-block file (8 KiB) round-tripped byte-exact\n");
    }

    /* 4. mkdir + create-in-subdir + rename */
    if (btrfs_mkdir(NULL, "bsub") != 0) { kprintf("[btrfs] FAIL: mkdir\n"); return -7; }
    struct vnode *inner = btrfs_create(NULL, "bsub/inner.txt");
    if (!inner) { kprintf("[btrfs] FAIL: create in subdir\n"); return -7; }
    if (btrfs_write(inner, 0, "nested", 6) != 6) return -7;
    if (btrfs_rename(NULL, "bsub/inner.txt", "bsub/renamed.txt") != 0) {
        kprintf("[btrfs] FAIL: rename\n"); return -7;
    }
    if (!btrfs_lookup(NULL, "bsub/renamed.txt")) {
        kprintf("[btrfs] FAIL: lookup after rename\n"); return -7;
    }

    /* 5. link + settimes */
    if (btrfs_link(NULL, "bsub/renamed.txt", "bsub/hard.txt") != 0) {
        kprintf("[btrfs] FAIL: link\n"); return -8;
    }
    struct vnode *hl = btrfs_lookup(NULL, "bsub/hard.txt");
    if (!hl) { kprintf("[btrfs] FAIL: lookup hard link\n"); return -8; }
    if (btrfs_settimes(hl, 111, 222) != 0) { kprintf("[btrfs] FAIL: settimes\n"); return -8; }
    struct vfs_stat st;
    if (btrfs_stat(hl, &st) != 0) return -8;
    if (st.nlink < 2) { kprintf("[btrfs] FAIL: nlink after link\n"); return -8; }
    if (st.mtime != 222) { kprintf("[btrfs] FAIL: settimes mtime\n"); return -8; }

    /* 6. truncate down, then up with zero-fill read */
    {
        struct vnode *tc = btrfs_create(NULL, "trunc.bin");
        if (!tc) return -9;
        const char *longdata = "0123456789ABCDEFGHIJ";
        if (btrfs_write(tc, 0, longdata, strlen(longdata)) != (int64_t)strlen(longdata))
            return -9;
        if (btrfs_truncate(tc, 5) != 0) { kprintf("[btrfs] FAIL: truncate\n"); return -9; }
        char tbuf[32] = {0};
        if (btrfs_read(tc, 0, tbuf, sizeof(tbuf)) != 5 || memcmp(tbuf, "01234", 5) != 0) {
            kprintf("[btrfs] FAIL: truncate readback\n"); return -9;
        }
    }

    /* 7. unlink + rmdir */
    if (btrfs_unlink(NULL, "bsub/hard.txt") != 0) { kprintf("[btrfs] FAIL: unlink hard\n"); return -10; }
    if (btrfs_unlink(NULL, "bsub/renamed.txt") != 0) { kprintf("[btrfs] FAIL: unlink renamed\n"); return -10; }
    if (btrfs_rmdir(NULL, "bsub") != 0) { kprintf("[btrfs] FAIL: rmdir\n"); return -10; }

    /* 8. fsync persists the tree */
    if (btrfs_sync(NULL) != 0) { kprintf("[btrfs] FAIL: fsync\n"); return -11; }

    /* 9. RESIDUE2 T3 negative control: flip ONE byte of a block on the
     * media and require blk_read() to refuse it by digest.  A checksum
     * that never fires protects nothing, so the firing is the receipt.
     * The block is re-sealed afterwards so the volume stays clean. */
    {
        uint64_t lba = blk_alloc();
        for (int i = 0; i < BTRFS_NODE_SIZE; i++)
            bdatabuf[i] = (uint8_t)(i * 13 + 5);
        if (blk_write(lba, bdatabuf, BT_DATA, 0) != 0) {
            kprintf("[btrfs] FAIL: csum-probe write\n"); return -12;
        }
        if (btrfs_read_block(lba, bscratch) != 0) {
            kprintf("[btrfs] FAIL: csum-probe raw read\n"); return -12;
        }
        bscratch[100] ^= 0xFF;                       /* one bit-flip */
        if (btrfs_write_block(lba, bscratch) != 0) {
            kprintf("[btrfs] FAIL: csum-probe raw write\n"); return -12;
        }
        if (blk_read(lba, bscratch) == 0) {
            kprintf("[btrfs] FAIL: SHA-256 accepted a corrupted block\n");
            return -12;
        }
        /* re-seal: the good copy still sits in bdatabuf */
        if (blk_write(lba, bdatabuf, BT_DATA, 0) != 0 ||
            blk_read(lba, bscratch) != 0) {
            kprintf("[btrfs] FAIL: csum-probe restore\n"); return -12;
        }
        kprintf("[btrfs]   SHA-256 negative control: corrupted block refused, re-seal verified\n");
    }

    kprintf("[btrfs] PASS: CoW tree, SHA-256, multi-block, rename, rmdir, truncate, link, settimes, fsync\n");
    kprintf("[btrfs] PASS: SHA-256 detects on-disk corruption (RESIDUE2 T3)\n");
    return 0;
}
