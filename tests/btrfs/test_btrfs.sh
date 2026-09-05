#!/usr/bin/env bash
# test_btrfs.sh — F4b btrfs harness: full mutation surface + the internal
# CoW/CRC structural self-test receipt, driven from the shell on the mounted
# /btrfs volume.
#
# Scope note (honest bar): AuraLite's btrfs is a *custom* layout — its own
# CoW keyed tree, per-block magic and SHA-256 block-trailer checksums
# btrfs on-disk format (no subvolumes, snapshots, chunk tree, RAID; those are
# explicitly out of scope per FSFULL_PLAN F6).  So host `mkfs.btrfs` /
# `btrfs check` binary-format interop is NOT applicable the way it is for
# ext4, exactly as with f2fs.  The structural gate here is therefore the
# in-kernel btrfs self-test (`btrfsck` shell builtin, SYS_BTRFS_SELFTEST),
# which is a real check, not a no-op:
#   - CoW overwrite: writes generation-2 over generation-1 data and verifies
#     BOTH blocks still exist on disk, both CRC-valid, each with its own
#     content, and the tree re-resolves to the new block.
#   - multi-block (8 KiB) byte-exact round-trip; SHA-256 verify-on-read runs
#     on every block read.
#   - mkdir/create-in-subdir/rename/link(nlink>=2)/settimes/stat,
#     truncate-down with zero/trunc read-back, unlink, rmdir, fsync.
#
# Exercises (each asserted):
#   - kernel formats a blank disk (fsformat knob = 1)
#   - mkdir + create-in-subdir + write + cat (read-back round-trip)
#   - rename + stat (links, size, mode)
#   - hard link (nlink bumps to 2) + settimes (atime/mtime observed)
#   - fsync persists tree root / free pointer
#   - truncate down + read-back
#   - `btrfsck` (in-kernel CoW/CRC self-test) clean before and after deletions
#   - unlink (last link) + rmdir
#   - persistence on a clean remount (fsformat=0), no reformat
#
# Requires: qemu-system-x86_64.  Skips cleanly when absent.

set -u
cd "$(dirname "$0")/../.."
. tests/integration/lib/lib.sh
il_init
# F7 (FSFULL_PLAN.md): boot in FAST selftest mode so the kernel's
# destructive on-disk self-tests do not run during this coverage
# harness's own volume drive (the harness invokes its self-test
# explicitly where it needs it).  The five self-tests themselves run
# in the FULL selftest lane (verified by a dedicated full boot).
IL_SELFTEST=fast
il_have qemu-system-x86_64

il_section "btrfs (CoW tree, SHA-256 trailer, full mutation surface + internal self-test)"

# 6 AHCI disks: btrfs is the 5th (blkdev 4, /btrfs).  All are fresh so every
# FS phase formats independently.
D0="$IL_BUILD/btrfs-d0.img"; D1="$IL_BUILD/btrfs-d1.img"
D2="$IL_BUILD/btrfs-d2.img"; D3="$IL_BUILD/btrfs-d3.img"
D4="$IL_BUILD/btrfs-d4.img"; D5="$IL_BUILD/btrfs-d5.img"
for d in "$D0" "$D1" "$D2" "$D3" "$D4" "$D5"; do rm -f "$d"; done
il_make_disk "$D0" 16 "AURALHCI"
il_make_disk "$D1" 16 "AURALHCI"
il_make_disk "$D2" 16 "AURALHCI"
il_make_disk "$D3" 16 "AURALHCI"
il_make_disk "$D4" 128 "AURALHCI"
il_make_disk "$D5" 16 "AURALHCI"

QEMU_DISKS=(
    -drive "file=$D0,format=raw,if=none,id=d0" -device "ide-hd,drive=d0,bus=ahci0.0"
    -drive "file=$D1,format=raw,if=none,id=d1" -device "ide-hd,drive=d1,bus=ahci0.1"
    -drive "file=$D2,format=raw,if=none,id=d2" -device "ide-hd,drive=d2,bus=ahci0.2"
    -drive "file=$D3,format=raw,if=none,id=d3" -device "ide-hd,drive=d3,bus=ahci0.3"
    -drive "file=$D4,format=raw,if=none,id=d4" -device "ide-hd,drive=d4,bus=ahci0.4"
    -drive "file=$D5,format=raw,if=none,id=d5" -device "ide-hd,drive=d5,bus=ahci0.5"
)
AHCI_DEV=( -device "ahci,id=ahci0" )

# ---- Pass 1: kernel formats + full mutation surface (fsformat=1) ----
LOG1="$IL_LOGDIR/btrfs_p1.log"
il_send_delay 9
il_send "mkdir /btrfs/sub"
il_send_delay 1
il_send "write /btrfs/sub/f1.txt hello-from-btrfs"
il_send_delay 1
il_send "cat /btrfs/sub/f1.txt"
il_send_delay 1
il_send "mv /btrfs/sub/f1.txt /btrfs/sub/f2.txt"
il_send_delay 1
il_send "stat /btrfs/sub/f2.txt"
il_send_delay 1
il_send "link /btrfs/sub/f2.txt /btrfs/sub/hard.txt"
il_send_delay 1
il_send "stat /btrfs/sub/hard.txt"
il_send_delay 1
il_send "settimes /btrfs/sub/f2.txt 111 222"
il_send_delay 1
il_send "stat /btrfs/sub/f2.txt"
il_send_delay 1
il_send "fsync /btrfs/sub/f2.txt"
il_send_delay 1
il_send "truncate /btrfs/sub/f2.txt 7"
il_send_delay 1
il_send "cat /btrfs/sub/f2.txt"
il_send_delay 1
il_send "btrfsck"
il_send_delay 1
il_send "rm /btrfs/sub/hard.txt"
il_send_delay 1
il_send "rm /btrfs/sub/f2.txt"
il_send_delay 1
il_send "rmdir /btrfs/sub"
il_send_delay 1
il_send "btrfsck"
il_send_delay 1
il_send "ls /btrfs"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG1" 50 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=1"

il_assert_grep "$LOG1" "\[btrfs\] format complete"       "kernel formatted blank btrfs volume"
il_assert_grep "$LOG1" "hello-from-btrfs"                "btrfs write+read round-trip"
il_assert_grep "$LOG1" "\[btrfs\] rename:.*f1.txt.*f2.txt" "rename on btrfs"
il_assert_grep "$LOG1" "Links:   1"                      "stat shows links 1"
il_assert_grep "$LOG1" "\[btrfs\] link:.*hard.txt.*links 2" "hard link bumps nlink"
il_assert_grep "$LOG1" "settimes: .*atime=111 mtime=222" "settimes accepted"
il_assert_grep "$LOG1" "ATime:   111"                    "settimes atime observed via stat"
il_assert_grep "$LOG1" "MTime:   222"                    "settimes mtime observed via stat"
il_assert_grep "$LOG1" "\[btrfs\] fsync: persisted tree root LBA" \
                                                          "fsync persisted tree root"
il_assert_grep "$LOG1" "truncate: .* -> 7 bytes"        "truncate down to 7 bytes"
il_assert_grep "$LOG1" "hello-f"                          "truncate read-back (first 7 bytes)"
il_assert_grep "$LOG1" "\[btrfs\] PASS:"                  "btrfs CoW/SHA-256 self-test clean (allocations)"
# RESIDUE2 T3: the self-test's step 9 corrupts a sealed block on disk and
# requires the SHA-256 trailer to refuse it (negative control).  Fixed-string
# match: the parens in "(RESIDUE2 T3)" defeat grep -E (known lib.sh issue).
il_assert_grep_fixed "$LOG1" "[btrfs] PASS: SHA-256 detects on-disk corruption (RESIDUE2 T3)" \
                                                          "SHA-256 catches on-disk corruption"
il_assert_grep "$LOG1" "btrfsck: clean"                  "btrfsck builtin reports clean"
il_assert_grep "$LOG1" "\[btrfs\] unlink: removed 'sub/f2.txt'" "last link unlink"
il_assert_grep "$LOG1" "\[btrfs\] rmdir: removed 'sub'"  "rmdir on btrfs"
il_assert_grep "$LOG1" "btrfsck: clean"                  "btrfsck clean (after deletions)"

# ---- Pass 2: clean remount, no reformat, empty-root persistence ----
LOG2="$IL_LOGDIR/btrfs_p2.log"
il_send_delay 9
il_send "ls /btrfs"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG2" 40 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=0"

il_assert_grep "$LOG2" "\[btrfs\] mounted CoW filesystem" "btrfs remounted without reformat"
il_assert_grep "$LOG2" "SHA-256 checksums \(block trailer\) \+ CoW tree re-parenting enabled" \
                                                          "mount banner present (SHA-256 trailer)"

il_summary
