#!/usr/bin/env bash
# i386_fs_smoke.sh — PARITY_PLAN.md P7: the shared ext2 mounts on
# i386, proven.  (Hand-written; the P3 sed lesson.)
#
# One BIOS boot of the real `make iso` image with a SECOND IDE drive
# (primary slave) carrying a host-formatted ext2 image — the same
# mkfs recipe every other port's fs smoke uses:
#   - ata32 probes the slave at init (the master stays the boot disk
#     whose MBR the selftest pins);
#   - both drives register with the blkdev seam (ata0, ata1);
#   - the SHARED ext2.c — the same object list x86_64/rv64/a64 link,
#     now compiled -m32 after P7's width pay-down — picks the SECOND
#     device via the shared ext2_init(-1) rule, mounts, self-tests,
#     and cats the debugfs-seeded file back byte-exact.
#
# QEMU output to a file only (the standing rule); 200KB fuse.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
DISK="$BUILD/i386_fs_ext2.img"
LOG="$BUILD/i386_fs.log"

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-fs] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

MKFS=""; DEBUGFS=""
for p in /sbin/mkfs.ext2 /usr/sbin/mkfs.ext2; do [ -x "$p" ] && MKFS="$p"; done
for p in /sbin/debugfs  /usr/sbin/debugfs;  do [ -x "$p" ] && DEBUGFS="$p"; done
if [ -z "$MKFS" ] || [ -z "$DEBUGFS" ]; then
    echo "[i386-fs] SKIP: e2fsprogs not installed (need mkfs.ext2 + debugfs)" >&2
    exit 0
fi

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

TOKEN="I386FS_$$_$(date +%s)"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=4 status=none
"$MKFS" -q -b 1024 -I 128 -F "$DISK" >/dev/null 2>&1
SEED="$BUILD/i386_fs_seed.$$.txt"
echo "hello from linux mkfs $TOKEN" > "$SEED"
SEED_LEN=$(wc -c < "$SEED")
"$DEBUGFS" -w -R "write $SEED LINUX.TXT" "$DISK" >/dev/null 2>&1
rm -f "$SEED"

rm -f "$LOG"
timeout 90 qemu-system-i386 \
        -drive format=raw,file="$ISO",if=ide,snapshot=on \
        -drive format=raw,file="$DISK",if=ide,index=1 \
        -m 256M -display none -serial file:"$LOG" -no-reboot \
        < /dev/null > /dev/null 2>&1 || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

if [ "$(wc -c < "$LOG")" -gt 200000 ]; then
    echo "[i386-fs] FAIL: log exceeds the 200KB fuse (runaway output)" >&2
    exit 1
fi

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [i386-fs] OK   $desc"
    else
        echo "  [i386-fs] FAIL $desc (pattern: $pat)" >&2
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        echo "  [i386-fs] FAIL $desc (found: $pat)" >&2
        fail=1
    else
        echo "  [i386-fs] OK   $desc"
    fi
}

assert_grep "\[ata\] primary slave: 8192 sectors"     "the slave probe found the ext2 disk"
assert_grep "\[ata\] PASS:"                           "the master's own selftest still green"
assert_grep "\[blkdev\] blk0 = ata0"                  "master registered with the seam"
assert_grep "\[blkdev\] blk1 = ata1"                  "slave registered with the seam"
assert_grep "\[ext2\] mounted existing volume"        "SHARED ext2.c (-m32 build) recognised the volume"
assert_grep "\[ext2\] blkdev 1 mounted"               "the shared second-disk picker chose blk1"
assert_grep "\[fs32\] mounted ext2 on blkdev 1"       "arch glue receipt"
assert_grep "\[ext2\] PASS:"                          "ext2 self-test (write/dir/indirect/rename) on i386"
assert_grep "$TOKEN"                                  "cat returned this run's token byte-exact"
assert_grep "cat LINUX.TXT (${SEED_LEN} bytes)"       "and with the right length"
assert_no_grep "\[fs32\] cat: "                       "no cat failure path taken"
assert_no_grep "\[ata\] FAIL"                         "no ATA failure"
assert_no_grep "Page Fault\|kernel panic"             "no fault anywhere in the boot"

if [ "$fail" -ne 0 ]; then
    echo "[i386-fs] FAILED — log tail:" >&2
    tail -30 "$LOG" >&2
    exit 1
fi
echo "[i386-fs] all assertions passed"
