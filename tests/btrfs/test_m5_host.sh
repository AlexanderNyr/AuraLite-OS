#!/usr/bin/env bash
# Standalone regression: exercise the actual tree code over a RAM block device.
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test.c" <<'EOF'
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "kernel/fs/btrfs.c"
static unsigned char disk[128 * 1024 * 1024];
void kprintf(const char *fmt, ...) { (void)fmt; }
int fs_read_block(int dev, uint64_t lba, uint32_t count, void *buf) {
    (void)dev;
    if (lba * 512 + count * 512 > sizeof(disk)) return -1;
    memcpy(buf, disk + lba * 512, count * 512); return 0;
}
int fs_write_block(int dev, uint64_t lba, uint32_t count, const void *buf) {
    (void)dev;
    if (lba * 512 + count * 512 > sizeof(disk)) return -1;
    memcpy(disk + lba * 512, buf, count * 512); return 0;
}
int main(void) {
    uint8_t scratch[BTRFS_NODE_SIZE], staging[BTRFS_NODE_SIZE];
    bscratch = scratch; bdatabuf = staging;
    bm.nodesize = BTRFS_NODE_SIZE;
    bm.max_block = sizeof(disk);
    bm.next_free_block = BTRFS_FIRST_FREE;
    uint64_t root = blk_alloc();
    memset(bscratch, 0, BTRFS_NODE_SIZE);
    assert(blk_write(root, bscratch, BT_LEAF, 0) == 0);
    uint8_t data[25000], out[25000];
    memset(data, 0x5a, sizeof(data));
    /* Reverse-order insertion forces separator updates and leaf splits. */
    for (int i = 2500; i >= 1; i--) {
        data[0] = (uint8_t)i;
        assert(tree_insert(root, i, 1, 0, data, 100, &root) == 0);
    }
    for (int i = 1; i <= 2500; i++) {
        uint32_t size;
        assert(tree_lookup(root, i, 1, 0, out, sizeof(out), &size) == 0);
        assert(size == 100 && out[0] == (uint8_t)i && out[99] == 0x5a);
    }
    int depth; uint32_t leaves, items;
    tree_stats(root, &depth, &leaves, &items);
    assert(depth >= 3 && items == 2500);
    uint64_t old = root;
    memset(data, 0x77, sizeof(data));
    /* Large replacement exceeds one block in staging and needs a byte split. */
    assert(tree_insert(root, 2500, 1, 0, data, 3900, &root) == 0);
    uint32_t size;
    assert(tree_lookup(root, 2500, 1, 0, out, sizeof(out), &size) == 0);
    assert(size == 3900 && memcmp(out, data, size) == 0);
    assert(tree_lookup(old, 2500, 1, 0, out, sizeof(out), &size) == 0);
    assert(size == 100 && out[0] == (uint8_t)2500); /* old CoW root intact */
    assert(tree_remove(root, 2500, 1, 0, &root) == 0);
    assert(tree_lookup(root, 2500, 1, 0, out, sizeof(out), &size) != 0);
    assert(tree_insert(root, 2500, 1, 0, data, 100, &root) == 0);
    assert(tree_lookup(root, 2500, 1, 0, out, sizeof(out), &size) == 0);
    assert(tree_insert(root, 999, 1, 0, data, sizeof(data) + 100, &root) != 0);
    /* Find an actual 16-bit collision, then remove the chain head. */
    static int seen[65536];
    char first[32], second[32], name[32];
    for (int i = 0; i < 65536; i++) seen[i] = -1;
    int collision = 0;
    for (int i = 0; i < 65537; i++) {
        snprintf(name, sizeof(name), "collision_%d", i);
        unsigned h = (unsigned)name_hash((uint8_t *)name, (int)strlen(name));
        if (seen[h] >= 0) {
            snprintf(first, sizeof(first), "collision_%d", seen[h]);
            snprintf(second, sizeof(second), "%s", name);
            collision = 1; break;
        }
        seen[h] = i;
    }
    assert(collision);
    const char *names[2] = { first, second };
    for (int i = 0; i < 2; i++) {
        uint32_t len = (uint32_t)strlen(names[i]);
        memset(data, 0, 16 + len);
        w64(data, 1000 + i); w32(data + 12, len);
        memcpy(data + 16, names[i], len);
        assert(dir_insert(root, 5000, data, 16 + len, &root) == 0);
    }
    uint64_t key; int found;
    assert(dir_slot(root, 5000, second, out, &size, &key, &found) == 0 && found);
    assert(r64(out) == 1001);
    assert(dir_remove(root, 5000, first, &root) == 0);
    assert(dir_slot(root, 5000, second, out, &size, &key, &found) == 0 && found);
    assert(r64(out) == 1001);
    assert(dir_slot(root, 5000, first, out, &size, &key, &found) == 0 && !found);
    puts("M5 host regression: PASS (layout, splits, lookup, CoW, delete/reinsert, oversize, root growth, hash collisions)");
    return 0;
}
EOF
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I . \
    "$tmp/test.c" kernel/lib/sha256.c -Wl,--gc-sections -o "$tmp/test"
"$tmp/test"
