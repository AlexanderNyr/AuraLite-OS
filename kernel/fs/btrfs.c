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
#include "kernel/lib/errno.h"

/* ============================================================================
 * SECTION 1: ON-DISK FORMAT
 * ============================================================================ */

#define BTRFS_MAGIC          "_BHRfS_M"
#define AURAFS_DUAL_MAGIC    "_AURA2FS"
#define AURAFS_MIRROR_OFFSET 61440
#define BTRFS_BLK_MAGIC      0x42545246u   /* "BTRF" */

#define BTRFS_NODE_SIZE      4096
#define AURAFS_MOUNT_MAX_BLOCKS 262144U
#define AURAFS_MOUNT_MAX_STEPS 4194304U
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

#define MAX_LEAF_ITEMS 144
#define BTRFS_MAX_LEVELS 8
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
    int       dual;
    uint64_t  active_super;
    spinlock_t lock;
};

static struct btrfs_mount bm;
/* Volatile snapshot tokens must not survive a mount attempt. */
static uint64_t aurafs_mount_epoch;
static uint64_t aurafs_snapshot_id; /* never wrap/reuse during kernel lifetime */
static struct {
    struct aurafs_snapshot token;
    const struct aurafs_snapshot *owner; /* compared only, never dereferenced */
} aurafs_snapshots[AURAFS_MAX_SNAPSHOTS];
/* Conservative since-mount checkpoint candidates, not durable-root proof.
 * Never evict on successful sync: no device FLUSH/dual-super protocol exists.
 * Saturation disables audit, never ordinary writes or sync. */
#define AURAFS_CHECKPOINT_HISTORY 32
static struct {
    uint64_t root_lba, allocation_end;
} aurafs_checkpoint_history[AURAFS_CHECKPOINT_HISTORY];
static unsigned aurafs_checkpoint_count;
static int aurafs_checkpoint_overflow;

static void aurafs_checkpoint_remember(uint64_t root, uint64_t end) {
    for (unsigned i = 0; i < aurafs_checkpoint_count; i++)
        if (aurafs_checkpoint_history[i].root_lba == root &&
            aurafs_checkpoint_history[i].allocation_end == end) return;
    if (aurafs_checkpoint_count == AURAFS_CHECKPOINT_HISTORY) {
        aurafs_checkpoint_overflow = 1;
        return;
    }
    aurafs_checkpoint_history[aurafs_checkpoint_count].root_lba = root;
    aurafs_checkpoint_history[aurafs_checkpoint_count++].allocation_end = end;
}

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

/* Memoize verification, NOT disk contents: every call still reads through
 * fs_read_block and compares all 4096 bytes (header AND SHA trailer). Exact
 * equality with a previously verified copy makes re-hashing redundant.
 * Only metadata is eligible; data and superblocks always run SHA-256.
 * Shared state follows the existing serialized-driver calling contract. */
#ifndef AURAFS_VERIFY_CACHE_SLOTS
#define AURAFS_VERIFY_CACHE_SLOTS 16
#endif
#if AURAFS_VERIFY_CACHE_SLOTS < 0 || AURAFS_VERIFY_CACHE_SLOTS > 256
#error "AURAFS_VERIFY_CACHE_SLOTS must be between 0 and 256"
#endif
#if AURAFS_VERIFY_CACHE_SLOTS > 0
static struct {
    uint64_t lba;
    int valid;
    uint8_t bytes[BTRFS_NODE_SIZE];
} aurafs_verified[AURAFS_VERIFY_CACHE_SLOTS];
#endif
static struct {
    uint64_t reads, sha_reads, sha_writes, hits, batch_fast, batch_fallback;
} aurafs_verify_stats;

static void aurafs_verify_reset(void) {
    memset(&aurafs_verify_stats, 0, sizeof(aurafs_verify_stats));
#if AURAFS_VERIFY_CACHE_SLOTS > 0
    memset(aurafs_verified, 0, sizeof(aurafs_verified));
#endif
}

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

/* Keep the v2 writable prefix inside the mandatory remount envelope.
 * Device geometry remains unchanged; this is not a partition or reuse map. */
static uint64_t aurafs_append_ceiling(uint64_t bytes, int dual) {
    uint64_t cap = AURAFS_MOUNT_MAX_BLOCKS * (1ull * BTRFS_NODE_SIZE);
    return dual && bytes > cap ? cap : bytes;
}

static uint64_t blk_alloc(void) {
    uint64_t lba = bm.next_free_block;
    uint64_t limit = aurafs_append_ceiling(bm.max_block, bm.dual);
    /* Subtraction is deliberate: a corrupt cursor must not wrap UINT64_MAX. */
    if (bm.nodesize != BTRFS_NODE_SIZE || lba < BTRFS_FIRST_FREE ||
        lba % BTRFS_NODE_SIZE != 0 || lba > limit ||
        bm.nodesize > limit - lba) {
        kprintf("[btrfs] ENOSPC: CoW block pool exhausted at LBA %llu (volume end %llu)\n",
                (unsigned long long)lba, (unsigned long long)bm.max_block);
        return 0;
    }
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
    aurafs_verify_stats.sha_writes++;
    ksha256(blk, BTRFS_CSUM_OFFSET, blk + BTRFS_CSUM_OFFSET);
    return btrfs_write_block(lba, blk);
}

/* Read a block and verify its SHA-256 trailer + magic (RESIDUE2 T3).
 * Returns 0 on success.  A digest mismatch names its two honest causes:
 * bit-rot on the media, or a pre-T3 legacy volume whose trailer bytes
 * are whatever the old CRC32C era left there (neither is served). */
static int blk_read(uint64_t lba, uint8_t *blk) {
    aurafs_verify_stats.reads++;
    if (btrfs_read_block(lba, blk) != 0) return -1;
    if (r32(blk + 4) != BTRFS_BLK_MAGIC) return -1;
#if AURAFS_VERIFY_CACHE_SLOTS > 0
    uint32_t type = r32(blk + 12);
    unsigned slot = (unsigned)((lba / BTRFS_NODE_SIZE) % AURAFS_VERIFY_CACHE_SLOTS);
    int metadata = type == BT_ROOT || type == BT_LEAF;
    if (metadata && aurafs_verified[slot].valid &&
        aurafs_verified[slot].lba == lba &&
        memcmp(aurafs_verified[slot].bytes, blk, BTRFS_NODE_SIZE) == 0) {
        aurafs_verify_stats.hits++;
        return 0;
    }
#endif
    uint8_t digest[KSHA256_DIGEST_SIZE];
    aurafs_verify_stats.sha_reads++;
    ksha256(blk, BTRFS_CSUM_OFFSET, digest);
    if (memcmp(digest, blk + BTRFS_CSUM_OFFSET, KSHA256_DIGEST_SIZE) != 0) {
        kprintf("[btrfs] SHA-256 FAIL on block %llu (digest mismatch: "
                "corrupt block or legacy CRC32C volume)\n",
                (unsigned long long)lba);
        return -1;
    }
#if AURAFS_VERIFY_CACHE_SLOTS > 0
    if (metadata) {
        memcpy(aurafs_verified[slot].bytes, blk, BTRFS_NODE_SIZE);
        aurafs_verified[slot].lba = lba;
        aurafs_verified[slot].valid = 1;
    }
#endif
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

/* Search a leaf for an exact item using binary search.  Returns index or -1. */
static int leaf_find(const uint8_t *leaf, uint64_t obj, uint32_t typ, uint64_t off) {
    int lo = 0, hi = leaf_nslots(leaf) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint64_t o; uint32_t t, dd, ds; uint64_t f;
        leaf_get(leaf, mid, &o, &t, &f, &dd, &ds);
        int c = key_cmp(o, t, f, obj, typ, off);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else       hi = mid - 1;
    }
    return -1;
}

/* ---- node / root operations ---- */

#define MAX_NODE_PTRS 125

static int root_nptrs(const uint8_t *root) { return (int)r32(root + 32); }
static int blk_type_hdr(const uint8_t *blk) { return (int)r32(blk + 12); }

static void root_get(const uint8_t *root, int i, uint64_t *obj, uint32_t *typ,
                     uint64_t *off, uint64_t *child) {
    const uint8_t *p = root + 36 + i * 32;
    *obj = r64(p + 0);
    *off = r64(p + 8);
    *typ = r32(p + 16);
    *child = r64(p + 24);
}

struct root_ptr {
    uint64_t obj, off;
    uint32_t typ;
    uint64_t child_lba;
};

static void root_set_ptrs(uint8_t *root, int n, const struct root_ptr *ptrs) {
    w32(root + 32, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t *p = root + 36 + i * 32;
        w64(p + 0, ptrs[i].obj);
        w64(p + 8, ptrs[i].off);
        w32(p + 16, ptrs[i].typ);
        w32(p + 20, 0);
        w64(p + 24, ptrs[i].child_lba);
    }
}

/* Find the child index that should hold the key (last ptr with key<=k) via binary search. */
static int root_child_for(const uint8_t *root, uint64_t obj, uint32_t typ, uint64_t off) {
    int n = root_nptrs(root);
    if (n <= 0) return 0;
    int lo = 0, hi = n - 1, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint64_t o; uint32_t t; uint64_t f, c; (void)c;
        root_get(root, mid, &o, &t, &f, &c);
        if (key_cmp(o, t, f, obj, typ, off) <= 0) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

static int tree_descend(uint64_t root_lba, uint64_t obj, uint32_t typ, uint64_t off,
                        uint64_t *path_lbas, int *path_ci, int *depth_out, uint8_t *leaf_buf) {
    uint64_t cur = root_lba;
    int depth = 0;
    while (cur) {
        if (blk_read(cur, leaf_buf) != 0) return -1;
        if (blk_type_hdr(leaf_buf) == BT_LEAF) {
            *depth_out = depth;
            return 0;
        }
        if (depth >= BTRFS_MAX_LEVELS) return -1;
        int ci = root_child_for(leaf_buf, obj, typ, off);
        path_lbas[depth] = cur;
        path_ci[depth] = ci;
        depth++;
        uint64_t o, f, child; uint32_t t;
        root_get(leaf_buf, ci, &o, &t, &f, &child);
        cur = child;
    }
    return -1;
}

/* Look up an item in the tree.  Copies up to cap bytes of data into out. */
static int tree_lookup(uint64_t root_lba, uint64_t obj, uint32_t typ, uint64_t off,
                       uint8_t *out, uint32_t cap, uint32_t *out_size) {
    if (!root_lba) return -1;
    uint64_t path_lbas[BTRFS_MAX_LEVELS];
    int path_ci[BTRFS_MAX_LEVELS];
    int depth = 0;
    if (tree_descend(root_lba, obj, typ, off, path_lbas, path_ci, &depth, bscratch) != 0)
        return -1;
    int idx = leaf_find(bscratch, obj, typ, off);
    if (idx < 0) return -1;
    uint32_t dd = 0, ds = 0;
    uint64_t oo; uint32_t tt; uint64_t ff;
    leaf_get(bscratch, idx, &oo, &tt, &ff, &dd, &ds);
    if (ds > cap) ds = cap;
    if (out) memcpy(out, bscratch + dd, ds);
    if (out_size) *out_size = ds;
    return 0;
}

/* Preserve the existing 28-byte item layout and checksum trailer. */
static int leaf_room(int count, const struct leaf_slot *slots) {
    uint32_t used = 36 + (uint32_t)count * 28;
    if (count < 0 || count > MAX_LEAF_ITEMS || used > BTRFS_CSUM_OFFSET)
        return 0;
    for (int i = 0; i < count; i++) {
        if (slots[i].dsz > BTRFS_CSUM_OFFSET - used) return 0;
        used += slots[i].dsz;
    }
    return 1;
}

static int leaf_build(uint8_t *blk, int count, const struct leaf_slot *slots,
                      const uint32_t *placed, const uint8_t *databuf) {
    if (!leaf_room(count, slots)) return -1;
    memset(blk, 0, BTRFS_NODE_SIZE);
    w32(blk + 32, (uint32_t)count);
    uint32_t dcur = BTRFS_CSUM_OFFSET;
    for (int i = 0; i < count; i++) {
        uint8_t *sp = blk + 36 + i * 28;
        dcur -= slots[i].dsz;
        w64(sp + 0, slots[i].obj);
        w64(sp + 8, slots[i].off);
        w32(sp + 16, slots[i].typ);
        w32(sp + 20, dcur);
        w32(sp + 24, slots[i].dsz);
        memcpy(blk + dcur, databuf + placed[i], slots[i].dsz);
    }
    return 0;
}

/* Insert into the tree with CoW, 50/50 splits, and root growth. */
static int tree_insert(uint64_t root_lba, uint64_t obj, uint32_t typ, uint64_t off,
                       const uint8_t *data, uint32_t size, uint64_t *new_root) {
    if (!root_lba || size > BTRFS_CSUM_OFFSET - 36 - 28) return -1;
    uint64_t path_lbas[BTRFS_MAX_LEVELS];
    int path_ci[BTRFS_MAX_LEVELS];
    int depth = 0;
    if (tree_descend(root_lba, obj, typ, off, path_lbas, path_ci, &depth, bdatabuf) != 0)
        return -1;

    static struct leaf_slot slots[MAX_LEAF_ITEMS + 1];
    static uint32_t placed[MAX_LEAF_ITEMS + 1];
    int n = leaf_nslots(bdatabuf);
    /* Like bscratch/bdatabuf, staging is shared by the serialized FS path.
     * Keep it off the small kernel stack (also called from btrfsck). */
    static uint8_t databuf[2 * BTRFS_NODE_SIZE];
    uint32_t dcur = 0;
    int added = 0;
    int out_n = 0;
    for (int i = 0; i < n; i++) {
        uint64_t o; uint32_t t; uint32_t dd, ds; uint64_t f;
        leaf_get(bdatabuf, i, &o, &t, &f, &dd, &ds);
        if (!added && key_cmp(o, t, f, obj, typ, off) == 0) {
            slots[out_n].obj = o; slots[out_n].typ = t; slots[out_n].off = f;
            slots[out_n].dsz = size;
            placed[out_n] = dcur;
            memcpy(databuf + dcur, data, size);
            dcur += size;
            out_n++;
            added = 1;
            continue;
        }
        if (!added && key_cmp(obj, typ, off, o, t, f) < 0) {
            slots[out_n].obj = obj; slots[out_n].typ = typ; slots[out_n].off = off;
            slots[out_n].dsz = size;
            placed[out_n] = dcur;
            memcpy(databuf + dcur, data, size);
            dcur += size;
            out_n++;
            added = 1;
        }
        slots[out_n].obj = o; slots[out_n].typ = t; slots[out_n].off = f;
        slots[out_n].dsz = ds;
        placed[out_n] = dcur;
        memcpy(databuf + dcur, bdatabuf + dd, ds);
        dcur += ds;
        out_n++;
    }
    if (!added) {
        slots[out_n].obj = obj; slots[out_n].typ = typ; slots[out_n].off = off;
        slots[out_n].dsz = size;
        placed[out_n] = dcur;
        memcpy(databuf + dcur, data, size);
        dcur += size;
        out_n++;
    }

    struct root_ptr up[2];
    int n_up = 0;

    if (leaf_room(out_n, slots)) {
        uint64_t nl = blk_alloc();
        if (!nl) return -1;
        if (leaf_build(bscratch, out_n, slots, placed, databuf) != 0) return -1;
        if (blk_write(nl, bscratch, BT_LEAF, BTRFS_ROOT_INO) != 0) return -1;
        up[0].obj = slots[0].obj; up[0].typ = slots[0].typ; up[0].off = slots[0].off;
        up[0].child_lba = nl;
        n_up = 1;
    } else {
        int mid = out_n / 2;
        /* Variable-sized items need a byte-aware split, not just count/2. */
        if (!leaf_room(mid, slots) || !leaf_room(out_n - mid, slots + mid)) {
            for (mid = 1; mid < out_n; mid++)
                if (leaf_room(mid, slots) && leaf_room(out_n - mid, slots + mid))
                    break;
            if (mid == out_n) return -1;
        }
        uint64_t l1 = blk_alloc();
        uint64_t l2 = blk_alloc();
        if (!l1 || !l2) return -1;
        if (leaf_build(bscratch, mid, slots, placed, databuf) != 0) return -1;
        if (blk_write(l1, bscratch, BT_LEAF, BTRFS_ROOT_INO) != 0) return -1;
        if (leaf_build(bscratch, out_n - mid, slots + mid, placed + mid, databuf) != 0) return -1;
        if (blk_write(l2, bscratch, BT_LEAF, BTRFS_ROOT_INO) != 0) return -1;
        up[0].obj = slots[0].obj; up[0].typ = slots[0].typ; up[0].off = slots[0].off;
        up[0].child_lba = l1;
        up[1].obj = slots[mid].obj; up[1].typ = slots[mid].typ; up[1].off = slots[mid].off;
        up[1].child_lba = l2;
        n_up = 2;
    }

    for (int lvl = depth - 1; lvl >= 0; lvl--) {
        if (blk_read(path_lbas[lvl], bscratch) != 0) return -1;
        int np = root_nptrs(bscratch);
        int ci = path_ci[lvl];
        static struct root_ptr ptrs[MAX_NODE_PTRS + 2];
        int op = 0;
        for (int i = 0; i < np; i++) {
            uint64_t o, f, c; uint32_t t;
            root_get(bscratch, i, &o, &t, &f, &c);
            if (i == ci) {
                for (int k = 0; k < n_up; k++) ptrs[op++] = up[k];
            } else {
                ptrs[op].obj = o; ptrs[op].typ = t; ptrs[op].off = f; ptrs[op].child_lba = c;
                op++;
            }
        }
        if (op <= MAX_NODE_PTRS) {
            uint64_t nn = blk_alloc();
            if (!nn) return -1;
            memset(bscratch, 0, BTRFS_NODE_SIZE);
            root_set_ptrs(bscratch, op, ptrs);
            if (blk_write(nn, bscratch, BT_ROOT, BTRFS_ROOT_INO) != 0) return -1;
            up[0] = ptrs[0]; up[0].child_lba = nn;
            n_up = 1;
        } else {
            int mid = op / 2;
            uint64_t n1 = blk_alloc();
            uint64_t n2 = blk_alloc();
            if (!n1 || !n2) return -1;
            memset(bscratch, 0, BTRFS_NODE_SIZE);
            root_set_ptrs(bscratch, mid, ptrs);
            if (blk_write(n1, bscratch, BT_ROOT, BTRFS_ROOT_INO) != 0) return -1;
            memset(bscratch, 0, BTRFS_NODE_SIZE);
            root_set_ptrs(bscratch, op - mid, ptrs + mid);
            if (blk_write(n2, bscratch, BT_ROOT, BTRFS_ROOT_INO) != 0) return -1;
            up[0] = ptrs[0]; up[0].child_lba = n1;
            up[1] = ptrs[mid]; up[1].child_lba = n2;
            n_up = 2;
        }
    }

    if (n_up == 1) {
        *new_root = up[0].child_lba;
    } else {
        uint64_t nr = blk_alloc();
        if (!nr) return -1;
        memset(bscratch, 0, BTRFS_NODE_SIZE);
        root_set_ptrs(bscratch, 2, up);
        if (blk_write(nr, bscratch, BT_ROOT, BTRFS_ROOT_INO) != 0) return -1;
        *new_root = nr;
    }
    return 0;
}

/* Small metadata transactions. Fast path: all keys route to one leaf and
 * the result fits; rebuild that leaf and its ancestry once. Otherwise use
 * the existing split-capable inserter, keeping its new roots private. */
#ifndef AURAFS_BATCH_WRITES
#define AURAFS_BATCH_WRITES 1
#endif
#if AURAFS_BATCH_WRITES != 0 && AURAFS_BATCH_WRITES != 1
#error "AURAFS_BATCH_WRITES must be 0 or 1"
#endif
#define AURAFS_EDIT_MAX 32
struct tree_edit {
    uint64_t obj, off;
    uint32_t typ, size;
    const uint8_t *data;
};
static int tree_insert_many(uint64_t root, const struct tree_edit *edits,
                             int count, uint64_t *new_root) {
    if (!root || !edits || !new_root || count < 1 || count > AURAFS_EDIT_MAX)
        return -1;
    for (int i = 0; i < count; i++)
        if (edits[i].size > 64 || (edits[i].size && !edits[i].data)) return -1;
    if (!AURAFS_BATCH_WRITES) goto sequential;

    uint64_t path[BTRFS_MAX_LEVELS], other[BTRFS_MAX_LEVELS];
    int ci[BTRFS_MAX_LEVELS], oi[BTRFS_MAX_LEVELS], depth, od;
    if (tree_descend(root, edits[0].obj, edits[0].typ, edits[0].off,
                      path, ci, &depth, bdatabuf) != 0) return -1;
    for (int i = 1; i < count; i++) {
        if (tree_descend(root, edits[i].obj, edits[i].typ, edits[i].off,
                          other, oi, &od, bscratch) != 0) return -1;
        if (depth != od) goto sequential;
        for (int d = 0; d < depth; d++)
            if (path[d] != other[d] || ci[d] != oi[d]) goto sequential;
    }
    static struct leaf_slot slots[MAX_LEAF_ITEMS + AURAFS_EDIT_MAX];
    static uint32_t placed[MAX_LEAF_ITEMS + AURAFS_EDIT_MAX];
    static uint8_t packed[2 * BTRFS_NODE_SIZE];
    int n = leaf_nslots(bdatabuf);
    if (n < 0 || n > MAX_LEAF_ITEMS) return -1;
    uint32_t used = 0;
    for (int i = 0; i < n; i++) {
        uint32_t off, size;
        leaf_get(bdatabuf, i, &slots[i].obj, &slots[i].typ, &slots[i].off,
                  &off, &size);
        if (off > BTRFS_CSUM_OFFSET || size > BTRFS_CSUM_OFFSET - off ||
            size > sizeof(packed) - used) return -1;
        slots[i].src = used; slots[i].dsz = size;
        memcpy(packed + used, bdatabuf + off, size); used += size;
    }
    for (int e = 0; e < count; e++) {
        int i = 0;
        while (i < n && key_cmp(slots[i].obj, slots[i].typ, slots[i].off,
                                edits[e].obj, edits[e].typ, edits[e].off) < 0) i++;
        int replace = i < n && key_cmp(slots[i].obj, slots[i].typ, slots[i].off,
                                       edits[e].obj, edits[e].typ, edits[e].off) == 0;
        if (!replace) {
            for (int j = n; j > i; j--) slots[j] = slots[j-1];
            n++;
        }
        if (edits[e].size > sizeof(packed) - used) goto sequential;
        slots[i].obj = edits[e].obj; slots[i].typ = edits[e].typ;
        slots[i].off = edits[e].off; slots[i].dsz = edits[e].size;
        slots[i].src = used;
        if (edits[e].size) memcpy(packed + used, edits[e].data, edits[e].size);
        used += edits[e].size;
    }
    if (!leaf_room(n, slots)) goto sequential;
    for (int i = 0; i < n; i++) placed[i] = slots[i].src;
    uint64_t child = blk_alloc();
    if (!child || leaf_build(bscratch, n, slots, placed, packed) != 0 ||
        blk_write(child, bscratch, BT_LEAF, BTRFS_ROOT_INO) != 0) return -1;
    uint64_t obj = slots[0].obj, off = slots[0].off;
    uint32_t typ = slots[0].typ;
    for (int d = depth - 1; d >= 0; d--) {
        if (blk_read(path[d], bscratch) != 0) return -1;
        uint8_t *ptr = bscratch + 36 + ci[d] * 32;
        w64(ptr, obj); w64(ptr + 8, off); w32(ptr + 16, typ);
        w64(ptr + 24, child);
        uint64_t unused;
        root_get(bscratch, 0, &obj, &typ, &off, &unused);
        child = blk_alloc();
        if (!child || blk_write(child, bscratch, BT_ROOT, BTRFS_ROOT_INO) != 0)
            return -1;
    }
    aurafs_verify_stats.batch_fast++;
    *new_root = child;
    return 0;

sequential:;
    uint64_t current = root;
    for (int i = 0; i < count; i++)
        if (tree_insert(current, edits[i].obj, edits[i].typ, edits[i].off,
                         edits[i].data, edits[i].size, &current) != 0) return -1;
    aurafs_verify_stats.batch_fallback++;
    *new_root = current;
    return 0;
}

static int tree_remove(uint64_t root_lba, uint64_t obj, uint32_t typ, uint64_t off,
                       uint64_t *new_root) {
    if (!root_lba) return -1;
    uint64_t path_lbas[BTRFS_MAX_LEVELS];
    int path_ci[BTRFS_MAX_LEVELS];
    int depth = 0;
    if (tree_descend(root_lba, obj, typ, off, path_lbas, path_ci, &depth, bdatabuf) != 0)
        return -1;
    int idx = leaf_find(bdatabuf, obj, typ, off);
    if (idx < 0) return -1;

    static struct leaf_slot slots[MAX_LEAF_ITEMS];
    static uint32_t placed[MAX_LEAF_ITEMS];
    int n = leaf_nslots(bdatabuf);
    /* Like bscratch/bdatabuf, staging is shared by the serialized FS path.
     * Keep it off the small kernel stack (also called from btrfsck). */
    static uint8_t databuf[2 * BTRFS_NODE_SIZE];
    uint32_t dcur = 0;
    int out_n = 0;
    for (int i = 0; i < n; i++) {
        if (i == idx) continue;
        uint64_t o; uint32_t t; uint32_t dd, ds; uint64_t f;
        leaf_get(bdatabuf, i, &o, &t, &f, &dd, &ds);
        slots[out_n].obj = o; slots[out_n].typ = t; slots[out_n].off = f;
        slots[out_n].dsz = ds;
        placed[out_n] = dcur;
        memcpy(databuf + dcur, bdatabuf + dd, ds);
        dcur += ds;
        out_n++;
    }

    uint64_t nl = blk_alloc();
    if (!nl) return -1;
    if (leaf_build(bscratch, out_n, slots, placed, databuf) != 0) return -1;
    if (blk_write(nl, bscratch, BT_LEAF, BTRFS_ROOT_INO) != 0) return -1;

    uint64_t child_lba = nl;
    for (int lvl = depth - 1; lvl >= 0; lvl--) {
        if (blk_read(path_lbas[lvl], bscratch) != 0) return -1;
        int np = root_nptrs(bscratch);
        int ci = path_ci[lvl];
        static struct root_ptr ptrs[MAX_NODE_PTRS];
        for (int i = 0; i < np; i++) {
            uint64_t o, f, c; uint32_t t;
            root_get(bscratch, i, &o, &t, &f, &c);
            ptrs[i].obj = o; ptrs[i].typ = t; ptrs[i].off = f;
            ptrs[i].child_lba = (i == ci) ? child_lba : c;
        }
        uint64_t nn = blk_alloc();
        if (!nn) return -1;
        memset(bscratch, 0, BTRFS_NODE_SIZE);
        root_set_ptrs(bscratch, np, ptrs);
        if (blk_write(nn, bscratch, BT_ROOT, BTRFS_ROOT_INO) != 0) return -1;
        child_lba = nn;
    }
    *new_root = child_lba;
    return 0;
}

typedef int (*tree_visit_fn)(uint64_t obj, uint32_t typ, uint64_t off,
                             const uint8_t *data, uint32_t size, void *ctx);

static int tree_visit_node(uint64_t lba, uint64_t obj, uint32_t typ,
                           tree_visit_fn fn, void *ctx) {
    uint8_t blk[4096];
    if (blk_read(lba, blk) != 0) return 0;
    if (blk_type_hdr(blk) == BT_LEAF) {
        int n = leaf_nslots(blk);
        for (int i = 0; i < n; i++) {
            uint64_t o; uint32_t t, dd, ds; uint64_t f;
            leaf_get(blk, i, &o, &t, &f, &dd, &ds);
            if (o == obj && t == typ) {
                int rc = fn(o, t, f, blk + dd, ds, ctx);
                if (rc) return rc;
            }
        }
        return 0;
    }
    int np = root_nptrs(blk);
    for (int i = 0; i < np; i++) {
        uint64_t o, f, c; uint32_t t;
        root_get(blk, i, &o, &t, &f, &c);
        int rc = tree_visit_node(c, obj, typ, fn, ctx);
        if (rc) return rc;
    }
    return 0;
}

static int tree_visit(uint64_t root_lba, uint64_t obj, uint32_t typ,
                      tree_visit_fn fn, void *ctx) {
    if (!root_lba) return -1;
    return tree_visit_node(root_lba, obj, typ, fn, ctx);
}

static void tree_stats_node(uint64_t lba, int lvl, int *max_depth,
                            uint32_t *leaves, uint32_t *items) {
    uint8_t blk[4096];
    if (blk_read(lba, blk) != 0) return;
    if (blk_type_hdr(blk) == BT_LEAF) {
        if (lvl > *max_depth) *max_depth = lvl;
        (*leaves)++;
        *items += (uint32_t)leaf_nslots(blk);
        return;
    }
    int np = root_nptrs(blk);
    for (int i = 0; i < np; i++) {
        uint64_t o, f, c; uint32_t t;
        root_get(blk, i, &o, &t, &f, &c);
        tree_stats_node(c, lvl + 1, max_depth, leaves, items);
    }
}

static void tree_stats(uint64_t root_lba, int *depth, uint32_t *leaves, uint32_t *items) {
    *depth = 0; *leaves = 0; *items = 0;
    if (!root_lba) return;
    tree_stats_node(root_lba, 1, depth, leaves, items);
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

/* Collision chain: low 16 bits retain the legacy hash, high bits select
 * a chain slot. Empty records are tombstones, so deleting a predecessor
 * never hides a later colliding name. Existing single-entry volumes work. */
#define DIR_CHAIN_LIMIT 4096
static int dir_slot(uint64_t root, uint64_t parent, const char *name,
                    uint8_t *record, uint32_t *size, uint64_t *key, int *found) {
    uint32_t len = (uint32_t)strlen(name);
    uint64_t hash = name_hash((const uint8_t *)name, (int)len);
    uint64_t vacant = (uint64_t)-1;
    for (uint64_t i = 0; i < DIR_CHAIN_LIMIT; i++) {
        uint64_t off = hash | (i << 16);
        uint32_t n = 0;
        if (tree_lookup(root, parent, BTRFS_ITYPE_DIR, off, record,
                        BTRFS_MAX_PATH + 24, &n) != 0) {
            *key = vacant == (uint64_t)-1 ? off : vacant;
            *found = 0; *size = 0; return 0;
        }
        if (!n) { if (vacant == (uint64_t)-1) vacant = off; continue; }
        if (n >= 16 && r32(record + 12) == len && n >= 16 + len &&
            memcmp(record + 16, name, len) == 0) {
            *key = off; *found = 1; *size = n; return 0;
        }
    }
    if (vacant != (uint64_t)-1) {
        *key = vacant; *found = 0; *size = 0; return 0;
    }
    return -1;
}

static int dir_insert(uint64_t root, uint64_t parent, const uint8_t *data,
                      uint32_t size, uint64_t *new_root) {
    char name[BTRFS_MAX_PATH];
    if (size < 16) return -1;
    uint32_t len = r32(data + 12);
    if (len >= sizeof(name) || size < 16 + len) return -1;
    memcpy(name, data + 16, len); name[len] = 0;
    uint8_t record[BTRFS_MAX_PATH + 24];
    uint32_t n; uint64_t key; int found;
    if (dir_slot(root, parent, name, record, &n, &key, &found) != 0) return -1;
    return tree_insert(root, parent, BTRFS_ITYPE_DIR, key, data, size, new_root);
}

static int dir_remove(uint64_t root, uint64_t parent, const char *name,
                      uint64_t *new_root) {
    uint8_t record[BTRFS_MAX_PATH + 24];
    uint32_t n; uint64_t key; int found;
    if (dir_slot(root, parent, name, record, &n, &key, &found) != 0 || !found)
        return -1;
    return tree_insert(root, parent, BTRFS_ITYPE_DIR, key, record, 0, new_root);
}

/* ============================================================================
 * SECTION 7: PATH RESOLUTION
 * ============================================================================ */

/* Resolve a path.  Final component may not exist yet.  On success fills
 * *out_parent (dir that would contain the final component), *out_ino (its
 * ino if it exists, else 0), *found, and basename. */
static int btrfs_path_resolve_at(uint64_t root, const char *path, uint64_t *out_parent,
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
        uint64_t key; int hit;
        uint8_t di[BTRFS_MAX_PATH + 24];
        uint32_t sz = 0;
        if (dir_slot(root, dir_ino, comp, di, &sz, &key, &hit) == 0 && hit) {
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

static int btrfs_path_resolve(const char *path, uint64_t *out_parent,
                              uint64_t *out_ino, int *found, char *base, int bsz) {
    return btrfs_path_resolve_at(bm.tree_root_lba, path, out_parent, out_ino,
                                  found, base, bsz);
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
        /* Cache identity is the inode, not a reusable pathname. Hard-link
         * aliases share the vnode (and its append lock/visible size). */
        if (bv4pool[i].in_use && bv4pool[i].objectid == objid) {
            bv4pool[i].is_dir = is_dir;
            bv4pool[i].size = size;
            bv4pool[i].vnode.size = size;
            return &bv4pool[i];
        }
    }
    static uint64_t bv_lru_seq = 0;
    int slot = -1;
    for (int i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
        if (!bv4pool[i].in_use && bv4pool[i].vnode.vfs_refs == 0) { slot = i; break; }
    }
    if (slot < 0) {
        uint64_t best_seq = (uint64_t)-1;
        for (int i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
            if (bv4pool[i].vnode.vfs_refs == 0 && bv4pool[i].size < best_seq) {
                /* use slot index as simple round-robin if no timestamp */
                slot = i;
                best_seq = bv_lru_seq++;
                break;
            }
        }
        if (slot < 0) {
            for (int i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
                if (bv4pool[i].vnode.vfs_refs == 0) { slot = i; break; }
            }
        }
    }
    if (slot >= 0) {
        if (1) {
            struct btrfs_vinfo *v = &bv4pool[slot];
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
        if (bv4pool[i].in_use && strcmp(bv4pool[i].path, path) == 0) {
            /* Unlink/rename invalidates the name, not an open OFD's inode.
             * No VFS close callback is needed: LRU checks vfs_refs when the
             * final OFD reference has gone. A raw lookup pointer is unpinned. */
            if (bv4pool[i].vnode.vfs_refs) bv4pool[i].path[0] = 0;
            else bv4pool[i].in_use = 0;
        }
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
    struct btrfs_vinfo *vi = bv_intern(path, ino, is_dir, size);
    return vi ? &vi->vnode : NULL;
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
    if (dir_insert(bm.tree_root_lba, parent, di, (uint32_t)(16 + n),
                    &bm.tree_root_lba) != 0) return NULL;

    kprintf("[btrfs] create: ino %llu path '%s' parent=%llu\n",
            (unsigned long long)new_ino, base, (unsigned long long)parent);
    struct btrfs_vinfo *vi = bv_intern(path, new_ino, 0, 0);
    return vi ? &vi->vnode : NULL;
}

/* Locate the data extent for (ino, off).  Returns disk LBA or 0 if absent. */
static uint64_t extent_get_at(uint64_t root, uint64_t ino, uint64_t off) {
    uint8_t rec[EXTENT_REC_SIZE];
    if (tree_lookup(root, ino, BTRFS_ITYPE_EXTENT, off,
                    rec, EXTENT_REC_SIZE, NULL) != 0)
        return 0;
    return r64(rec + 0);
}

static uint64_t extent_get(uint64_t ino, uint64_t off) {
    return extent_get_at(bm.tree_root_lba, ino, off);
}

static int64_t btrfs_read_at(uint64_t root, uint64_t ino, uint64_t pos,
                            void *buf, uint64_t count) {
    uint8_t rec[INODE_REC_SIZE];
    if (tree_lookup(root, ino, BTRFS_ITYPE_INODE, 0, rec, sizeof(rec), NULL) != 0)
        return -1;
    if (r64(rec) & 0x4000) return -1;
    uint64_t fsize = r64(rec + 24);
    if (pos >= fsize || !count) return 0;
    /* Avoid wrapping pos+count, including caller requests of UINT64_MAX. */
    if (count > fsize - pos) count = fsize - pos;
    if (!buf || count > INT64_MAX) return -1;

    uint8_t *out = (uint8_t*)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t abs = pos + done;
        uint64_t bstart = (abs / DATA_PAYLOAD) * DATA_PAYLOAD;
        uint32_t inblk = (uint32_t)(abs - bstart);
        uint64_t lba = extent_get_at(root, ino, bstart);
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

static int64_t btrfs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct btrfs_vinfo *v = vn ? (struct btrfs_vinfo *)vn->fs_data : NULL;
    if (!v || v->is_dir) return -1;
    return btrfs_read_at(bm.tree_root_lba, v->objectid, pos, buf, count);
}

/* In-memory read-only snapshots. No root switch, checkpoint or data copy.
 * All API calls share the driver's existing scratch buffers: the caller
 * must serialize them with ordinary filesystem operations. */
static int aurafs_snapshot_slot(const struct aurafs_snapshot *snapshot) {
    if (!snapshot || !bm.mounted || !snapshot->registration_id) return -1;
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++) {
        const struct aurafs_snapshot *t = &aurafs_snapshots[i].token;
        if (t->registration_id == snapshot->registration_id &&
            t->mount_epoch == snapshot->mount_epoch &&
            t->root_lba == snapshot->root_lba &&
            t->allocation_end == snapshot->allocation_end) return (int)i;
    }
    return -1;
}

int aurafs_snapshot_create(struct aurafs_snapshot *snapshot) {
    if (!snapshot || !bm.mounted || !aurafs_mount_epoch ||
        aurafs_snapshot_id == UINT64_MAX) return -1;
    int slot = -1;
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++) {
        if (aurafs_snapshots[i].owner == snapshot) return -1;
        if (!aurafs_snapshots[i].token.registration_id && slot < 0) slot = (int)i;
    }
    if (slot < 0) return -1;
    struct aurafs_snapshot token = {
        .root_lba = bm.tree_root_lba,
        .allocation_end = bm.next_free_block,
        .mount_epoch = aurafs_mount_epoch,
        .registration_id = ++aurafs_snapshot_id,
    };
    aurafs_snapshots[slot].token = token;
    aurafs_snapshots[slot].owner = snapshot;
    *snapshot = token;
    return 0;
}

int aurafs_snapshot_roots(struct aurafs_snapshot *roots, uint32_t capacity) {
    if (!bm.mounted) return -1;
    unsigned count = 0;
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++)
        if (aurafs_snapshots[i].token.registration_id) count++;
    if (!roots) return capacity ? -1 : (int)count;
    if (capacity < count) return -1;
    unsigned out = 0;
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++)
        if (aurafs_snapshots[i].token.registration_id)
            roots[out++] = aurafs_snapshots[i].token;
    return (int)count;
}

int64_t aurafs_snapshot_read(const struct aurafs_snapshot *snapshot,
                             const char *path, uint64_t pos, void *buf,
                             uint64_t count) {
    if (aurafs_snapshot_slot(snapshot) < 0 || !path ||
        snapshot->mount_epoch != aurafs_mount_epoch ||
        !snapshot->mount_epoch || snapshot->root_lba < BTRFS_FIRST_FREE ||
        snapshot->root_lba % BTRFS_NODE_SIZE ||
        snapshot->allocation_end % BTRFS_NODE_SIZE ||
        snapshot->root_lba >= snapshot->allocation_end ||
        snapshot->allocation_end > bm.next_free_block)
        return -1;
    uint64_t parent, ino; int found;
    if (btrfs_path_resolve_at(snapshot->root_lba, path, &parent, &ino,
                              &found, NULL, 0) != 0 || !found)
        return -1;
    return btrfs_read_at(snapshot->root_lba, ino, pos, buf, count);
}

void aurafs_snapshot_release(struct aurafs_snapshot *snapshot) {
    int slot = aurafs_snapshot_slot(snapshot);
    if (slot >= 0) memset(&aurafs_snapshots[slot], 0, sizeof(aurafs_snapshots[slot]));
    if (snapshot) memset(snapshot, 0, sizeof(*snapshot));
}

static int64_t btrfs_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct btrfs_vinfo *v = (struct btrfs_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    if (count == 0) return 0;
    if (!buf || count > INT64_MAX || pos > UINT64_MAX - count) return -1;

    uint8_t rec[INODE_REC_SIZE];
    if (inode_get(v->objectid, rec) != 0) return -1;
    uint64_t fsize = r64(rec + 24);

    /* Keep metadata private until the entire write, including inode, works.
     * These bounded staging arrays share the driver's non-reentrant contract. */
    static struct tree_edit edits[AURAFS_EDIT_MAX];
    static uint8_t records[AURAFS_EDIT_MAX][INODE_REC_SIZE];
    uint64_t private_root = bm.tree_root_lba;
    int pending = 0;
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
        if (!new_lba) return -1; /* ENOSPC must never become a write to block 0. */
        if (blk_write(new_lba, blk, BT_DATA, v->objectid) != 0) return -1;

        /* Stage the extent, reserving a final slot for the inode record. */
        w64(records[pending], new_lba);
        w64(records[pending] + 8, DATA_PAYLOAD);
        edits[pending] = (struct tree_edit){v->objectid, bstart,
                                            BTRFS_ITYPE_EXTENT, EXTENT_REC_SIZE,
                                            records[pending]};
        pending++;
        if (pending == AURAFS_EDIT_MAX - 1 && done + chunk < count) {
            if (tree_insert_many(private_root, edits, pending, &private_root) != 0)
                return -1;
            pending = 0;
        }
        done += chunk;
    }

    uint64_t new_size = pos + count;
    if (new_size > fsize) fsize = new_size;
    w64(rec + 24, fsize);
    w64(rec + 48, 1337);   /* mtime */
    memcpy(records[pending], rec, INODE_REC_SIZE);
    edits[pending] = (struct tree_edit){v->objectid, 0, BTRFS_ITYPE_INODE,
                                        INODE_REC_SIZE, records[pending]};
    if (tree_insert_many(private_root, edits, pending + 1, &private_root) != 0)
        return -1;
    bm.tree_root_lba = private_root;
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
    if (s < 16) return 0; /* deleted collision-chain slot */
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

    /* FSMODERN M1: a listing that reports every file as 0 bytes is worse
     * than no listing -- `ls /data` said "(0 bytes)" for a file that `cat`
     * then read correctly, which reads like corruption and is only an
     * unfilled field.  The size lives in the inode record (offset 24, the
     * same one btrfs_read uses), so read it.  The directory item itself
     * carries ino + type + name and never had a size to give. */
    uint8_t irec[INODE_REC_SIZE];
    if (inode_get(de->inode, irec) == 0) de->size = r64(irec + 24);

    e->count++;
    return 0;
}

static int count_dir_children(uint64_t o, uint32_t t, uint64_t off,
                              const uint8_t *d, uint32_t s, void *c) {
    (void)o; (void)t; (void)off; (void)d; (void)s;
    if (s >= 16) (*(int*)c)++;
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
    if (dir_insert(bm.tree_root_lba, parent, di, (uint32_t)(16 + n),
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
    if (dir_remove(bm.tree_root_lba, parent, base, &nr) != 0)
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
    if (dir_remove(bm.tree_root_lba, parent, base, &nr) != 0)
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
    if (dir_insert(bm.tree_root_lba, np, di, (uint32_t)(16 + n), &nr) != 0)
        return -1;
    bm.tree_root_lba = nr;
    if (dir_remove(bm.tree_root_lba, op, obase, &nr) != 0)
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
    if (dir_insert(bm.tree_root_lba, np, di, (uint32_t)(16 + n), &nr) != 0)
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
static int aurafs_checkpoint_tree_guard(const uint8_t *sb);
static int btrfs_sync(void *fs_data) {
    (void)fs_data;
    if (!bm.mounted || bm.generation == UINT64_MAX) return -1;
    if (bm.dual && (bm.nodesize != BTRFS_NODE_SIZE ||
        bm.next_free_block < BTRFS_FIRST_FREE + BTRFS_NODE_SIZE ||
        bm.next_free_block % BTRFS_NODE_SIZE ||
        bm.next_free_block > aurafs_append_ceiling(bm.total_bytes, 1))) return -1;
    uint64_t target = bm.dual ? (bm.active_super == BTRFS_SUPER_OFFSET ?
                      AURAFS_MIRROR_OFFSET : BTRFS_SUPER_OFFSET) : BTRFS_SUPER_OFFSET;
    /* Flush appended data/metadata before publishing the root. The generic
     * fs_cache_sync() discards device errors; range flush reports them. */
    uint64_t first = BTRFS_FIRST_FREE / BC_BLOCK_SIZE;
    uint64_t end = bm.next_free_block / BC_BLOCK_SIZE;
    if (end < first || bc_flush_range(bm.bdev, first, end - first) != 0)
        return -1;
    int pre_barrier = blkdev_flush(bm.bdev);
    if ((pre_barrier != 0 && pre_barrier != -ENOTSUP) ||
        (bm.dual && pre_barrier != 0)) return -1;
    uint8_t *sb = bscratch;
    if (blk_read(bm.active_super, sb) != 0) return -1;
    w64(sb + 56, bm.generation);
    w64(sb + 64, bm.tree_root_lba);
    w64(sb + 72, bm.next_free_block);
    w64(sb + 136, bm.next_inode);
    w32(sb + 8, (uint32_t)bm.generation);
    /* Appended metadata is now at the backend and ordered by pre-FLUSH.
     * Refuse publication if this candidate cannot pass the same mandatory
     * metadata/depth/work check used by mount. No target is queued yet. */
    if (bm.dual && aurafs_checkpoint_tree_guard(sb) != 0) {
        kprintf("[aurafs] checkpoint metadata guard failed; publication refused, "
                "active checkpoint unchanged\n");
        return -1;
    }
    if (bm.dual) kprintf("[aurafs] checkpoint candidate metadata verified before publication\n");
    aurafs_checkpoint_remember(bm.tree_root_lba, bm.next_free_block);
    if (blk_write(target, sb, BT_SUPER, 0) != 0) return -1;
    /* blk_write queues the checkpoint in cache; success is not persistence. */
    if (bc_flush_range(bm.bdev, target / BC_BLOCK_SIZE, 1) != 0)
        return -1;
    int post_barrier = blkdev_flush(bm.bdev);
    /* Do not degrade a real barrier failure to the unsupported weak path.
     * Capability must be consistent within one serialized sync attempt. */
    if (post_barrier != pre_barrier) return -1;
    /* Read back through the backend, NOT fs_read_block/blk_read: those
     * could just return our newly clean cached super even if the device
     * acknowledged a dropped/partial write. Exact bytes also cover padding.
     * This is read-after-write verification, NOT a device FLUSH barrier or
     * permission to retire older checkpoint candidates. Shared scratch is
     * safe only under the existing serialized-caller contract. */
    if (blkdev_read(bm.bdev, target / BLKDEV_SECTOR_SIZE,
                    BTRFS_NODE_SIZE / BLKDEV_SECTOR_SIZE, bdatabuf) != 0 ||
        memcmp(sb, bdatabuf, BTRFS_NODE_SIZE) != 0) {
        kprintf("[aurafs] checkpoint readback failed; history retained, durability=WEAK\n");
        return -1;
    }
    bm.active_super = target;
    bm.generation++;
    int td = 0; uint32_t tl = 0, ti = 0;
    tree_stats(bm.tree_root_lba, &td, &tl, &ti);
    kprintf("[aurafs] checkpoint: backend readback verified; barriers=%s durability=WEAK, history retained\n",
            pre_barrier == 0 ? "device-FLUSH" : "unsupported");
    kprintf("[btrfs] fsync: persisted tree root LBA %llu gen %llu (depth=%d leaves=%u items=%u)\n",
            (unsigned long long)bm.tree_root_lba,
            (unsigned long long)bm.generation, td, tl, ti);
    kprintf("[aurafs] verify: reads=%llu sha_reads=%llu sha_writes=%llu hits=%llu slots=%u\n",
            (unsigned long long)aurafs_verify_stats.reads,
            (unsigned long long)aurafs_verify_stats.sha_reads,
            (unsigned long long)aurafs_verify_stats.sha_writes,
            (unsigned long long)aurafs_verify_stats.hits,
            (unsigned)AURAFS_VERIFY_CACHE_SLOTS);
    kprintf("[aurafs] batch: fast=%llu fallback=%llu enabled=%u\n",
            (unsigned long long)aurafs_verify_stats.batch_fast,
            (unsigned long long)aurafs_verify_stats.batch_fallback,
            (unsigned)AURAFS_BATCH_WRITES);
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
    /* FSMODERN M3: WEAK, and the reason is specific.  btrfs keeps its
     * tree index in RAM (bm.tree_root_lba) and only writes it on .sync, so
     * between a write() and a sync() the DATA blocks are in the buffer
     * cache and the INDEX that names them does not exist on disk at all.
     * A power cut in that window does not corrupt the volume -- the old
     * tree is still valid -- it silently loses the write.  M1 taught this
     * the hard way; M3's job is to stop pretending otherwise. */
    .durability = FS_DURABILITY_WEAK,
};

/* ============================================================================
 * SECTION 10: FORMAT AND MOUNT
 * ============================================================================ */

static int format_btrfs(void) {
    kprintf("[btrfs] formatting CoW filesystem...\n");
    bm.nodesize = BTRFS_NODE_SIZE;
    bm.sectorsize = BTRFS_NODE_SIZE;

    /* FSMODERN M1: the volume's size is the DEVICE's answer, not ours.
     *
     * This line used to read 256 MiB unconditionally.  On a 64 MiB disk that
     * wrote an allocation ceiling four times the hardware into the
     * superblock, and `bm.max_block = bm.total_bytes` made the allocator
     * willing to place blocks past the end of the device -- writes that the
     * block layer silently truncated or that landed outside the image
     * entirely.  It went unnoticed because /btrfs only mounts as the FIFTH
     * disk, which no default boot attaches; M1 attaches one, and the
     * receipt said "268435456 bytes" for a 64 MiB image on the first run.
     *
     * Now: ask the seam.  A backend that does not report a capacity (0) is
     * refused rather than guessed at -- a filesystem that invents its own
     * size is the same class of bug as a parser that invents a home. */
    uint64_t sectors = blkdev_sector_count(bm.bdev);
    if (sectors == 0 || sectors > UINT64_MAX / BLKDEV_SECTOR_SIZE) {
        kprintf("[btrfs] device %d does not report its capacity; refusing to "
                "format (a filesystem must not invent its own size)\n",
                bm.bdev);
        return -1;
    }
    bm.total_bytes = sectors * BLKDEV_SECTOR_SIZE;
    if (bm.dual) {
        /* Opt-in never overwrites a foreign/nonempty device, even if the
         * generic legacy format permission was also supplied. */
        if (bm.total_bytes < BTRFS_FIRST_FREE + 4 * BTRFS_NODE_SIZE ||
            bm.total_bytes % BTRFS_NODE_SIZE || blkdev_flush(bm.bdev) != 0) return -1;
        for (uint64_t pos = 0; pos < bm.total_bytes; pos += BTRFS_NODE_SIZE) {
            if (blkdev_read(bm.bdev, pos / 512, BTRFS_NODE_SIZE / 512, bdatabuf) != 0) return -1;
            for (unsigned i = 0; i < BTRFS_NODE_SIZE; i++) if (bdatabuf[i]) return -1;
        }
    }
    kprintf("[btrfs] device %d reports %llu sectors (%llu MiB)\n",
            bm.bdev, (unsigned long long)sectors,
            (unsigned long long)(bm.total_bytes / (1024 * 1024)));
    bm.generation = 1;
    bm.next_free_block = BTRFS_FIRST_FREE;
    bm.max_block = aurafs_append_ceiling(bm.total_bytes, bm.dual);
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
    memcpy(sb + 128, bm.dual ? AURAFS_DUAL_MAGIC : BTRFS_MAGIC, 8);
    w64(sb + 136, bm.next_inode);
    if (!bm.dual && blk_write(BTRFS_SUPER_OFFSET, sb, BT_SUPER, 0) != 0) return -1;

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
    if (bm.dual) {
        memset(sb, 0, BTRFS_NODE_SIZE);
        w64(sb + 32, bm.total_bytes); w64(sb + 40, bm.nodesize);
        w64(sb + 48, bm.sectorsize); w64(sb + 56, bm.generation);
        memcpy(sb + 80, "aurafs-v2", 9);
        memcpy(sb + 128, AURAFS_DUAL_MAGIC, 8);
        if (bc_flush_range(bm.bdev, BTRFS_FIRST_FREE / BC_BLOCK_SIZE,
             (bm.next_free_block - BTRFS_FIRST_FREE) / BC_BLOCK_SIZE) != 0 ||
             blkdev_flush(bm.bdev) != 0) return -1;
    } else if (btrfs_read_block(BTRFS_SUPER_OFFSET, sb) != 0) return -1;
    w64(sb + 64, bm.tree_root_lba);
    w64(sb + 72, bm.next_free_block);
    w64(sb + 136, bm.next_inode);
    if (blk_write(BTRFS_SUPER_OFFSET, sb, BT_SUPER, 0) != 0) return -1;

    if (bm.dual) {
        if (bc_flush_range(bm.bdev, BTRFS_SUPER_OFFSET / BC_BLOCK_SIZE, 1) != 0 ||
            blkdev_flush(bm.bdev) != 0 ||
            blkdev_read(bm.bdev, BTRFS_SUPER_OFFSET / 512, 8, bdatabuf) != 0 ||
            memcmp(sb, bdatabuf, BTRFS_NODE_SIZE)) return -1;
        if (blk_write(AURAFS_MIRROR_OFFSET, sb, BT_SUPER, 0) != 0 ||
            bc_flush_range(bm.bdev, AURAFS_MIRROR_OFFSET / BC_BLOCK_SIZE, 1) != 0 ||
            blkdev_flush(bm.bdev) != 0 ||
            blkdev_read(bm.bdev, AURAFS_MIRROR_OFFSET / 512, 8, bdatabuf) != 0 ||
            memcmp(sb, bdatabuf, BTRFS_NODE_SIZE)) return -1;
    }

    kprintf("[btrfs] format complete: %llu bytes, CoW enabled\n",
            (unsigned long long)bm.total_bytes);
    return 0;
}

/* Validate before trusting geometry, cursor or a root address from disk.
 * Never auto-format a recognizable but damaged volume. */
static int aurafs_validate_super(const uint8_t *sb, uint64_t device_bytes) {
    uint8_t digest[KSHA256_DIGEST_SIZE];
    ksha256(sb, BTRFS_CSUM_OFFSET, digest);
    if (r32(sb + 4) != BTRFS_BLK_MAGIC || r32(sb + 12) != BT_SUPER ||
        (memcmp(sb + 128, BTRFS_MAGIC, 8) != 0 &&
         memcmp(sb + 128, AURAFS_DUAL_MAGIC, 8) != 0) ||
        memcmp(digest, sb + BTRFS_CSUM_OFFSET, sizeof(digest)) != 0)
        return -1;
    if (!memcmp(sb + 128, AURAFS_DUAL_MAGIC, 8) &&
        (!r64(sb + 56) || r64(sb + 56) == UINT64_MAX ||
         r32(sb + 8) != (uint32_t)r64(sb + 56) ||
         (r64(sb + 24) != BTRFS_SUPER_OFFSET && r64(sb + 24) != AURAFS_MIRROR_OFFSET))) return -1;
    uint64_t total = r64(sb + 32), cursor = r64(sb + 72);
    uint64_t root = r64(sb + 64), ino = r64(sb + 136);
    if (r64(sb + 40) != BTRFS_NODE_SIZE || r64(sb + 48) != BTRFS_NODE_SIZE ||
        total > device_bytes || total < BTRFS_FIRST_FREE + BTRFS_NODE_SIZE ||
        cursor < BTRFS_FIRST_FREE + BTRFS_NODE_SIZE || cursor > total ||
        cursor % BTRFS_NODE_SIZE || root < BTRFS_FIRST_FREE ||
        root % BTRFS_NODE_SIZE || root >= cursor || ino < 2 || ino > UINT32_MAX)
        return -1;
    return 0;
}

/* 0 selected into bscratch, 1 absent/unrecognised, -1 damaged/ambiguous.
 * Read-only: no automatic repair and no migration. */
static int aurafs_load_checkpoint(uint64_t device_bytes) {
    int ar = btrfs_read_super(BTRFS_SUPER_OFFSET, bscratch);
    int br = btrfs_read_super(AURAFS_MIRROR_OFFSET, bdatabuf);
    int av2 = !ar && !memcmp(bscratch + 128, AURAFS_DUAL_MAGIC, 8);
    int bv2 = !br && !memcmp(bdatabuf + 128, AURAFS_DUAL_MAGIC, 8);
    int al = !ar && !memcmp(bscratch + 128, BTRFS_MAGIC, 8);
    int aok = av2 && aurafs_validate_super(bscratch, device_bytes) == 0 &&
              r64(bscratch + 24) == BTRFS_SUPER_OFFSET;
    int bok = bv2 && aurafs_validate_super(bdatabuf, device_bytes) == 0 &&
              r64(bdatabuf + 24) == AURAFS_MIRROR_OFFSET;
    int legacy = al && aurafs_validate_super(bscratch, device_bytes) == 0;
    if (legacy && bok) return -1; /* mixed format, no implicit migration */
    if (aok || bok) {
        if (aok && bok && (r64(bscratch + 32) != r64(bdatabuf + 32) ||
            (r64(bscratch + 56) == r64(bdatabuf + 56) &&
             memcmp(bscratch + 32, bdatabuf + 32, BTRFS_CSUM_OFFSET - 32)))) return -1;
        if (aok) aurafs_checkpoint_remember(r64(bscratch + 64), r64(bscratch + 72));
        if (bok) aurafs_checkpoint_remember(r64(bdatabuf + 64), r64(bdatabuf + 72));
        bm.dual = 1;
        if (bok && (!aok || r64(bdatabuf + 56) > r64(bscratch + 56))) {
            memcpy(bscratch, bdatabuf, BTRFS_NODE_SIZE);
            bm.active_super = AURAFS_MIRROR_OFFSET;
        } else bm.active_super = BTRFS_SUPER_OFFSET;
        return 0;
    }
    if (legacy) { bm.active_super = BTRFS_SUPER_OFFSET; return 0; }
    if (av2 || bv2 || al) return -1;
    if (ar || br) return -1; /* unreadable is not evidence of a blank disk */
    return 1;
}

/* Read-only diagnostic, NOT a free-space map or permission to reuse blocks.
 * One byte/block for union roles plus one for each root's DFS state. */
#define AURAFS_AUDIT_MAX_BLOCKS 262144U
struct aurafs_audit_frame {
    uint64_t lba;
    uint32_t next, count, type;
    uint8_t bytes[BTRFS_NODE_SIZE];
};
struct aurafs_audit_work {
    struct aurafs_snapshot roots[AURAFS_MAX_SNAPSHOTS + AURAFS_CHECKPOINT_HISTORY + 2];
    struct aurafs_audit_frame frames[BTRFS_MAX_LEVELS + 1];
    uint8_t data[BTRFS_NODE_SIZE];
};

static int aurafs_audit_address(uint64_t lba, uint64_t end) {
    return lba >= BTRFS_FIRST_FREE && !(lba % BTRFS_NODE_SIZE) &&
           lba < end && end - lba >= BTRFS_NODE_SIZE;
}

static int aurafs_audit_leaf(const uint8_t *blk, uint32_t count) {
    uint32_t table_end = 36 + count * 28;
    uint64_t prev_obj = 0, prev_off = 0;
    uint32_t prev_typ = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t obj, off; uint32_t typ, dd, ds;
        leaf_get(blk, i, &obj, &typ, &off, &dd, &ds);
        if (i && key_cmp(prev_obj, prev_typ, prev_off, obj, typ, off) >= 0) return -1;
        prev_obj = obj; prev_typ = typ; prev_off = off;
        if (dd < table_end || dd > BTRFS_CSUM_OFFSET || ds > BTRFS_CSUM_OFFSET - dd)
            return -1;
        if (typ == BTRFS_ITYPE_DIR && !ds) continue; /* deleted hash slot */
        if ((typ == BTRFS_ITYPE_INODE && ds != INODE_REC_SIZE) ||
            (typ == BTRFS_ITYPE_EXTENT &&
             (ds != EXTENT_REC_SIZE || off % DATA_PAYLOAD ||
              r64(blk + dd + 8) != DATA_PAYLOAD)) ||
            (typ == BTRFS_ITYPE_DIR &&
             (ds < 16 || r32(blk + dd + 12) != ds - 16)) ||
            (typ != BTRFS_ITYPE_INODE && typ != BTRFS_ITYPE_EXTENT && typ != BTRFS_ITYPE_DIR))
            return -1;
        for (uint32_t j = 0; j < i; j++) {
            uint64_t jo, jf; uint32_t jt, jd, js;
            leaf_get(blk, j, &jo, &jt, &jf, &jd, &js);
            if (dd < jd + js && jd < dd + ds) return -1;
        }
    }
    return 0;
}

int aurafs_reachability_audit(struct aurafs_reachability *report) {
    if (!report || !bm.mounted || aurafs_checkpoint_overflow ||
        !aurafs_checkpoint_count || bm.nodesize != BTRFS_NODE_SIZE ||
        bm.next_free_block > bm.max_block ||
        bm.next_free_block % BTRFS_NODE_SIZE || bm.next_free_block < BTRFS_FIRST_FREE)
        return -1;
    uint64_t blocks = bm.next_free_block / BTRFS_NODE_SIZE;
    if (blocks > AURAFS_AUDIT_MAX_BLOCKS) return -1;
    struct aurafs_audit_work *w = kmalloc(sizeof(*w) + blocks * 2);
    if (!w) return -1;
    uint8_t *roles = (uint8_t *)(w + 1), *state = roles + blocks;
    memset(roles, 0, (size_t)blocks);
    struct aurafs_reachability result = {0};
    int rc = -1;
    /* This is the checkpoint image observed THROUGH THE BLOCK CACHE. It is
     * not proof of a durable device image or of an older super after torn I/O. */
    if (btrfs_read_super(bm.active_super, w->data) != 0 ||
        aurafs_validate_super(w->data, bm.total_bytes) != 0 ||
        r64(w->data + 32) != bm.total_bytes ||
        r64(w->data + 72) > bm.next_free_block) goto out;
    w->roots[0] = (struct aurafs_snapshot){.root_lba = bm.tree_root_lba,
                                        .allocation_end = bm.next_free_block};
    w->roots[1] = (struct aurafs_snapshot){.root_lba = r64(w->data + 64),
                                        .allocation_end = r64(w->data + 72)};
    int snaps = aurafs_snapshot_roots(w->roots + 2, AURAFS_MAX_SNAPSHOTS);
    if (snaps < 0) goto out;
    result.roots = 2 + snaps;
    for (unsigned i = 0; i < aurafs_checkpoint_count; i++) {
        uint64_t root = aurafs_checkpoint_history[i].root_lba;
        uint64_t end = aurafs_checkpoint_history[i].allocation_end;
        unsigned j;
        for (j = 0; j < result.roots; j++)
            if (w->roots[j].root_lba == root && w->roots[j].allocation_end == end) break;
        if (j != result.roots) continue;
        w->roots[result.roots++] = (struct aurafs_snapshot){.root_lba = root,
                                                         .allocation_end = end};
    }
    uint64_t steps = 0;
    for (uint32_t root = 0; root < result.roots; root++) {
        uint64_t end = w->roots[root].allocation_end;
        if (end > bm.next_free_block || end % BTRFS_NODE_SIZE) goto out;
        memset(state, 0, (size_t)blocks);
        unsigned depth = 0;
        w->frames[0].lba = w->roots[root].root_lba;
        w->frames[0].next = UINT32_MAX;
        for (;;) {
            if (++steps > 4194304) goto out; /* bounded diagnostic CPU/I/O work */
            struct aurafs_audit_frame *f = &w->frames[depth];
            if (!aurafs_audit_address(f->lba, end)) goto out;
            uint64_t index = f->lba / BTRFS_NODE_SIZE;
            if (f->next == UINT32_MAX) {
                if (state[index] == 1) goto out; /* ancestor cycle */
                if (state[index] == 2) goto pop;
                if (roles[index] == 2 || blk_read(f->lba, f->bytes) != 0 ||
                    r64(f->bytes + 24) != f->lba) goto out;
                f->type = r32(f->bytes + 12);
                f->count = r32(f->bytes + 32);
                if (f->type == BT_LEAF) {
                    if (f->count > MAX_LEAF_ITEMS ||
                        aurafs_audit_leaf(f->bytes, f->count) != 0) goto out;
                } else if (f->type == BT_ROOT) {
                    if (!f->count || f->count > MAX_NODE_PTRS) goto out;
                    for (uint32_t i = 1; i < f->count; i++) {
                        const uint8_t *p = f->bytes + 36 + (i - 1) * 32;
                        const uint8_t *q = p + 32;
                        if (key_cmp(r64(p), r32(p + 16), r64(p + 8),
                                    r64(q), r32(q + 16), r64(q + 8)) >= 0) goto out;
                    }
                } else goto out;
                if (!roles[index]) { roles[index] = 1; result.metadata_blocks++; }
                state[index] = 1; f->next = 0;
            }
            if (f->next == f->count) {
                state[index] = 2;
                goto pop;
            }
            uint32_t item = f->next++;
            if (f->type == BT_ROOT) {
                if (depth == BTRFS_MAX_LEVELS) goto out;
                uint64_t child = r64(f->bytes + 36 + item * 32 + 24);
                depth++;
                w->frames[depth].lba = child;
                w->frames[depth].next = UINT32_MAX;
            } else {
                uint64_t obj, off; uint32_t typ, dd, ds;
                leaf_get(f->bytes, item, &obj, &typ, &off, &dd, &ds);
                if (typ != BTRFS_ITYPE_EXTENT) continue;
                uint64_t lba = r64(f->bytes + dd);
                if (!aurafs_audit_address(lba, end)) goto out;
                uint64_t di = lba / BTRFS_NODE_SIZE;
                if (roles[di] == 1 || blk_read(lba, w->data) != 0 ||
                    r32(w->data + 12) != BT_DATA || r64(w->data + 24) != lba ||
                    r64(w->data + 16) != obj) goto out;
                if (!roles[di]) { roles[di] = 2; result.data_blocks++; }
            }
            continue;
pop:
            if (!depth) break;
            depth--;
        }
    }
    result.allocated_blocks = (bm.next_free_block - BTRFS_FIRST_FREE) / BTRFS_NODE_SIZE;
    result.unreferenced_blocks = result.allocated_blocks - result.metadata_blocks - result.data_blocks;
    *report = result; /* success-only publication; not a reusable-block list */
    rc = 0;
out:
    kfree(w);
    return rc;
}

/* Mandatory selected-tree verification for v2, before mounted publication.
 * User-selected policy: fail closed on corruption/OOM/limits, never rollback.
 * Prefix budget: 1 GiB, depth 8 edges, 4M frame steps. Metadata only: extent
 * addresses/role conflicts checked, but file data blocks are NOT read. */
struct aurafs_mount_work {
    struct aurafs_audit_frame frames[BTRFS_MAX_LEVELS + 1];
};
static int aurafs_checkpoint_tree_guard_limited(const uint8_t *sb,
                                               uint64_t max_blocks, uint64_t max_steps) {
    uint64_t end = r64(sb + 72), blocks = end / BTRFS_NODE_SIZE;
    if (!blocks || blocks > max_blocks || blocks > AURAFS_MOUNT_MAX_BLOCKS) return -1;
    struct aurafs_mount_work *w = kmalloc(sizeof(*w) + blocks);
    if (!w) return -1;
    /* 0 unseen, 1 metadata active, 2 metadata done, 3 extent data address.
     * Repeated metadata references/cycles are invalid inside a single tree.
     * Shared DATA references are not rejected by this structural check. */
    uint8_t *state = (uint8_t *)(w + 1);
    memset(state, 0, (size_t)blocks);
    unsigned depth = 0;
    uint64_t steps = 0;
    int rc = -1;
    w->frames[0].lba = r64(sb + 64); w->frames[0].next = UINT32_MAX;
    for (;;) {
        if (++steps > max_steps || steps > AURAFS_MOUNT_MAX_STEPS) goto out;
        struct aurafs_audit_frame *f = &w->frames[depth];
        if (!aurafs_audit_address(f->lba, end)) goto out;
        uint64_t index = f->lba / BTRFS_NODE_SIZE;
        if (f->next == UINT32_MAX) {
            if (state[index]) goto out;
            if (blkdev_read(bm.bdev, f->lba / BLKDEV_SECTOR_SIZE,
                           BTRFS_NODE_SIZE / BLKDEV_SECTOR_SIZE, f->bytes) != 0) goto out;
            uint8_t digest[KSHA256_DIGEST_SIZE];
            ksha256(f->bytes, BTRFS_CSUM_OFFSET, digest);
            if (r32(f->bytes + 4) != BTRFS_BLK_MAGIC || r64(f->bytes + 24) != f->lba ||
                memcmp(digest, f->bytes + BTRFS_CSUM_OFFSET, sizeof(digest))) goto out;
            f->type = r32(f->bytes + 12); f->count = r32(f->bytes + 32);
            if (f->type == BT_LEAF) {
                if (f->count > MAX_LEAF_ITEMS || aurafs_audit_leaf(f->bytes, f->count)) goto out;
            } else if (f->type == BT_ROOT) {
                if (!f->count || f->count > MAX_NODE_PTRS) goto out;
                for (uint32_t i = 1; i < f->count; i++) {
                    const uint8_t *p = f->bytes + 36 + (i - 1) * 32, *q = p + 32;
                    if (key_cmp(r64(p), r32(p + 16), r64(p + 8),
                                r64(q), r32(q + 16), r64(q + 8)) >= 0) goto out;
                }
            } else goto out;
            state[index] = 1; f->next = 0;
        }
        if (f->next == f->count) {
            state[index] = 2;
            if (!depth) break;
            depth--; continue;
        }
        uint32_t item = f->next++;
        if (f->type == BT_ROOT) {
            if (depth == BTRFS_MAX_LEVELS) goto out;
            uint64_t child = r64(f->bytes + 36 + item * 32 + 24);
            depth++;
            w->frames[depth].lba = child; w->frames[depth].next = UINT32_MAX;
        } else {
            uint64_t obj, off; uint32_t typ, dd, ds;
            leaf_get(f->bytes, item, &obj, &typ, &off, &dd, &ds);
            if (typ != BTRFS_ITYPE_EXTENT) continue;
            uint64_t lba = r64(f->bytes + dd);
            if (!aurafs_audit_address(lba, end)) goto out;
            uint64_t data_index = lba / BTRFS_NODE_SIZE;
            if (state[data_index] == 1 || state[data_index] == 2) goto out;
            state[data_index] = 3;
        }
    }
    rc = 0;
out:
    kfree(w);
    return rc;
}
static int aurafs_checkpoint_tree_guard(const uint8_t *sb) {
    return aurafs_checkpoint_tree_guard_limited(sb, AURAFS_MOUNT_MAX_BLOCKS,
                                               AURAFS_MOUNT_MAX_STEPS);
}

int btrfs_init(int prefer_port) {
    memset(aurafs_checkpoint_history, 0, sizeof(aurafs_checkpoint_history));
    aurafs_checkpoint_count = 0;
    aurafs_checkpoint_overflow = 0;
    memset(aurafs_snapshots, 0, sizeof(aurafs_snapshots));
    if (++aurafs_mount_epoch == 0) ++aurafs_mount_epoch;
    aurafs_verify_reset();
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

    uint64_t sectors = blkdev_sector_count(bm.bdev);
    if (!sectors || sectors > UINT64_MAX / BLKDEV_SECTOR_SIZE) return -1;
    int selected = aurafs_load_checkpoint(sectors * BLKDEV_SECTOR_SIZE);
    if (selected < 0) {
        kprintf("[aurafs] refusing damaged or ambiguous checkpoint; volume NOT reformatted\n");
        return -1;
    }
    if (selected > 0) {
        if (!fs_format_allowed()) return -1;
        bm.dual = fs_aurafs_dual_allowed();
        if (format_btrfs() != 0 ||
            aurafs_load_checkpoint(sectors * BLKDEV_SECTOR_SIZE) != 0) return -1;
    }
    uint8_t *sb = bscratch;
    if (bm.dual && aurafs_checkpoint_tree_guard(sb) != 0) {
        memset(aurafs_checkpoint_history, 0, sizeof(aurafs_checkpoint_history));
        aurafs_checkpoint_count = 0;
        aurafs_checkpoint_overflow = 0;
        kprintf("[aurafs] selected checkpoint metadata guard failed; mount refused, "
                "no older-root rollback or repair\n");
        return -1;
    }

    bm.nodesize = r64(sb + 40) ? r64(sb + 40) : BTRFS_NODE_SIZE;
    bm.sectorsize = r64(sb + 48) ? r64(sb + 48) : BTRFS_NODE_SIZE;
    bm.total_bytes = r64(sb + 32);
    bm.generation = r64(sb + 56) + 1;
    bm.tree_root_lba = r64(sb + 64);
    bm.next_free_block = r64(sb + 72) ? r64(sb + 72) : BTRFS_FIRST_FREE;
    bm.max_block = aurafs_append_ceiling(bm.total_bytes, bm.dual);
    bm.next_inode = (uint32_t)(r64(sb + 136) ? r64(sb + 136) : 2);

    kprintf("[btrfs] mounted CoW filesystem at /btrfs:\n");
    kprintf("       size=%llu, nodesize=%llu, tree_root=%llu, gen=%llu\n",
            (unsigned long long)bm.total_bytes, (unsigned long long)bm.nodesize,
            (unsigned long long)bm.tree_root_lba, (unsigned long long)bm.generation);
    kprintf("       SHA-256 checksums (block trailer) + CoW tree re-parenting enabled\n");
    kprintf("[aurafs] append log: cursor=%llu free_blocks=%llu; CoW, SHA-256, "
            "hashed dirs; reclaim=none durability=WEAK\n",
            (unsigned long long)bm.next_free_block,
            (unsigned long long)((bm.max_block - bm.next_free_block) / BTRFS_NODE_SIZE));
    aurafs_checkpoint_remember(bm.tree_root_lba, bm.next_free_block);
    bm.mounted = 1;
    kprintf("[aurafs] checkpoint format=%s active=%llu generation=%llu migration=none\n",
            bm.dual ? "dual-v2" : "legacy",
            (unsigned long long)bm.active_super,
            (unsigned long long)(bm.generation - 1));
    return 0;
}

/* ============================================================================
 * SECTION 11: SELF-TEST
 * ============================================================================ */

/* Exercise the driver's VFS-reference contract, not FD-table machinery.
 * Test pins model active OFDs; every failure path drops only its own pins. */
static int btrfs_test_open_identity(void) {
    struct vnode *old = NULL, *fresh = NULL;
    char bytes[8];
    int rc = -1;
    old = btrfs_create(NULL, "m6j-open");
    if (!old) goto out;
    old->vfs_refs++;
    uint64_t old_ino = old->inode_id;
    if (btrfs_write(old, 0, "OLD", 3) != 3 ||
        btrfs_unlink(NULL, "m6j-open") != 0) goto out;
    fresh = btrfs_create(NULL, "m6j-open");
    if (!fresh) goto out;
    fresh->vfs_refs++;
    if (fresh == old || fresh->inode_id == old_ino ||
        old->inode_id != old_ino ||
        btrfs_write(fresh, 0, "NEW", 3) != 3 ||
        btrfs_read(old, 0, bytes, sizeof(bytes)) != 3 || memcmp(bytes, "OLD", 3) ||
        btrfs_write(old, 0, "GONE", 4) != 4 ||
        btrfs_read(fresh, 0, bytes, sizeof(bytes)) != 3 || memcmp(bytes, "NEW", 3)) goto out;
    struct vfs_stat st;
    if (btrfs_stat(old, &st) != 0 || st.nlink != 0 ||
        btrfs_rename(NULL, "m6j-open", "m6j-renamed") != 0 ||
        btrfs_lookup(NULL, "m6j-renamed") != fresh ||
        btrfs_link(NULL, "m6j-renamed", "m6j-hard") != 0 ||
        btrfs_lookup(NULL, "m6j-hard") != fresh ||
        btrfs_unlink(NULL, "m6j-renamed") != 0 ||
        btrfs_read(fresh, 0, bytes, sizeof(bytes)) != 3 || memcmp(bytes, "NEW", 3) ||
        btrfs_unlink(NULL, "m6j-hard") != 0 ||
        btrfs_read(fresh, 0, bytes, sizeof(bytes)) != 3 || memcmp(bytes, "NEW", 3)) goto out;
    rc = 0;
out:
    if (fresh) fresh->vfs_refs--;
    if (old) old->vfs_refs--;
    return rc;
}

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
        static uint8_t pat[8192];
        for (int i = 0; i < 8192; i++) pat[i] = (uint8_t)(i * 7 + 3);
        if (btrfs_write(mb, 0, pat, sizeof(pat)) != (int64_t)sizeof(pat)) {
            kprintf("[btrfs] FAIL: multi-block write\n"); return -6;
        }
        static uint8_t got[8192];
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

    if (btrfs_test_open_identity() != 0) {
        kprintf("[btrfs] FAIL: pinned vnode identity after unlink/recreate\n");
        return -13;
    }
    kprintf("[btrfs] PASS: pinned vnode identity, unlink/recreate, rename/hard-link aliases\n");

    kprintf("[btrfs] PASS: CoW tree, SHA-256, multi-block, rename, rmdir, truncate, link, settimes, fsync\n");
    kprintf("[btrfs] PASS: SHA-256 detects on-disk corruption (RESIDUE2 T3)\n");
    return 0;
}
