#!/usr/bin/env bash
# rv_fs_smoke.sh — PARITY_PLAN.md P2: ext2 mounted on a64, proven.
#
# One boot of the a64 kernel with a HOST-FORMATTED ext2 image (the
# exact mkfs.ext2 recipe test_ext2.sh uses on x86) on virtio-mmio blk:
#   - vblk sniffs sector 0, sees no parity pattern, hands the disk to
#     the blkdev seam (the pattern disk's selftest gate is untouched —
#     rv_parity_smoke.sh keeps proving that lane);
#   - the SHARED ext2.c (same object list as x86: KERNELRV_SHARED)
#     mounts it and self-tests read/write/dir/indirect;
#   - a known file seeded with debugfs is read back through the ops
#     table and its per-run token must appear byte-exact.
#
# QEMU output goes to a file only (the standing rule); 200KB fuse.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/kernela64.img"
TAR="$BUILD/initrd.tar"
DISK="$BUILD/a64_fs_ext2.img"
LOG="$BUILD/a64_fs.log"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[a64-fs] SKIP: qemu-system-aarch64 not installed" >&2
    exit 0
fi

MKFS=""; DEBUGFS=""
for p in /sbin/mkfs.ext2 /usr/sbin/mkfs.ext2; do [ -x "$p" ] && MKFS="$p"; done
for p in /sbin/debugfs  /usr/sbin/debugfs;  do [ -x "$p" ] && DEBUGFS="$p"; done
if [ -z "$MKFS" ] || [ -z "$DEBUGFS" ]; then
    echo "[a64-fs] SKIP: e2fsprogs not installed (need mkfs.ext2 + debugfs)" >&2
    exit 0
fi

[ -s "$IMG" ] || make -C "$ROOT" kernela64-img >/dev/null
[ -s "$TAR" ] || make -C "$ROOT" "$TAR" >/dev/null

# ---- the image: mkfs on the host, one seeded file with a run token ----
TOKEN="A64FS_$$_$(date +%s)"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=4 status=none
"$MKFS" -q -b 1024 -I 128 -F "$DISK" >/dev/null 2>&1
SEED="$BUILD/a64_fs_seed.$$.txt"
echo "hello from linux mkfs $TOKEN" > "$SEED"
SEED_LEN=$(wc -c < "$SEED")
"$DEBUGFS" -w -R "write $SEED LINUX.TXT" "$DISK" >/dev/null 2>&1
rm -f "$SEED"

# ---- one boot ----
rm -f "$LOG"
{
    for _ in $(seq 1 150); do
        grep -qa "auralite# " "$LOG" 2>/dev/null && break
        sleep 1
    done
    printf 'ls /\n';           sleep 2
    printf 'stat LINUX.TXT\n'; sleep 2
    printf 'cat LINUX.TXT\n';  sleep 2
    printf 'run bina64/fsio\n';       sleep 3
    printf 'exit\n'; sleep 3
} | timeout 240 qemu-system-aarch64 \
        -machine virt -cpu cortex-a72 -m 256M \
        -display none -serial stdio -no-reboot \
        -kernel "$IMG" -initrd "$TAR" \
        -global virtio-mmio.force-legacy=true \
        -drive file="$DISK",format=raw,if=none,id=hd \
        -device virtio-blk-device,drive=hd \
        > "$LOG" 2>/dev/null || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

if [ "$(wc -c < "$LOG")" -gt 200000 ]; then
    echo "[a64-fs] FAIL: log exceeds the 200KB fuse (runaway output)" >&2
    exit 1
fi

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [a64-fs] OK   $desc"
    else
        echo "  [a64-fs] FAIL $desc (pattern: $pat)" >&2
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [a64-fs] FAIL $desc (found: $pat)" >&2
        fail=1
    else
        echo "  [a64-fs] OK   $desc"
    fi
}

# ---- the receipts, one per link in the chain ----
assert_grep "virtio-blk over mmio"                        "vblk transport up"
assert_grep "sector 0: no test pattern; filesystem media" "media sniff took the fs lane"
assert_grep "\[blkdev\] blk0 = vblk0"                     "vblk registered with the seam"
assert_grep "\[ext2\] mounted existing volume"            "SHARED ext2.c recognised the host-formatted volume"
assert_grep "\[vfs\] mounted '/'"                        "R2: the SHARED mount table took the root mount"
assert_grep "\[ext2\] blkdev 0 mounted"                   "ext2 names the seam device, not a driver"
assert_grep "\[a64fs\] mounted ext2 on blkdev 0"           "arch glue receipt"
assert_grep "\[ext2\] PASS:"                              "ext2 self-test (write/dir/indirect/rename) on a64"
assert_grep "LINUX.TXT"                                   "seeded file visible"
assert_grep "$TOKEN"                                      "cat returned this run's token byte-exact"
assert_no_grep "\[a64fs\] cat: "                           "no cat failure path taken"
assert_no_grep "\[blk\]  FAIL"                            "no vblk failure"
# ---- P4: the same file, this time from USERSPACE via the file five ----
assert_grep "  LINUX.TXT"                                 "P4 ls: readdir lists the seeded file"
assert_grep "lost+found/"                                 "P4 ls: directories carry the slash"
assert_grep "file, ${SEED_LEN} bytes"                     "P4 stat: size through the trap"
assert_grep "(${SEED_LEN} bytes)"                         "P4 cat: lseek(END) size receipt"
assert_no_grep "ls: cannot open"                          "P4: open('/') succeeded"
assert_no_grep "cat: cannot open"                         "P4: open(file) succeeded"
# R6: malloc + stdio-lite round-trip through the mounted fs.
assert_grep "fsio: PASS malloc+stdio round-trip (48 bytes)" "R6: brk/malloc/fopen-create/fwrite/fread, one source"
assert_no_grep "UNHANDLED EXCEPTION"                      "no unhandled trap anywhere in the boot"

if [ "$fail" -ne 0 ]; then
    echo "[a64-fs] FAILED — log tail:" >&2
    tail -30 "$LOG" >&2
    exit 1
fi
echo "[a64-fs] all assertions passed"
