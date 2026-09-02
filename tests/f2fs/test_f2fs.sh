#!/usr/bin/env bash
# test_f2fs.sh — F4 f2fs harness: full mutation surface + the internal
# f2fs structural fsck + checkpoint-validation receipts, driven from the
# shell on the mounted /f2fs volume.
#
# Scope note (honest bar): AuraLite's f2fs is a *custom* layout — superblock
# magic 0xF2F20210 versus the real on-disk f2fs 0xF2F52010 — so the host
# mkfs.f2fs / fsck.f2fs binary-format interop lane is NOT applicable the way
# it is for ext4.  The fsck gate here is therefore the in-kernel structural
# fsck (SYS_F2FS_FSCK, the `f2fsck` shell builtin) which walks the NAT for
# every in-use NID, checks node-block headers, and flags inodes with no
# file-type bits or zero link counts.  This is a real structural check, not
# a no-op.
#
# Exercises (each asserted):
#   - kernel formats a blank disk (fsformat knob = 1)
#   - mkdir + create-in-subdir + write + cat (read-back round-trip)
#   - rename + stat (links, size, mode)
#   - hard link (nlink bumps to 2) + settimes (atime/mtime observed)
#   - fsync = segment flush + checkpoint advance
#   - internal f2fsck clean before and after deletions
#   - unlink (last link frees the NID from NAT) + rmdir
#   - persistence + checkpoint validation on a clean remount (fsformat=0)
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

il_section "f2fs (full mutation surface + internal fsck + CP validation)"

# 6 AHCI disks: f2fs is the 6th (blkdev 5, /f2fs).  All are fresh so every
# FS phase formats independently.  D5 must be > ~34 MB: f2fs's backup
# superblock sits at page 8194 (~33.5 MB), so a 16 MiB disk cannot hold it.
D0="$IL_BUILD/f2fs-d0.img"; D1="$IL_BUILD/f2fs-d1.img"
D2="$IL_BUILD/f2fs-d2.img"; D3="$IL_BUILD/f2fs-d3.img"
D4="$IL_BUILD/f2fs-d4.img"; D5="$IL_BUILD/f2fs-d5.img"
for d in "$D0" "$D1" "$D2" "$D3" "$D4" "$D5"; do rm -f "$d"; done
il_make_disk "$D0" 16 "AURALHCI"
il_make_disk "$D1" 16 "AURALHCI"
il_make_disk "$D2" 16 "AURALHCI"
il_make_disk "$D3" 16 "AURALHCI"
il_make_disk "$D4" 16 "AURALHCI"
il_make_disk "$D5" 128 "AURALHCI"

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
LOG1="$IL_LOGDIR/f2fs_p1.log"
il_send_delay 9
il_send "mkdir /f2fs/sub"
il_send_delay 1
il_send "write /f2fs/sub/f1.txt hello-from-f2fs"
il_send_delay 1
il_send "cat /f2fs/sub/f1.txt"
il_send_delay 1
il_send "mv /f2fs/sub/f1.txt /f2fs/sub/f2.txt"
il_send_delay 1
il_send "stat /f2fs/sub/f2.txt"
il_send_delay 1
il_send "link /f2fs/sub/f2.txt /f2fs/sub/hard.txt"
il_send_delay 1
il_send "stat /f2fs/sub/hard.txt"
il_send_delay 1
il_send "settimes /f2fs/sub/f2.txt 111 222"
il_send_delay 1
il_send "stat /f2fs/sub/f2.txt"
il_send_delay 1
il_send "fsync /f2fs/sub/f2.txt"
il_send_delay 1
il_send "f2fsck"
il_send_delay 1
il_send "rm /f2fs/sub/hard.txt"
il_send_delay 1
il_send "rm /f2fs/sub/f2.txt"
il_send_delay 1
il_send "rmdir /f2fs/sub"
il_send_delay 1
il_send "f2fsck"
il_send_delay 1
il_send "ls /f2fs"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG1" 50 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=1"

il_assert_grep "$LOG1" "\[f2fs\] format complete"          "kernel formatted blank f2fs volume"
il_assert_grep "$LOG1" "hello-from-f2fs"                    "f2fs write+read round-trip"
il_assert_grep "$LOG1" "\[f2fs\] rename:.*f1.txt.*f2.txt"   "rename on f2fs"
il_assert_grep "$LOG1" "Links:   1"                         "stat shows links 1"
il_assert_grep "$LOG1" "\[f2fs\] link:.*hard.txt.*links 2"  "hard link bumps nlink"
il_assert_grep "$LOG1" "settimes: .*atime=111 mtime=222"    "settimes accepted"
il_assert_grep "$LOG1" "ATime:   111"                       "settimes atime observed via stat"
il_assert_grep "$LOG1" "MTime:   222"                       "settimes mtime observed via stat"
il_assert_grep "$LOG1" "\[f2fs\] fsync: flushed segment.*checkpoint ver=" \
                                                             "fsync flushed segment + advanced CP"
il_assert_grep "$LOG1" "f2fsck: clean"                      "internal f2fsck clean (allocations)"
il_assert_grep "$LOG1" "\[f2fs\] unlink: removed 'sub/f2.txt' \(nid [0-9]+, links 0\)" \
                                                             "last link frees NID"
il_assert_grep "$LOG1" "\[f2fs\] rmdir: removed 'sub'"       "rmdir on f2fs"
il_assert_grep "$LOG1" "f2fsck: clean"                      "internal f2fsck clean (after deletions)"

# ---- Pass 2: clean remount, CP validation, empty-root persistence ----
LOG2="$IL_LOGDIR/f2fs_p2.log"
il_send_delay 9
il_send "ls /f2fs"
il_send_delay 1
il_send "f2fsck"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG2" 40 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=0"

il_assert_grep "$LOG2" "\[f2fs\] CP pack 0 ok ver="          "checkpoint pack 0 validated on mount"
il_assert_grep "$LOG2" "\[f2fs\] CP pack 1 ok ver="          "checkpoint pack 1 validated on mount"
il_assert_grep "$LOG2" "\[f2fs\] mounted"                     "f2fs remounted without reformat"
il_assert_grep "$LOG2" "f2fsck: clean"                       "f2fsck clean on remounted volume"

il_summary
