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
    uint8_t data[4000], out[4000];
    memset(data, 0x5a, sizeof(data));
    /* Reverse-order insertion forces separator updates and leaf splits. */
    for (int i = 400; i >= 1; i--) {
        data[0] = (uint8_t)i;
        assert(tree_insert(root, i, 1, 0, data, 100, &root) == 0);
    }
    for (int i = 1; i <= 400; i++) {
        uint32_t size;
        assert(tree_lookup(root, i, 1, 0, out, sizeof(out), &size) == 0);
        assert(size == 100 && out[0] == (uint8_t)i && out[99] == 0x5a);
    }
    uint64_t old = root;
    memset(data, 0x77, sizeof(data));
    /* Large replacement exceeds one block in staging and needs a byte split. */
    assert(tree_insert(root, 400, 1, 0, data, 3900, &root) == 0);
    uint32_t size;
    assert(tree_lookup(root, 400, 1, 0, out, sizeof(out), &size) == 0);
    assert(size == 3900 && memcmp(out, data, size) == 0);
    assert(tree_lookup(old, 400, 1, 0, out, sizeof(out), &size) == 0);
    assert(size == 100 && out[0] == (uint8_t)400); /* old CoW root intact */
    assert(tree_remove(root, 400, 1, 0, &root) == 0);
    assert(tree_lookup(root, 400, 1, 0, out, sizeof(out), &size) != 0);
    assert(tree_insert(root, 400, 1, 0, data, 100, &root) == 0);
    assert(tree_lookup(root, 400, 1, 0, out, sizeof(out), &size) == 0);
    assert(tree_insert(root, 999, 1, 0, data, sizeof(data) + 100, &root) != 0);
    puts("M5 host regression: PASS (layout, splits, lookup, CoW, delete/reinsert, oversize)");
    return 0;
}
EOF
${CC:-cc} -O1 -g -ffunction-sections -fdata-sections -I . \
    "$tmp/test.c" kernel/lib/sha256.c -Wl,--gc-sections -o "$tmp/test"
"$tmp/test"
