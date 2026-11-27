/* M6a: actual btrfs + buffer cache + blkdev, RAM device with write faults.
 * No throughput claim: host memory is NOT an emulated or physical disk. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "kernel/fs/btrfs.c"
#include "tools/fsbench/clock_math.h"

#define DISK_SIZE (8 * 1024 * 1024)
static uint8_t disk[DISK_SIZE];
static int fail_appends, fail_checkpoint, writes, allow_format = 1;
static int append_seen, checkpoint_seen;
static int fail_super_read, drop_super_write, partial_super_write, alter_super_write;
static unsigned backend_super_reads;
static uint64_t guard_fail_root;
static unsigned guard_backend_reads;
static uint64_t guard_capacity_override;
static unsigned partial_checkpoint_bytes = 512;
/* M6o format-only fault counters: backend requests, not cache operations. */
static int format_write_count, format_write_at, format_write_mode;
static unsigned format_write_bytes;
static int format_readbacks, format_readback_at;
static int flush_enabled, flush_calls, fail_flush_at, change_flush_at;
static int super_writes_at_flush[2], flushes_at_readback;
static int device_flush(void *ctx) {
    (void)ctx;
    flush_calls++;
    if (flush_calls <= 2) super_writes_at_flush[flush_calls - 1] = checkpoint_seen;
    if (flush_calls == fail_flush_at) return -EIO;
    if (flush_calls == change_flush_at) return -ENOTSUP;
    return flush_enabled ? 0 : -ENOTSUP;
}
void kprintf(const char *fmt, ...) { (void)fmt; }
void spinlock_init(spinlock_t *l) { l->locked = 0; }
void spinlock_acquire(spinlock_t *l) { (void)l; }
void spinlock_release(spinlock_t *l) { (void)l; }
int spinlock_try_acquire(spinlock_t *l) { (void)l; return 1; }
static int audit_oom;
void *kmalloc(uint64_t n) { return audit_oom ? NULL : calloc(1, (size_t)n); }
void kfree(void *p) { free(p); }
uint64_t pmm_get_usable_frames(void) { return 128 * 1024 * 1024 / 4096; }
int fs_format_allowed(void) { return allow_format; }
static int allow_dual;
int fs_aurafs_dual_allowed(void) { return allow_dual; }
static int rd(void *ctx, uint64_t lba, uint32_t n, void *buf) {
    (void)ctx;
    if (guard_fail_root && lba == guard_fail_root / 512) {
        guard_backend_reads++;
        return -EIO;
    }
    if (lba == BTRFS_SUPER_OFFSET / 512 || lba == AURAFS_MIRROR_OFFSET / 512) {
        backend_super_reads++;
        flushes_at_readback = flush_calls;
        if (checkpoint_seen) {
            format_readbacks++;
            if (format_readbacks == format_readback_at) return -EIO;
        }
        if (fail_super_read) return -1;
    }
    if (lba > DISK_SIZE / 512 || n > DISK_SIZE / 512 - lba) return -1;
    memcpy(buf, disk + lba * 512, n * 512); return 0;
}
static int wr(void *ctx, uint64_t lba, uint32_t n, const void *buf) {
    (void)ctx;
    if (lba > DISK_SIZE / 512 || n > DISK_SIZE / 512 - lba) return -1;
    format_write_count++;
    if (format_write_count == format_write_at) {
        if (format_write_mode == 1) return -EIO;
        assert(format_write_bytes <= n * 512);
        memcpy(disk + lba * 512, buf, format_write_bytes);
        return 0; /* accepted prefix only, including accepted dropped writes */
    }
    if (lba == BTRFS_SUPER_OFFSET / 512 || lba == AURAFS_MIRROR_OFFSET / 512) {
        if (fail_checkpoint) return -1;
        checkpoint_seen++;
        if (drop_super_write) return 0; /* acknowledge without storing */
        if (partial_super_write) {
            memcpy(disk + lba * 512, buf, partial_checkpoint_bytes);
            return 0; /* first sector only, stale SHA trailer */
        }
        if (alter_super_write) {
            memcpy(disk + lba * 512, buf, n * 512);
            disk[lba * 512 + 80] ^= 1;
            ksha256(disk + lba * 512, BTRFS_CSUM_OFFSET,
                    disk + lba * 512 + BTRFS_CSUM_OFFSET);
            return 0; /* valid SHA but not the submitted checkpoint bytes */
        }
    } else {
        if (fail_appends) return -1;
        append_seen++;
    }
    memcpy(disk + lba * 512, buf, n * 512); writes++; return 0;
}
static uint64_t capacity(void *ctx) { (void)ctx; return guard_capacity_override ? guard_capacity_override : DISK_SIZE / 512; }
static void seal(uint8_t *sb) {
    ksha256(sb, BTRFS_CSUM_OFFSET, sb + BTRFS_CSUM_OFFSET);
}
static void test_verification_cache(void) {
    uint8_t good[BTRFS_NODE_SIZE], block[BTRFS_NODE_SIZE];
    uint64_t root = bm.tree_root_lba;
    assert(blk_read(root, good) == 0);
    uint64_t sha = aurafs_verify_stats.sha_reads;
    uint64_t hits = aurafs_verify_stats.hits;
    assert(blk_read(root, block) == 0);
#if AURAFS_VERIFY_CACHE_SLOTS > 0
    assert(aurafs_verify_stats.sha_reads == sha);
    assert(aurafs_verify_stats.hits == hits + 1);
#else
    assert(aurafs_verify_stats.sha_reads == sha + 1);
    assert(aurafs_verify_stats.hits == hits);
#endif
    /* Bypass any FS helper invalidation: even a warmed entry must compare
     * freshly fetched bytes, including header, payload and digest trailer. */
    const unsigned offsets[] = {16, BTRFS_CSUM_OFFSET - 1, BTRFS_CSUM_OFFSET + 7};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        memcpy(block, good, sizeof(block)); block[offsets[i]] ^= 1;
        assert(fs_write_block(bm.bdev, root / 512, 8, block) == 0);
        assert(blk_read(root, block) != 0);
        assert(fs_write_block(bm.bdev, root / 512, 8, good) == 0);
        assert(blk_read(root, block) == 0);
    }
    uint64_t data = blk_alloc();
    memset(block, 0x31, sizeof(block));
    assert(blk_write(data, block, BT_DATA, 123) == 0);
    sha = aurafs_verify_stats.sha_reads; hits = aurafs_verify_stats.hits;
    assert(blk_read(data, block) == 0 && blk_read(data, block) == 0);
    assert(aurafs_verify_stats.sha_reads == sha + 2);
    assert(aurafs_verify_stats.hits == hits);
    puts("AuraFS M6d: PASS (metadata memoization, fresh-byte comparison, header/payload/trailer corruption, data always hashed)");
}

static struct aurafs_snapshot stale_snapshot;
static void test_snapshots(void) {
    static uint8_t original[9000], replacement[9000], out[9000];
    for (unsigned i = 0; i < sizeof(original); i++) {
        original[i] = (uint8_t)(i * 17 + 3);
        replacement[i] = (uint8_t)(i * 7 + 9);
    }
    assert(btrfs_mkdir(NULL, "snapdir") == 0);
    struct vnode *file = btrfs_create(NULL, "snapdir/file");
    assert(file);
    assert(btrfs_write(file, 0, original, sizeof(original)) == sizeof(original));
    struct aurafs_snapshot a, b;
    uint64_t tail = bm.next_free_block;
    int before_writes = writes;
    assert(aurafs_snapshot_create(&a) == 0);
    assert(bm.next_free_block == tail && writes == before_writes);
    assert(aurafs_snapshot_create(NULL) != 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, original, sizeof(out)) == 0);
    assert(btrfs_write(file, 0, replacement, sizeof(replacement)) == sizeof(replacement));
    assert(aurafs_snapshot_create(&b) == 0);
    assert(b.root_lba != a.root_lba);
    assert(btrfs_read(file, 0, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, replacement, sizeof(out)) == 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, original, sizeof(out)) == 0);
    assert(btrfs_truncate(file, 321) == 0);
    assert(btrfs_rename(NULL, "snapdir/file", "snapdir/renamed") == 0);
    assert(btrfs_unlink(NULL, "snapdir/renamed") == 0);
    assert(!btrfs_lookup(NULL, "snapdir/file"));
    assert(!btrfs_lookup(NULL, "snapdir/renamed"));
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, original, sizeof(out)) == 0);
    assert(aurafs_snapshot_read(&b, "snapdir/file", 0, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, replacement, sizeof(out)) == 0);
    assert(aurafs_snapshot_read(&a, "snapdir/renamed", 0, out, sizeof(out)) < 0);
    assert(aurafs_snapshot_read(&a, "snapdir", 0, out, sizeof(out)) < 0);
    assert(aurafs_snapshot_read(&a, "missing", 0, out, sizeof(out)) < 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 8990, out, UINT64_MAX) == 10);
    assert(memcmp(out, original + 8990, 10) == 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", UINT64_MAX, out, 10) == 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, NULL, 10) < 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, NULL, 0) == 0);

    /* Reusing a live path must not redirect the historical namespace. */
    struct vnode *fresh = btrfs_create(NULL, "snapdir/file");
    assert(fresh && btrfs_write(fresh, 0, "new", 3) == 3);
    assert(btrfs_read(fresh, 0, out, sizeof(out)) == 3);
    assert(memcmp(out, "new", 3) == 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, original, sizeof(out)) == 0);

    /* Reading an old data block still verifies its SHA-256 trailer. */
    uint64_t parent, ino; int found;
    assert(btrfs_path_resolve_at(a.root_lba, "snapdir/file", &parent, &ino,
                                &found, NULL, 0) == 0 && found);
    uint64_t data_lba = extent_get_at(a.root_lba, ino, 0);
    uint8_t block[BTRFS_NODE_SIZE];
    assert(btrfs_read_block(data_lba, block) == 0);
    block[BTRFS_HDR_SIZE] ^= 1;
    assert(btrfs_write_block(data_lba, block) == 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, out, sizeof(out)) < 0);
    block[BTRFS_HDR_SIZE] ^= 1;
    assert(btrfs_write_block(data_lba, block) == 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, out, sizeof(out)) == sizeof(out));

    struct aurafs_snapshot released = a;
    aurafs_snapshot_release(&released);
    assert(aurafs_snapshot_read(&released, "snapdir/file", 0, out, 10) < 0);
    aurafs_snapshot_release(NULL);
    assert(aurafs_snapshot_read(NULL, "snapdir/file", 0, out, 10) < 0);
    assert(aurafs_snapshot_read(&a, "snapdir/file", 0, out, 10) < 0);
    stale_snapshot = b;
    aurafs_snapshot_release(&a); aurafs_snapshot_release(&b);
    puts("AuraFS M6b: PASS (zero-copy snapshots, multi-block old/new versions, truncate/rename/unlink, bounds, checksum, release)");
}

static void test_snapshot_registry(void) {
    struct aurafs_snapshot tokens[AURAFS_MAX_SNAPSHOTS], roots[AURAFS_MAX_SNAPSHOTS];
    struct aurafs_snapshot extra, before;
    uint8_t out[16];
    assert(aurafs_snapshot_roots(NULL, 0) == 0);
    uint64_t cursor = bm.next_free_block, root = bm.tree_root_lba;
    int saved_writes = writes;
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++) {
        assert(aurafs_snapshot_create(&tokens[i]) == 0);
        assert(tokens[i].registration_id);
    }
    memset(&extra, 0x5a, sizeof(extra)); before = extra;
    assert(aurafs_snapshot_create(&extra) < 0);
    assert(memcmp(&extra, &before, sizeof(extra)) == 0);
    before = tokens[0];
    assert(aurafs_snapshot_create(&tokens[0]) < 0);
    assert(memcmp(&tokens[0], &before, sizeof(before)) == 0);
    memset(roots, 0xa5, sizeof(roots));
    struct aurafs_snapshot untouched = roots[0];
    assert(aurafs_snapshot_roots(roots, AURAFS_MAX_SNAPSHOTS - 1) < 0);
    assert(memcmp(&roots[0], &untouched, sizeof(untouched)) == 0);
    assert(aurafs_snapshot_roots(NULL, 1) < 0);
    assert(aurafs_snapshot_roots(roots, AURAFS_MAX_SNAPSHOTS) == AURAFS_MAX_SNAPSHOTS);
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++) {
        assert(memcmp(&roots[i], &tokens[i], sizeof(tokens[i])) == 0);
        for (unsigned j = 0; j < i; j++)
            assert(tokens[i].registration_id != tokens[j].registration_id);
    }
    /* Mutated fields cannot read or unregister a real root. */
    extra = tokens[0]; extra.root_lba += BTRFS_NODE_SIZE;
    assert(aurafs_snapshot_read(&extra, "snapdir/file", 0, out, sizeof(out)) < 0);
    aurafs_snapshot_release(&extra);
    assert(aurafs_snapshot_roots(NULL, 0) == AURAFS_MAX_SNAPSHOTS);
    extra = tokens[0]; extra.allocation_end += BTRFS_NODE_SIZE;
    aurafs_snapshot_release(&extra);
    extra = tokens[0]; extra.mount_epoch++;
    aurafs_snapshot_release(&extra);
    extra = tokens[0]; extra.registration_id = 0;
    aurafs_snapshot_release(&extra);
    assert(aurafs_snapshot_roots(NULL, 0) == AURAFS_MAX_SNAPSHOTS);
    /* Enumeration/copies are aliases, not extra references. */
    struct aurafs_snapshot stale = tokens[0];
    aurafs_snapshot_release(&roots[0]);
    assert(aurafs_snapshot_roots(NULL, 0) == AURAFS_MAX_SNAPSHOTS - 1);
    assert(aurafs_snapshot_read(&tokens[0], "snapdir/file", 0, out, sizeof(out)) < 0);
    assert(aurafs_snapshot_create(&tokens[0]) == 0);
    assert(tokens[0].registration_id != stale.registration_id);
    aurafs_snapshot_release(&stale); /* stale alias must not free reused slot */
    assert(aurafs_snapshot_roots(NULL, 0) == AURAFS_MAX_SNAPSHOTS);
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++) {
        aurafs_snapshot_release(&tokens[i]); aurafs_snapshot_release(&tokens[i]);
    }
    assert(aurafs_snapshot_roots(NULL, 0) == 0);
    /* ID exhaustion fails closed and leaves output untouched. */
    uint64_t saved_id = aurafs_snapshot_id;
    aurafs_snapshot_id = UINT64_MAX;
    before = extra;
    assert(aurafs_snapshot_create(&extra) < 0);
    assert(memcmp(&extra, &before, sizeof(extra)) == 0);
    aurafs_snapshot_id = saved_id; /* test injection only */
    assert(bm.next_free_block == cursor && bm.tree_root_lba == root && writes == saved_writes);
    puts("AuraFS M6g: PASS (bounded registered roots, saturation, aliases, stale IDs, malformed tokens, no I/O, exhaustion)");
}

static void test_reachability(void) {
    struct aurafs_reachability report, unchanged;
    assert(aurafs_reachability_audit(NULL) < 0);
    assert(aurafs_reachability_audit(&report) == 0);
    assert(report.roots == 2 && report.metadata_blocks && report.data_blocks);
    assert(report.allocated_blocks == report.metadata_blocks + report.data_blocks + report.unreferenced_blocks);
    uint64_t saved_root = bm.tree_root_lba;
    uint64_t data = blk_alloc(), leaf = blk_alloc(), root = blk_alloc();
    uint8_t *bytes = calloc(1, BTRFS_NODE_SIZE), *original = calloc(1, BTRFS_NODE_SIZE);
    assert(bytes && original);
    assert(blk_write(data, bytes, BT_DATA, 42) == 0);
    memset(bytes, 0, BTRFS_NODE_SIZE);
    w32(bytes + 32, 1);
    w64(bytes + 36, 42); w64(bytes + 44, 0);
    w32(bytes + 52, BTRFS_ITYPE_EXTENT);
    w32(bytes + 56, BTRFS_CSUM_OFFSET - EXTENT_REC_SIZE);
    w32(bytes + 60, EXTENT_REC_SIZE);
    w64(bytes + BTRFS_CSUM_OFFSET - EXTENT_REC_SIZE, data);
    w64(bytes + BTRFS_CSUM_OFFSET - 8, DATA_PAYLOAD);
    assert(blk_write(leaf, bytes, BT_LEAF, 1) == 0);
    struct root_ptr ptr = {42, 0, BTRFS_ITYPE_EXTENT, leaf};
    memset(bytes, 0, BTRFS_NODE_SIZE); root_set_ptrs(bytes, 1, &ptr);
    assert(blk_write(root, bytes, BT_ROOT, 1) == 0);
    bm.tree_root_lba = root;
    struct aurafs_snapshot pin;
    assert(aurafs_snapshot_create(&pin) == 0);
    assert(aurafs_reachability_audit(&report) == 0 && report.roots == 3);
    assert(report.metadata_blocks >= 2 && report.data_blocks >= 1);
    bm.tree_root_lba = saved_root;
    assert(aurafs_reachability_audit(&report) == 0);
    uint64_t pinned = report.metadata_blocks + report.data_blocks;
    aurafs_snapshot_release(&pin);
    assert(aurafs_reachability_audit(&report) == 0);
    assert(report.metadata_blocks + report.data_blocks == pinned - 3);
    bm.tree_root_lba = root;
    uint64_t before_cursor = bm.next_free_block;
    unchanged = report;
    audit_oom = 1;
    assert(aurafs_reachability_audit(&report) < 0); audit_oom = 0;
    assert(memcmp(&report, &unchanged, sizeof(report)) == 0);
    assert(btrfs_read_super(BTRFS_SUPER_OFFSET, original) == 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE); bytes[80] ^= 1;
    assert(btrfs_write_block(BTRFS_SUPER_OFFSET, bytes) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    assert(btrfs_write_block(BTRFS_SUPER_OFFSET, original) == 0);
    assert(memcmp(&report, &unchanged, sizeof(report)) == 0);
    bm.next_free_block = (AURAFS_AUDIT_MAX_BLOCKS + 1ULL) * BTRFS_NODE_SIZE;
    assert(aurafs_reachability_audit(&report) < 0);
    bm.next_free_block = before_cursor;
    /* All malformed structures are re-sealed: checksum alone isn't enough. */
    assert(btrfs_read_block(root, original) == 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE);
    w64(bytes + 60, root); /* cycle */
    assert(blk_write(root, bytes, BT_ROOT, 1) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    w64(bytes + 60, before_cursor); /* out of range */
    assert(blk_write(root, bytes, BT_ROOT, 1) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    w64(bytes + 60, leaf + 1); /* unaligned */
    assert(blk_write(root, bytes, BT_ROOT, 1) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE); w32(bytes + 32, UINT32_MAX);
    assert(blk_write(root, bytes, BT_ROOT, 1) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    assert(btrfs_write_block(root, original) == 0);
    assert(btrfs_read_block(leaf, original) == 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE); w32(bytes + 56, 36);
    assert(blk_write(leaf, bytes, BT_LEAF, 1) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE); w32(bytes + 60, UINT32_MAX);
    assert(blk_write(leaf, bytes, BT_LEAF, 1) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE);
    w64(bytes + BTRFS_CSUM_OFFSET - EXTENT_REC_SIZE, root); /* metadata as data */
    assert(blk_write(leaf, bytes, BT_LEAF, 1) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    assert(btrfs_write_block(leaf, original) == 0);
    assert(btrfs_read_block(data, original) == 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE); bytes[BTRFS_HDR_SIZE] ^= 1;
    assert(btrfs_write_block(data, bytes) == 0); /* SHA mismatch */
    assert(aurafs_reachability_audit(&report) < 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE);
    assert(blk_write(data, bytes, BT_DATA, 43) == 0); /* wrong owner */
    assert(aurafs_reachability_audit(&report) < 0);
    memcpy(bytes, original, BTRFS_NODE_SIZE); w64(bytes + 24, data + BTRFS_NODE_SIZE);
    ksha256(bytes, BTRFS_CSUM_OFFSET, bytes + BTRFS_CSUM_OFFSET);
    assert(btrfs_write_block(data, bytes) == 0); /* wrong self, valid hash */
    assert(aurafs_reachability_audit(&report) < 0);
    assert(btrfs_write_block(data, original) == 0);
    int before_writes = writes;
    assert(aurafs_reachability_audit(&report) == 0);
    assert(writes == before_writes && bm.next_free_block == before_cursor);
    /* Depth limit is checked iteratively, without kernel-stack recursion. */
    uint64_t child = leaf;
    for (unsigned i = 0; i <= BTRFS_MAX_LEVELS; i++) {
        uint64_t parent = blk_alloc();
        ptr.child_lba = child;
        memset(bytes, 0, BTRFS_NODE_SIZE); root_set_ptrs(bytes, 1, &ptr);
        assert(blk_write(parent, bytes, BT_ROOT, 1) == 0);
        child = parent;
    }
    bm.tree_root_lba = child;
    assert(aurafs_reachability_audit(&report) < 0);
    bm.tree_root_lba = saved_root;
    free(bytes); free(original);
    puts("AuraFS M6h: PASS (live/checkpoint/snapshot union, pin/release, no writes, bounded allocation, corruption/cycle rejection)");
}

static int checkpoint_has(uint64_t root, uint64_t end) {
    for (unsigned i = 0; i < aurafs_checkpoint_count; i++)
        if (aurafs_checkpoint_history[i].root_lba == root &&
            aurafs_checkpoint_history[i].allocation_end == end) return 1;
    return 0;
}
static void test_checkpoint_barriers(void) {
    struct vnode *file = btrfs_create(NULL, "checkpoint-barriers");
    assert(file); file->vfs_refs++;
    assert(btrfs_sync(NULL) == 0); /* unsupported fallback */
    flush_enabled = 1;
    for (unsigned failure = 0; failure < 4; failure++) {
        uint8_t value = (uint8_t)failure;
        assert(btrfs_write(file, 0, &value, 1) == 1);
        uint64_t gen = bm.generation;
        flush_calls = checkpoint_seen = backend_super_reads = 0;
        fail_flush_at = failure == 1 ? 1 : failure == 2 ? 2 : 0;
        change_flush_at = failure == 3 ? 2 : 0;
        int rc = btrfs_sync(NULL);
        if (failure) {
            assert(rc < 0 && bm.generation == gen && !backend_super_reads);
            if (failure == 1) assert(!checkpoint_seen && flush_calls == 1);
            else assert(checkpoint_seen == 1 && flush_calls == 2);
            fail_flush_at = change_flush_at = 0;
            flush_calls = checkpoint_seen = 0;
            assert(btrfs_sync(NULL) == 0);
        } else assert(rc == 0);
        assert(bm.generation == gen + 1 && flush_calls == 2);
        assert(super_writes_at_flush[0] == 0 && super_writes_at_flush[1] == 1);
        assert(backend_super_reads == 1 && flushes_at_readback == 2);
        assert(checkpoint_has(bm.tree_root_lba, bm.next_free_block));
    }
    flush_enabled = 0;
    flush_calls = 0;
    assert(btrfs_sync(NULL) == 0 && flush_calls == 2); /* still weak fallback */
    file->vfs_refs--;
    puts("AuraFS M6m: PASS (pre/post device barriers, ordering, failures, capability change, weak unsupported path, retry/history)");
}

static void test_checkpoint_readback(void) {
    struct vnode *file = btrfs_create(NULL, "checkpoint-readback");
    assert(file);
    file->vfs_refs++;
    assert(btrfs_write(file, 0, "0", 1) == 1 && btrfs_sync(NULL) == 0);
    for (unsigned mode = 0; mode < 4; mode++) {
        uint8_t value = (uint8_t)('1' + mode);
        uint8_t before[BTRFS_NODE_SIZE], cached[BTRFS_NODE_SIZE];
        memcpy(before, disk + BTRFS_SUPER_OFFSET, sizeof(before));
        assert(btrfs_write(file, 0, &value, 1) == 1);
        uint64_t generation = bm.generation, root = bm.tree_root_lba;
        uint64_t end = bm.next_free_block;
        drop_super_write = mode == 0;
        partial_super_write = mode == 1;
        alter_super_write = mode == 2;
        fail_super_read = mode == 3;
        backend_super_reads = 0;
        assert(btrfs_sync(NULL) < 0);
        assert(backend_super_reads == 1); /* bypasses a warm super cache */
        assert(bm.generation == generation);
        assert(checkpoint_has(root, end));
        if (mode == 0) assert(!memcmp(before, disk + BTRFS_SUPER_OFFSET, sizeof(before)));
        if (mode == 2)
            assert(aurafs_validate_super(disk + BTRFS_SUPER_OFFSET, DISK_SIZE) == 0);
        /* A normal FS read still sees the exact submitted image in cache. */
        assert(btrfs_read_super(BTRFS_SUPER_OFFSET, cached) == 0);
        assert(r64(cached + 64) == root && r64(cached + 72) == end);
        if (mode != 3) assert(memcmp(cached, disk + BTRFS_SUPER_OFFSET, sizeof(cached)));
        drop_super_write = partial_super_write = alter_super_write = fail_super_read = 0;
        unsigned count = aurafs_checkpoint_count;
        backend_super_reads = 0;
        assert(btrfs_sync(NULL) == 0);
        assert(backend_super_reads == 1 && bm.generation == generation + 1);
        assert(aurafs_checkpoint_count == count); /* retry deduplicates, never retires */
        assert(!memcmp(cached, disk + BTRFS_SUPER_OFFSET, sizeof(cached)));
    }
    file->vfs_refs--;
    puts("AuraFS M6l: PASS (backend bypass, dropped/partial/resealed writes, read failure, no premature generation, retained history, retry)");
}

static void test_checkpoint_history(void) {
    struct vnode *file = btrfs_create(NULL, "checkpoint-history");
    assert(file && btrfs_write(file, 0, "old", 3) == 3);
    assert(btrfs_sync(NULL) == 0);
    uint64_t old_root = bm.tree_root_lba, old_end = bm.next_free_block;
    unsigned count = aurafs_checkpoint_count;
    assert(checkpoint_has(old_root, old_end));
    assert(btrfs_sync(NULL) == 0 && aurafs_checkpoint_count == count);
    assert(btrfs_write(file, 0, "new", 3) == 3);
    uint64_t failed_root = bm.tree_root_lba, failed_end = bm.next_free_block;
    uint8_t super[BTRFS_NODE_SIZE];
    memcpy(super, disk + BTRFS_SUPER_OFFSET, sizeof(super));
    fail_checkpoint = 1;
    assert(btrfs_sync(NULL) < 0);
    fail_checkpoint = 0;
    assert(memcmp(super, disk + BTRFS_SUPER_OFFSET, sizeof(super)) == 0);
    assert(checkpoint_has(old_root, old_end) && checkpoint_has(failed_root, failed_end));
    assert(aurafs_checkpoint_count == count + 1);
    /* Cache holds the attempted super, but history still includes old root. */
    assert(btrfs_read_super(BTRFS_SUPER_OFFSET, super) == 0);
    assert(r64(super + 64) == failed_root);
    struct aurafs_reachability report;
    assert(aurafs_reachability_audit(&report) == 0);
    assert(report.roots >= 3);
    assert(btrfs_write(file, 0, "end", 3) == 3);
    assert(btrfs_sync(NULL) == 0);
    assert(checkpoint_has(old_root, old_end) && checkpoint_has(failed_root, failed_end));
    assert(checkpoint_has(bm.tree_root_lba, bm.next_free_block));
    assert(aurafs_reachability_audit(&report) == 0);
    /* Observe an actual old-generation data block only history now retains. */
    uint64_t parent, ino; int found;
    assert(btrfs_path_resolve_at(old_root, "checkpoint-history", &parent, &ino, &found, NULL, 0) == 0 && found);
    uint64_t data = extent_get_at(old_root, ino, 0);
    uint8_t bytes[BTRFS_NODE_SIZE];
    assert(btrfs_read_block(data, bytes) == 0);
    bytes[BTRFS_HDR_SIZE] ^= 1;
    assert(btrfs_write_block(data, bytes) == 0);
    assert(aurafs_reachability_audit(&report) < 0);
    bytes[BTRFS_HDR_SIZE] ^= 1;
    assert(btrfs_write_block(data, bytes) == 0);
    assert(aurafs_reachability_audit(&report) == 0);
    /* Saturation disables diagnostics, not normal persistence attempts. */
    for (unsigned i = 0; i <= AURAFS_CHECKPOINT_HISTORY; i++) {
        uint8_t value = (uint8_t)i;
        assert(btrfs_write(file, 0, &value, 1) == 1);
        assert(btrfs_sync(NULL) == 0);
    }
    assert(aurafs_checkpoint_count == AURAFS_CHECKPOINT_HISTORY && aurafs_checkpoint_overflow);
    struct aurafs_reachability before = report;
    assert(aurafs_reachability_audit(&report) < 0);
    assert(memcmp(&report, &before, sizeof(report)) == 0);
    assert(btrfs_sync(NULL) == 0 && aurafs_checkpoint_overflow);
    puts("AuraFS M6i: PASS (mounted/attempted checkpoint roots, failed flush/cache divergence, old data coverage, dedup, saturation)");
}

static void test_open_vnode_identity(void) {
    assert(btrfs_test_open_identity() == 0);
    struct vnode *old = btrfs_create(NULL, "m6j-pressure");
    assert(old);
    old->vfs_refs++;
    uint64_t ino = old->inode_id;
    assert(btrfs_write(old, 0, "PIN", 3) == 3);
    assert(btrfs_unlink(NULL, "m6j-pressure") == 0);
    for (unsigned i = 0; i < BTRFS_MAX_OPEN_VNODES * 2; i++) {
        char path[32]; snprintf(path, sizeof(path), "pressure-%u", i);
        assert(bv_intern(path, 100000 + i, 0, 0));
        assert(old->inode_id == ino && old->vfs_refs == 1);
    }
    char bytes[8];
    assert(btrfs_read(old, 0, bytes, sizeof(bytes)) == 3 && !memcmp(bytes, "PIN", 3));
    /* Pin every slot, including inactive ones: no free-slot shortcut may
     * reset a still-referenced vnode. No filesystem operation is involved. */
    for (unsigned i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) bv4pool[i].vnode.vfs_refs++;
    uint64_t ids[BTRFS_MAX_OPEN_VNODES];
    for (unsigned i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) ids[i] = bv4pool[i].objectid;
    assert(!bv_intern("all-pinned", 900000, 0, 0));
    for (unsigned i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) {
        assert(ids[i] == bv4pool[i].objectid);
        bv4pool[i].vnode.vfs_refs--;
    }
    old->vfs_refs--;
    /* Simulate the last OFD dropping its existing VFS reference; cache
     * reuse needs no added close callback. This is not a VFS dup/fork test. */
    for (unsigned i = 0; i < BTRFS_MAX_OPEN_VNODES; i++) bv4pool[i].vnode.vfs_refs++;
    old->vfs_refs--;
    struct btrfs_vinfo *reused = bv_intern("released-slot", 900001, 0, 0);
    assert(reused && &reused->vnode == old && !old->vfs_refs);
    for (unsigned i = 0; i < BTRFS_MAX_OPEN_VNODES; i++)
        if (&bv4pool[i] != reused) bv4pool[i].vnode.vfs_refs--;
    struct aurafs_reachability report;
    assert(aurafs_reachability_audit(&report) == 0);
    puts("AuraFS M6j: PASS (pinned identity, unlink/recreate, rename/hardlink aliases, slot pressure/exhaustion, final-reference reuse)");
}

static void test_batched_writes(void) {
    enum { LENGTH = (AURAFS_EDIT_MAX + 3) * DATA_PAYLOAD + 17 };
    static uint8_t first[LENGTH], next[LENGTH], out[LENGTH];
    for (unsigned i = 0; i < LENGTH; i++) {
        first[i] = (uint8_t)(i * 11 + 7); next[i] = (uint8_t)(i * 13 + 5);
    }
    struct vnode *file = btrfs_create(NULL, "batch-large");
    assert(file);
    uint64_t fallback = aurafs_verify_stats.batch_fallback;
    assert(btrfs_write(file, 0, first, LENGTH) == LENGTH);
    assert(btrfs_read(file, 0, out, LENGTH) == LENGTH);
    assert(memcmp(first, out, LENGTH) == 0);
    struct aurafs_snapshot old;
    assert(aurafs_snapshot_create(&old) == 0);
    assert(btrfs_write(file, 0, next, LENGTH) == LENGTH);
    assert(btrfs_read(file, 0, out, LENGTH) == LENGTH);
    assert(memcmp(next, out, LENGTH) == 0);
    assert(aurafs_snapshot_read(&old, "batch-large", 0, out, LENGTH) == LENGTH);
    assert(memcmp(first, out, LENGTH) == 0);

    /* A one-block budget admits data but not its metadata. Publish nothing. */
    uint64_t root = bm.tree_root_lba, max = bm.max_block;
    bm.max_block = bm.next_free_block + BTRFS_NODE_SIZE;
    assert(btrfs_write(file, 0, first, 100) < 0);
    assert(bm.tree_root_lba == root && file->size == LENGTH);
    bm.max_block = max;
    assert(btrfs_read(file, 0, out, LENGTH) == LENGTH);
    assert(memcmp(next, out, LENGTH) == 0);
    assert(btrfs_write(file, 0, first, 100) == 100);
    assert(btrfs_read(file, 0, out, LENGTH) == LENGTH);
    assert(memcmp(first, out, 100) == 0 && memcmp(next + 100, out + 100, LENGTH - 100) == 0);

    /* Grow the tree, then choose the extreme keys of distinct leaves. */
    for (int i = 0; i < 96; i++) {
        char name[32]; snprintf(name, sizeof(name), "batch-filler-%d", i);
        assert(btrfs_create(NULL, name));
    }
    uint8_t records[2][INODE_REC_SIZE];
    struct tree_edit edits[2];
    for (int i = 0; i < 2; i++) {
        uint64_t path[BTRFS_MAX_LEVELS]; int ci[BTRFS_MAX_LEVELS], depth;
        assert(tree_descend(bm.tree_root_lba, i ? UINT64_MAX : 0,
                            i ? UINT32_MAX : 0, i ? UINT64_MAX : 0,
                            path, ci, &depth, bdatabuf) == 0 && depth > 0);
        uint32_t off, size;
        int index = i ? leaf_nslots(bdatabuf) - 1 : 0;
        leaf_get(bdatabuf, index, &edits[i].obj, &edits[i].typ, &edits[i].off,
                  &off, &size);
        assert(size <= sizeof(records[i]));
        memcpy(records[i], bdatabuf + off, size);
        edits[i].size = size; edits[i].data = records[i];
    }
    uint64_t candidate;
    fallback = aurafs_verify_stats.batch_fallback;
    assert(tree_insert_many(bm.tree_root_lba, edits, 2, &candidate) == 0);
    assert(aurafs_verify_stats.batch_fallback > fallback);
#if AURAFS_BATCH_WRITES
    assert(aurafs_verify_stats.batch_fast > 0);
#else
    assert(aurafs_verify_stats.batch_fast == 0);
#endif
    aurafs_snapshot_release(&old);
    puts("AuraFS M6e: PASS (bounded multi-chunk batches, cross-leaf fallback, byte-exact overwrite/snapshot, unpublished failed metadata, retry)");
}

static void dual_payload(const char *expected) {
    struct vnode *v = btrfs_lookup(NULL, "dual-file");
    char data[8];
    assert(v && btrfs_read(v, 0, data, sizeof(data)) == 3 && !memcmp(data, expected, 3));
}
static void test_dual_checkpoints(void) {
    uint8_t a[BTRFS_NODE_SIZE], b[BTRFS_NODE_SIZE];
    /* A fresh v2 volume requires BOTH explicit permission and device barrier. */
    bc_init(); memset(disk, 0, sizeof(disk));
    allow_format = allow_dual = 1; flush_enabled = 0;
    int before = writes;
    assert(btrfs_init(0) < 0 && writes == before);
    flush_enabled = 1;
    disk[DISK_SIZE - 1] = 1;
    assert(btrfs_init(0) < 0 && writes == before); /* full-device zero test */
    disk[DISK_SIZE - 1] = 0;
    assert(btrfs_init(0) == 0 && bm.dual && bm.active_super == BTRFS_SUPER_OFFSET);
    assert(!memcmp(disk + BTRFS_SUPER_OFFSET + 128, AURAFS_DUAL_MAGIC, 8));
    assert(aurafs_validate_super(disk + AURAFS_MIRROR_OFFSET, DISK_SIZE) == 0);
    struct vnode *v = btrfs_create(NULL, "dual-file");
    assert(v && btrfs_write(v, 0, "OLD", 3) == 3);
    assert(btrfs_sync(NULL) == 0 && bm.active_super == AURAFS_MIRROR_OFFSET);
    assert(btrfs_sync(NULL) == 0 && bm.active_super == BTRFS_SUPER_OFFSET);
    memcpy(a, disk + BTRFS_SUPER_OFFSET, sizeof(a));
    uint64_t old_end = bm.next_free_block;
    const unsigned cuts[] = {0, 512, 2048, 3584};
    for (unsigned cut = 0; cut < sizeof(cuts) / sizeof(cuts[0]); cut++) {
        v = btrfs_lookup(NULL, "dual-file");
        assert(v && btrfs_write(v, 0, "NEW", 3) == 3);
        partial_checkpoint_bytes = cuts[cut]; partial_super_write = 1;
        assert(btrfs_sync(NULL) < 0 && bm.active_super == BTRFS_SUPER_OFFSET);
        partial_super_write = 0; partial_checkpoint_bytes = 512;
        before = writes; bc_init(); allow_format = allow_dual = 0;
        assert(btrfs_init(0) == 0 && bm.dual && bm.next_free_block == old_end);
        assert(writes == before); dual_payload("OLD");
    }
    v = btrfs_lookup(NULL, "dual-file");
    assert(v && btrfs_write(v, 0, "NEW", 3) == 3);
    assert(btrfs_sync(NULL) == 0 && bm.active_super == AURAFS_MIRROR_OFFSET);
    memcpy(b, disk + AURAFS_MIRROR_OFFSET, sizeof(b));
    before = writes; bc_init();
    assert(btrfs_init(0) == 0 && bm.active_super == AURAFS_MIRROR_OFFSET);
    assert(writes == before); dual_payload("NEW");
    struct aurafs_reachability report;
    assert(aurafs_reachability_audit(&report) == 0 && report.roots >= 3);
    /* A corrupt latest checkpoint falls back without repair/reformat. */
    disk[AURAFS_MIRROR_OFFSET + 80] ^= 1; bc_init();
    allow_format = allow_dual = 1;
    assert(btrfs_init(0) == 0 && bm.active_super == BTRFS_SUPER_OFFSET);
    assert(writes == before); dual_payload("OLD");
    memcpy(disk + AURAFS_MIRROR_OFFSET, b, sizeof(b));
    disk[BTRFS_SUPER_OFFSET + 80] ^= 1; bc_init();
    assert(btrfs_init(0) == 0 && bm.active_super == AURAFS_MIRROR_OFFSET);
    assert(writes == before); dual_payload("NEW");
    disk[AURAFS_MIRROR_OFFSET + 80] ^= 1; bc_init();
    assert(btrfs_init(0) < 0 && writes == before); /* both corrupt, format allowed */
    /* A slot copied verbatim into the wrong physical slot is not valid. */
    memcpy(disk + AURAFS_MIRROR_OFFSET, a, sizeof(a)); bc_init();
    assert(btrfs_init(0) < 0 && writes == before);
    /* Same generation but divergent valid bodies is ambiguous, fail closed. */
    memcpy(disk + BTRFS_SUPER_OFFSET, a, sizeof(a));
    memcpy(disk + AURAFS_MIRROR_OFFSET, b, sizeof(b));
    w64(disk + AURAFS_MIRROR_OFFSET + 56, r64(a + 56));
    w32(disk + AURAFS_MIRROR_OFFSET + 8, r32(a + 8));
    seal(disk + AURAFS_MIRROR_OFFSET); bc_init();
    assert(btrfs_init(0) < 0 && writes == before);
    /* Wrapped generation is invalid, not the newest possible checkpoint. */
    memcpy(disk + AURAFS_MIRROR_OFFSET, b, sizeof(b));
    w64(disk + AURAFS_MIRROR_OFFSET + 56, UINT64_MAX);
    w32(disk + AURAFS_MIRROR_OFFSET + 8, UINT32_MAX);
    seal(disk + AURAFS_MIRROR_OFFSET); bc_init();
    assert(btrfs_init(0) == 0 && bm.active_super == BTRFS_SUPER_OFFSET);
    assert(writes == before); dual_payload("OLD");
    /* Unsupported barrier on an existing v2 never becomes weak write success. */
    memcpy(disk + AURAFS_MIRROR_OFFSET, b, sizeof(b)); bc_init();
    assert(btrfs_init(0) == 0); flush_enabled = 0;
    assert(btrfs_sync(NULL) < 0 && writes == before);
    flush_enabled = 1;
    bm.generation = UINT64_MAX;
    assert(btrfs_sync(NULL) < 0 && writes == before);
    /* A failed post-publication barrier is ambiguous: a complete new slot
     * can already be visible. Do not promise failed-sync rollback. */
    bc_init(); assert(btrfs_init(0) == 0);
    v = btrfs_lookup(NULL, "dual-file");
    assert(v && btrfs_write(v, 0, "AMB", 3) == 3);
    flush_calls = 0; fail_flush_at = 2;
    assert(btrfs_sync(NULL) < 0);
    fail_flush_at = 0; before = writes; bc_init();
    assert(btrfs_init(0) == 0 && writes == before); dual_payload("AMB");
    puts("AuraFS M6n: PASS (explicit zero-volume v2, barrier requirement, alternating slots, torn write/cache-drop fallback, latest selection, ambiguity/corruption refusal, no repair/migration)");
}

/* This is an immediate-write RAM backend. Cache drop discards dirty blocks;
 * barrier failure may still leave valid bytes, deliberately not rollback. */
static void format_fault_reset(void) {
    bc_init(); memset(disk, 0, sizeof(disk));
    allow_format = allow_dual = flush_enabled = 1;
    flush_calls = fail_flush_at = change_flush_at = 0;
    checkpoint_seen = format_write_count = format_readbacks = 0;
    format_write_at = format_readback_at = format_write_mode = 0;
    fail_appends = fail_checkpoint = fail_super_read = 0;
    drop_super_write = partial_super_write = alter_super_write = 0;
}
static int format_valid_slots(void) {
    return (aurafs_validate_super(disk + BTRFS_SUPER_OFFSET, DISK_SIZE) == 0) +
           (aurafs_validate_super(disk + AURAFS_MIRROR_OFFSET, DISK_SIZE) == 0);
}
static void format_fault_recover(int result) {
    int valid = format_valid_slots();
    assert(result < 0 || valid == 2);
    if (result < 0) assert(!bm.mounted);
    format_write_at = format_readback_at = fail_flush_at = 0;
    int before = writes;
    /* Zero filesystem-cache state, not a clean unmount or cache flush. */
    bc_init(); allow_format = allow_dual = 0;
    int recovered = btrfs_init(0);
    assert((recovered == 0) == (valid > 0));
    assert(writes == before);
    if (!valid) {
        int nonzero = 0;
        for (unsigned i = 0; i < sizeof(disk); i++) nonzero |= disk[i];
        bc_init(); allow_format = allow_dual = 1;
        int retry = btrfs_init(0);
        if (nonzero) {
            assert(retry < 0 && !bm.mounted && writes == before);
            return; /* explicit wipe/provisioning needed, no silent repair */
        }
        assert(retry == 0 && format_valid_slots() == 2);
    }
    assert(bm.dual && bm.mounted);
    /* Valid super receipts alone aren't enough: traverse and extend tree. */
    struct vnode *v = btrfs_create(NULL, "format-recovered");
    assert(v && btrfs_write(v, 0, "OK!", 3) == 3);
    assert(btrfs_sync(NULL) == 0);
    bc_init(); allow_format = allow_dual = 0; before = writes;
    assert(btrfs_init(0) == 0 && writes == before);
    char out[4]; v = btrfs_lookup(NULL, "format-recovered");
    assert(v && btrfs_read(v, 0, out, sizeof(out)) == 3 && !memcmp(out, "OK!", 3));
}
static void test_dual_format_interruptions(void) {
    format_fault_reset();
    assert(btrfs_init(0) == 0 && format_valid_slots() == 2);
    int total_writes = format_write_count, total_flushes = flush_calls;
    int total_reads = format_readbacks;
    assert(total_writes == 3 && total_flushes == 4 && total_reads >= 2);
    unsigned cases = 0;
    for (int at = 1; at <= total_writes; at++) {
        format_fault_reset(); format_write_at = at; format_write_mode = 1;
        int rc = btrfs_init(0);
        /* Range flush retries a one-shot rejected write within its bounded
         * pass loop, but retains the failure result: bytes may be valid. */
        assert(rc < 0 && format_valid_slots() == (at - 1));
        format_fault_recover(rc); cases++;
    }
    /* Partial accepted writes only target checkpoints: data-tree silent
     * corruption is outside the checkpoint-copy recovery guarantee. */
    const unsigned cuts[] = {0, 512, 2048, 3584};
    for (int slot = 0; slot < 2; slot++) {
        for (unsigned c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
            format_fault_reset(); format_write_at = total_writes - 1 + slot;
            format_write_mode = 2; format_write_bytes = cuts[c];
            int rc = btrfs_init(0);
            assert(rc < 0 && format_valid_slots() == slot);
            format_fault_recover(rc); cases++;
        }
    }
    for (int at = 1; at <= total_flushes; at++) {
        format_fault_reset(); fail_flush_at = at;
        int rc = btrfs_init(0);
        assert(rc < 0 && format_valid_slots() == (at > 2 ? at - 2 : 0));
        format_fault_recover(rc); cases++;
    }
    for (int at = 1; at <= 2; at++) {
        format_fault_reset(); format_readback_at = at;
        int rc = btrfs_init(0);
        assert(rc < 0 && format_valid_slots() == at);
        format_fault_recover(rc); cases++;
    }
    printf("AuraFS M6o: PASS (%u initial-format fault cases, %d writes, %d barriers; no-copy refusal, surviving-copy recovery, explicit zero-only retry)\n",
           cases, total_writes, total_flushes);
}

static void guard_refuses(void) {
    int before = writes;
    allow_format = allow_dual = 1;
    assert(btrfs_init(0) < 0 && !bm.mounted && writes == before);
    assert(!aurafs_checkpoint_count && !aurafs_checkpoint_overflow);
}
static void test_selected_root_guard(void) {
    format_fault_reset(); assert(btrfs_init(0) == 0);
    struct vnode *v = btrfs_create(NULL, "dual-file");
    assert(v && btrfs_write(v, 0, "OLD", 3) == 3 && btrfs_sync(NULL) == 0);
    uint64_t older_root = bm.tree_root_lba;
    assert(btrfs_write(v, 0, "NEW", 3) == 3 && btrfs_sync(NULL) == 0);
    uint64_t leaf = bm.tree_root_lba;
    if (r32(disk + leaf + 12) == BT_ROOT) {
        assert(r32(disk + leaf + 32) == 1);
        leaf = r64(disk + leaf + 36 + 24);
    }
    assert(older_root != leaf && r32(disk + leaf + 12) == BT_LEAF);
    w64(disk + bm.active_super + 64, leaf); seal(disk + bm.active_super);
    bc_init(); assert(btrfs_init(0) == 0);
    uint8_t *image = malloc(DISK_SIZE); assert(image);
    memcpy(image, disk, DISK_SIZE);
    /* Warm FS + verification caches, then damage the backend without cache
     * reset: selected-root read must still see damaged stored bytes. */
    dual_payload("NEW");
    disk[leaf + 80] ^= 1; guard_refuses();
    memcpy(disk, image, DISK_SIZE); bc_init();
    guard_fail_root = leaf; unsigned reads = guard_backend_reads;
    guard_refuses(); assert(guard_backend_reads > reads); guard_fail_root = 0;
    bc_init(); audit_oom = 1; guard_refuses(); audit_oom = 0;
    /* A damaged unselected older root must not veto the selected NEW tree. */
    disk[older_root + 80] ^= 1; bc_init(); int before = writes;
    assert(btrfs_init(0) == 0 && writes == before); dual_payload("NEW");
    memcpy(disk, image, DISK_SIZE);
    for (int fault = 0; fault < 4; fault++) {
        memcpy(disk, image, DISK_SIZE); bc_init(); uint8_t *p = disk + leaf;
        if (fault == 0) w32(p + 12, BT_DATA);
        if (fault == 1) w64(p + 24, leaf + BTRFS_NODE_SIZE);
        if (fault == 2) w32(p + 32, UINT32_MAX);
        if (fault == 3) w32(p + 36 + 20, 0); /* item overlaps header/table */
        seal(p); guard_refuses();
    }
    memcpy(disk, image, DISK_SIZE); bc_init();
    assert(btrfs_init(0) == 0); dual_payload("NEW");
    /* Build a valid internal-root fixture over the existing leaf. This is
     * explicit test media editing, not a production format operation. */
    uint64_t root = bm.next_free_block;
    uint8_t *p = disk + root; memset(p, 0, BTRFS_NODE_SIZE);
    w32(p + 4, BTRFS_BLK_MAGIC); w32(p + 8, r32(disk + bm.active_super + 8));
    w32(p + 12, BT_ROOT); w64(p + 24, root); w32(p + 32, 1);
    memcpy(p + 36, disk + leaf + 36, 20); w64(p + 36 + 24, leaf); seal(p);
    w64(disk + bm.active_super + 64, root);
    w64(disk + bm.active_super + 72, root + BTRFS_NODE_SIZE); seal(disk + bm.active_super);
    memcpy(image, disk, DISK_SIZE); bc_init(); before = writes;
    assert(btrfs_init(0) == 0 && writes == before); dual_payload("NEW");
    for (int fault = 0; fault < 6; fault++) {
        memcpy(disk, image, DISK_SIZE); bc_init(); p = disk + root;
        if (fault == 0) w32(p + 32, 0);
        if (fault == 1) w32(p + 32, UINT32_MAX);
        if (fault == 2) w64(p + 36 + 24, root);
        if (fault == 3) w64(p + 36 + 24, leaf + 1);
        if (fault == 4) w64(p + 36 + 24, root + BTRFS_NODE_SIZE);
        if (fault == 5) { w32(p + 32, 2); memcpy(p + 68, p + 36, 32); }
        seal(p); guard_refuses();
    }
    /* M6s: descendants are now mandatory, even with an intact selected root. */
    memcpy(disk, image, DISK_SIZE); disk[leaf + 80] ^= 1; bc_init(); before = writes;
    guard_refuses();
    memcpy(disk, image, DISK_SIZE); bc_init();
    guard_fail_root = leaf; guard_refuses(); guard_fail_root = 0;
    /* Production budget helper: exact successful path vs explicit limits. */
    assert(aurafs_checkpoint_tree_guard_limited(disk + BTRFS_SUPER_OFFSET, 0, 100) < 0);
    assert(aurafs_checkpoint_tree_guard_limited(disk + BTRFS_SUPER_OFFSET,
                                              AURAFS_MOUNT_MAX_BLOCKS, 1) < 0);
    assert(aurafs_checkpoint_tree_guard(disk + BTRFS_SUPER_OFFSET) == 0);
    /* Descendant-local checksum-valid mutations must also fail mount. */
    for (int fault = 0; fault < 3; fault++) {
        memcpy(disk, image, DISK_SIZE); bc_init(); p = disk + leaf;
        if (fault == 0) w64(p + 24, root);
        if (fault == 1) w32(p + 12, BT_DATA);
        if (fault == 2) w32(p + 36 + 20, 0);
        seal(p); guard_refuses();
    }
    /* Repeated metadata child, two-block cycle, and depth boundary. */
    memcpy(disk, image, DISK_SIZE); p = disk + root;
    w32(p + 32, 2); memcpy(p + 68, p + 36, 32);
    w64(p + 68 + 8, r64(p + 36 + 8) + 1); seal(p); bc_init(); guard_refuses();
    for (int extra = 1; extra <= BTRFS_MAX_LEVELS; extra++) {
        memcpy(disk, image, DISK_SIZE);
        for (int j = 0; j <= extra; j++) {
            uint64_t address = root + j * BTRFS_NODE_SIZE;
            memcpy(disk + address, image + root, BTRFS_NODE_SIZE);
            w64(disk + address + 24, address);
            w64(disk + address + 36 + 24,
                j == extra ? leaf : address + BTRFS_NODE_SIZE);
            seal(disk + address);
        }
        w64(disk + BTRFS_SUPER_OFFSET + 72, root + (extra + 1) * BTRFS_NODE_SIZE);
        seal(disk + BTRFS_SUPER_OFFSET); bc_init();
        if (extra < BTRFS_MAX_LEVELS) assert(btrfs_init(0) == 0);
        else guard_refuses();
        if (extra == 1) {
            w64(disk + root + BTRFS_NODE_SIZE + 36 + 24, root);
            seal(disk + root + BTRFS_NODE_SIZE); bc_init(); guard_refuses();
        }
    }
    /* Extent addresses must not alias metadata (in either visit order). */
    unsigned extent = UINT32_MAX;
    for (unsigned i = 0; i < r32(image + leaf + 32); i++)
        if (r32(image + leaf + 36 + i * 28 + 16) == BTRFS_ITYPE_EXTENT) { extent = i; break; }
    assert(extent != UINT32_MAX);
    unsigned dd = r32(image + leaf + 36 + extent * 28 + 20);
    for (int fault = 0; fault < 3; fault++) {
        memcpy(disk, image, DISK_SIZE); bc_init();
        w64(disk + leaf + dd, fault == 0 ? root : fault == 1 ? leaf + 1 : root + BTRFS_NODE_SIZE);
        seal(disk + leaf); guard_refuses();
    }
    for (int order = 0; order < 2; order++) {
        memcpy(disk, image, DISK_SIZE); uint64_t other = root + BTRFS_NODE_SIZE;
        memcpy(disk + other, image + leaf, BTRFS_NODE_SIZE);
        w64(disk + other + 24, other); seal(disk + other);
        w64(disk + leaf + dd, other); seal(disk + leaf);
        p = disk + root; w32(p + 32, 2); memcpy(p + 68, p + 36, 32);
        w64(p + 68, UINT64_MAX);
        w64(p + 36 + 24, order ? other : leaf);
        w64(p + 68 + 24, order ? leaf : other); seal(p);
        w64(disk + BTRFS_SUPER_OFFSET + 72, other + BTRFS_NODE_SIZE);
        seal(disk + BTRFS_SUPER_OFFSET); bc_init(); guard_refuses();
    }
    /* Mandatory prefix cap is enforced before allocating or reading beyond
     * the real small fixture, using a sparse-capacity device declaration. */
    memcpy(disk, image, DISK_SIZE);
    uint64_t huge = (AURAFS_MOUNT_MAX_BLOCKS + 1ull) * BTRFS_NODE_SIZE;
    w64(disk + BTRFS_SUPER_OFFSET + 32, huge);
    w64(disk + BTRFS_SUPER_OFFSET + 72, huge); seal(disk + BTRFS_SUPER_OFFSET);
    guard_capacity_override = huge / 512; bc_init(); guard_refuses(); guard_capacity_override = 0;
    /* File payload is outside the metadata-only scope: mount accepts it,
     * but its normal SHA-checked data read must fail. */
    memcpy(disk, image, DISK_SIZE);
    uint64_t data_lba = r64(disk + leaf + dd); disk[data_lba + 80] ^= 1;
    bc_init(); before = writes; assert(btrfs_init(0) == 0 && writes == before);
    char out[8]; v = btrfs_lookup(NULL, "dual-file");
    assert(v && btrfs_read(v, 0, out, sizeof(out)) < 0);
    free(image);
    puts("AuraFS M6s: PASS (mandatory descendant SHA/layout/read checks, cycles/duplicate metadata, depth/prefix/work budgets, extent bounds and metadata aliases, no file-data scan)");
    puts("AuraFS M6r: PASS (selected root backend/SHA/self/type/layout/child bounds, cache bypass, OOM/read refusal, no older-root rollback, unselected-root scope explicit, descendants fail closed)");
}

static void publication_refuses(const uint8_t *a, const uint8_t *b) {
    uint64_t active = bm.active_super, generation = bm.generation;
    int before = checkpoint_seen;
    assert(btrfs_sync(NULL) < 0 && bm.active_super == active && bm.generation == generation);
    assert(checkpoint_seen == before);
    assert(!memcmp(disk + BTRFS_SUPER_OFFSET, a, BTRFS_NODE_SIZE));
    assert(!memcmp(disk + AURAFS_MIRROR_OFFSET, b, BTRFS_NODE_SIZE));
    /* Failure did not even queue a dirty target checkpoint. */
    assert(bc_flush_range(0, AURAFS_MIRROR_OFFSET / BC_BLOCK_SIZE, 2) == 0);
    assert(checkpoint_seen == before);
}
static void test_publication_envelope(void) {
    uint8_t a[BTRFS_NODE_SIZE], b[BTRFS_NODE_SIZE], good[BTRFS_NODE_SIZE];
    format_fault_reset(); assert(btrfs_init(0) == 0);
    struct vnode *v = btrfs_create(NULL, "dual-file");
    assert(v && btrfs_write(v, 0, "OLD", 3) == 3 && btrfs_sync(NULL) == 0);
    memcpy(a, disk + BTRFS_SUPER_OFFSET, sizeof(a));
    memcpy(b, disk + AURAFS_MIRROR_OFFSET, sizeof(b));
    assert(btrfs_write(v, 0, "NEW", 3) == 3);
    uint64_t root = bm.tree_root_lba, end = bm.next_free_block;
    assert(bc_flush_range(0, BTRFS_FIRST_FREE / BC_BLOCK_SIZE,
                          (end - BTRFS_FIRST_FREE) / BC_BLOCK_SIZE) == 0);
    memcpy(good, disk + root, sizeof(good));
    /* Warm metadata caches must not hide a damaged candidate root at sync. */
    disk[root + 80] ^= 1; publication_refuses(a, b);
    memcpy(disk + root, good, sizeof(good));
    guard_fail_root = root; publication_refuses(a, b); guard_fail_root = 0;
    audit_oom = 1; publication_refuses(a, b); audit_oom = 0;
    w32(disk + root + 12, BT_DATA); seal(disk + root); publication_refuses(a, b);
    memcpy(disk + root, good, sizeof(good));
    int flushes = flush_calls;
    bm.next_free_block = end + 1; publication_refuses(a, b);
    assert(flush_calls == flushes); bm.next_free_block = end;
    bm.next_free_block = DISK_SIZE + BTRFS_NODE_SIZE; publication_refuses(a, b);
    assert(flush_calls == flushes); bm.next_free_block = end;
    /* A candidate beyond the mount depth budget cannot be published. */
    assert(r32(good + 12) == BT_ROOT && r32(good + 32) == 1);
    for (int i = 0; i <= BTRFS_MAX_LEVELS; i++) {
        uint64_t at = end + i * BTRFS_NODE_SIZE;
        memcpy(disk + at, good, BTRFS_NODE_SIZE); w64(disk + at + 24, at);
        w64(disk + at + 36 + 24, i == BTRFS_MAX_LEVELS ? root : at + BTRFS_NODE_SIZE);
        seal(disk + at);
    }
    bm.tree_root_lba = end;
    bm.next_free_block = end + (BTRFS_MAX_LEVELS + 1) * BTRFS_NODE_SIZE;
    publication_refuses(a, b);
    bm.tree_root_lba = root; bm.next_free_block = end;
    /* Live state isn't rolled back; restore the candidate fault and retry. */
    assert(btrfs_sync(NULL) == 0);
    bc_init(); allow_format = allow_dual = 0;
    assert(btrfs_init(0) == 0); dual_payload("NEW");

    /* Sparse-capacity fixture: physical array is small; ONLY low metadata/
     * checkpoint requests are made. High-tail checks exercise allocator
     * accounting, not a benchmark or actual 1 GiB data write. */
    uint64_t cap = AURAFS_MOUNT_MAX_BLOCKS * (1ull * BTRFS_NODE_SIZE);
    uint64_t huge = cap + 2 * BTRFS_NODE_SIZE;
    uint64_t slot = bm.active_super;
    w64(disk + slot + 32, huge); seal(disk + slot);
    /* Keep the other slot unrecognisable to avoid mixed-total ambiguity. */
    uint64_t other = slot == BTRFS_SUPER_OFFSET ? AURAFS_MIRROR_OFFSET : BTRFS_SUPER_OFFSET;
    memset(disk + other, 0, BTRFS_NODE_SIZE);
    guard_capacity_override = huge / 512; bc_init(); assert(btrfs_init(0) == 0);
    assert(bm.total_bytes == huge && bm.max_block == cap);
    assert(aurafs_append_ceiling(huge, 0) == huge);
    assert(aurafs_append_ceiling(DISK_SIZE, 1) == DISK_SIZE);
    bm.next_free_block = cap - BTRFS_NODE_SIZE;
    assert(blk_alloc() == cap - BTRFS_NODE_SIZE && bm.next_free_block == cap);
    assert(blk_alloc() == 0 && bm.next_free_block == cap);
    /* Defence in depth if max_block were accidentally widened in RAM. */
    bm.max_block = huge; assert(blk_alloc() == 0 && bm.next_free_block == cap);
    bm.dual = 0; assert(blk_alloc() == cap); bm.dual = 1;
    bm.next_free_block = cap + BTRFS_NODE_SIZE;
    int before = checkpoint_seen; flushes = flush_calls;
    assert(btrfs_sync(NULL) < 0 && checkpoint_seen == before && flush_calls == flushes);
    bm.next_free_block = cap; bm.max_block = cap;
    assert(btrfs_sync(NULL) == 0); bc_init();
    assert(btrfs_init(0) == 0 && bm.next_free_block == cap && bm.max_block == cap);
    dual_payload("NEW"); assert(blk_alloc() == 0);
    guard_capacity_override = 0;
    puts("AuraFS M6t: PASS (v2 allocator/remount ceiling, last block/ENOSPC, no checkpoint queue on candidate SHA/type/read/OOM/geometry rejection, corrected retry, exact-cap sparse remount, legacy ceiling unchanged)");
}

int main(void) {
    assert(fsbench_elapsed_us(10, 990000000, 11, 10000000) == 20000);
    assert(fsbench_elapsed_us(10, 10000000, 10, 30000000) == 20000);
    assert(fsbench_elapsed_us(10, 0, 10, 0) == 0);
    assert(fsbench_elapsed_us(11, 0, 10, 0) == 0);
    const struct blkdev_ops ops = { .read = rd, .write = wr, .sector_count = capacity, .flush = device_flush };
    assert(blkdev_register("aurafs-test", &ops, NULL, 512) == 0);
    bc_init();
    assert(btrfs_init(0) == 0);
    assert(btrfs_sync(NULL) == 0);
    assert(aurafs_validate_super(disk + BTRFS_SUPER_OFFSET, DISK_SIZE) == 0);

    int before_opt_in = writes;
    allow_dual = 1;
    assert(btrfs_init(0) == 0 && !bm.dual && writes == before_opt_in);
    allow_dual = 0; /* existing legacy volume never migrates */
    test_verification_cache();
    test_snapshots();
    test_batched_writes();
    test_snapshot_registry();
    test_reachability();
    test_open_vnode_identity();
    test_checkpoint_barriers();
    test_checkpoint_readback();
    test_checkpoint_history();
    assert(btrfs_sync(NULL) == 0);

    /* Append cursor is monotonic; no scan, no wrapping on full/corrupt state. */
    uint64_t cursor = bm.next_free_block;
    assert(blk_alloc() == cursor);
    assert(blk_alloc() == cursor + BTRFS_NODE_SIZE);
    uint64_t good_cursor = bm.next_free_block;
    bm.next_free_block = bm.max_block;
    assert(blk_alloc() == 0 && bm.next_free_block == bm.max_block);
    bm.next_free_block = UINT64_MAX - 4095;
    assert(blk_alloc() == 0 && bm.next_free_block == UINT64_MAX - 4095);
    bm.next_free_block = good_cursor + 1;
    assert(blk_alloc() == 0 && bm.next_free_block == good_cursor + 1);
    bm.next_free_block = good_cursor;

    uint8_t before[BTRFS_NODE_SIZE];
    memcpy(before, disk + BTRFS_SUPER_OFFSET, sizeof(before));
    uint8_t payload[100]; memset(payload, 0x52, sizeof(payload));
    assert(tree_insert(bm.tree_root_lba, 200, 1, 0, payload, sizeof(payload),
                       &bm.tree_root_lba) == 0);
    uint64_t generation = bm.generation;
    fail_appends = 1;
    assert(btrfs_sync(NULL) != 0);
    assert(bm.generation == generation);
    assert(memcmp(before, disk + BTRFS_SUPER_OFFSET, sizeof(before)) == 0);
    fail_appends = 0; fail_checkpoint = 1;
    append_seen = checkpoint_seen = 0;
    assert(btrfs_sync(NULL) != 0);
    assert(append_seen > 0 && checkpoint_seen == 0);
    assert(bm.generation == generation);
    assert(memcmp(before, disk + BTRFS_SUPER_OFFSET, sizeof(before)) == 0);
    fail_checkpoint = 0;
    assert(btrfs_sync(NULL) == 0);
    assert(checkpoint_seen == 1 && bm.generation == generation + 1);
    assert(r64(disk + BTRFS_SUPER_OFFSET + 64) == bm.tree_root_lba);
    assert(r64(disk + BTRFS_SUPER_OFFSET + 72) == bm.next_free_block);

    /* Drop the cache, mount only durable bytes, and resume after the tail. */
    uint64_t persisted_cursor = bm.next_free_block;
    assert(aurafs_snapshot_create(&stale_snapshot) == 0);
    uint64_t pre_mount_id = stale_snapshot.registration_id;
    bc_init();
    allow_format = 0;
    assert(btrfs_init(0) == 0);
    assert(bm.next_free_block == persisted_cursor);
    assert(aurafs_checkpoint_count == 1 && !aurafs_checkpoint_overflow);
    assert(checkpoint_has(bm.tree_root_lba, bm.next_free_block));
    assert(aurafs_snapshot_roots(NULL, 0) == 0);
    struct aurafs_snapshot fresh;
    assert(aurafs_snapshot_create(&fresh) == 0);
    assert(fresh.registration_id > pre_mount_id);
    uint8_t stale_byte;
    assert(aurafs_snapshot_read(&stale_snapshot, "snapdir/file", 0, &stale_byte, 1) < 0);
    aurafs_snapshot_release(&stale_snapshot);
    assert(aurafs_snapshot_roots(NULL, 0) == 1);
    aurafs_snapshot_release(&fresh);
    uint8_t expired[10];
    assert(aurafs_snapshot_read(&stale_snapshot, "snapdir/file", 0, expired, sizeof(expired)) < 0);
    uint8_t recovered[100]; uint32_t recovered_size;
    assert(tree_lookup(bm.tree_root_lba, 200, 1, 0, recovered,
                       sizeof(recovered), &recovered_size) == 0);
    assert(recovered_size == sizeof(payload) &&
           memcmp(payload, recovered, sizeof(payload)) == 0);
    assert(blk_alloc() == persisted_cursor);
    allow_format = 1;

    /* The public write path must propagate ENOSPC, not write at byte zero. */
    struct vnode *full_file = btrfs_create(NULL, "full-test");
    assert(full_file);
    uint8_t zero_before[BTRFS_NODE_SIZE];
    assert(fs_read_block(0, 0, 8, zero_before) == 0);
    uint64_t resume = bm.next_free_block, old_root = bm.tree_root_lba;
    bm.next_free_block = bm.max_block;
    assert(btrfs_write(full_file, 0, payload, sizeof(payload)) < 0);
    assert(bm.tree_root_lba == old_root);
    uint8_t zero_after[BTRFS_NODE_SIZE];
    assert(fs_read_block(0, 0, 8, zero_after) == 0);
    assert(memcmp(zero_before, zero_after, sizeof(zero_before)) == 0);
    assert(btrfs_write(full_file, UINT64_MAX - 10, payload, 100) < 0);
    assert(btrfs_write(full_file, 0, NULL, 100) < 0);
    assert(bm.next_free_block == bm.max_block);
    bm.next_free_block = resume;
    assert(btrfs_write(full_file, 0, payload, sizeof(payload)) == sizeof(payload));
    assert(btrfs_sync(NULL) == 0);

    /* Reject even correctly checksummed nonsense geometry. */
    uint8_t sb[BTRFS_NODE_SIZE];
    memcpy(sb, disk + BTRFS_SUPER_OFFSET, sizeof(sb));
    sb[80] ^= 1;
    assert(aurafs_validate_super(sb, DISK_SIZE) != 0);
    memcpy(sb, disk + BTRFS_SUPER_OFFSET, sizeof(sb));
    w64(sb + 72, UINT64_MAX - 4095); seal(sb);
    assert(aurafs_validate_super(sb, DISK_SIZE) != 0);
    memcpy(sb, disk + BTRFS_SUPER_OFFSET, sizeof(sb));
    w64(sb + 40, 8192); seal(sb);
    assert(aurafs_validate_super(sb, DISK_SIZE) != 0);
    memcpy(sb, disk + BTRFS_SUPER_OFFSET, sizeof(sb));
    w64(sb + 32, DISK_SIZE * 2); seal(sb);
    assert(aurafs_validate_super(sb, DISK_SIZE) != 0);
    memcpy(sb, disk + BTRFS_SUPER_OFFSET, sizeof(sb));
    w64(sb + 64, r64(sb + 72)); seal(sb);
    assert(aurafs_validate_super(sb, DISK_SIZE) != 0);
    memcpy(sb, disk + BTRFS_SUPER_OFFSET, sizeof(sb));
    w64(sb + 136, (uint64_t)UINT32_MAX + 1); seal(sb);
    assert(aurafs_validate_super(sb, DISK_SIZE) != 0);

    /* A recognizable corrupt superblock must not be formatted, even if allowed. */
    assert(bc_flush_range(0, 0, DISK_SIZE / BC_BLOCK_SIZE) == 0);
    assert(fs_read_block(0, BTRFS_SUPER_OFFSET / 512, 8, sb) == 0);
    sb[80] ^= 1;
    assert(fs_write_block(0, BTRFS_SUPER_OFFSET / 512, 8, sb) == 0);
    assert(bc_flush_range(0, BTRFS_SUPER_OFFSET / BC_BLOCK_SIZE, 1) == 0);
    int saved_writes = writes;
    struct aurafs_snapshot before_failed_mount;
    assert(aurafs_snapshot_create(&before_failed_mount) == 0);
    assert(btrfs_init(0) != 0 && !bm.mounted);
    assert(aurafs_snapshot_roots(NULL, 0) < 0);
    assert(aurafs_checkpoint_count == 0 && !aurafs_checkpoint_overflow);
    for (unsigned i = 0; i < AURAFS_MAX_SNAPSHOTS; i++)
        assert(!aurafs_snapshots[i].token.registration_id);
    assert(aurafs_snapshot_read(&before_failed_mount, "snapdir/file", 0, sb, 10) < 0);
    assert(aurafs_snapshot_create(&before_failed_mount) < 0);
    assert(writes == saved_writes);
    puts("AuraFS M6a: PASS (append bounds, checksum/geometry, ordered checkpoint, injected I/O failures, no destructive reformat)");
    test_dual_checkpoints();
    test_dual_format_interruptions();
    test_selected_root_guard();
    test_publication_envelope();
    return 0;
}
