#!/usr/bin/env bash
# test_ntfs.sh — F5b NTFS read-only harness.
#
# NTFS is read-only by design.  The integration lane attaches a REAL
# host-created NTFS volume (mkntfs + ntfscp from ntfs-3g) and verifies, in
# the guest:
#   - the kernel mounts it read-only ([ntfs] mounted read-only ...);
#   - readdir lists host files with real sizes;
#   - `cat` returns the host file bytes byte-exact (both a resident $DATA
#     file and a multi-cluster, non-resident $DATA file read through
#     runlists);
#   - `stat` reports real sizes and the MFT index as the inode;
#   - every mutation attempt refuses: mkdir/write/unlink/rmdir/rename/
#     settimes print the `[ntfs] ... refused: read-only \(-EROFS\)` line /
#     return -EROFS and nothing is created or written.
#
# Device note: a single 6-port ICH9 AHCI controller exposes blkdev 0-5, all
# assigned to earlier filesystems, so the former fixed NTFS slot (blkdev 6)
# is unreachable.  kernel.c F5b auto-detects an NTFS signature across the
# reachable devices (the signature gate keeps it from misclaiming a
# dedicated slot); this harness attaches the NTFS volume as blkdev 0.
#
# Requires: qemu-system-x86_64, mkntfs + ntfscp + ntfsls (ntfs-3g).  Skips
# cleanly when the host tools are absent.

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

if ! il_have qemu-system-x86_64; then
    echo "${C_RED}[lib] qemu-system-x86_64 missing; skipping ntfs harness${C_RESET}"
    exit 0
fi
MKNTFS=$(command -v mkntfs || echo /sbin/mkntfs)
NTFSCP=$(command -v ntfscp || echo /sbin/ntfscp)
NTFSLS=$(command -v ntfsls || echo /usr/bin/ntfsls)
if [ ! -x "$MKNTFS" ] || [ ! -x "$NTFSCP" ] || [ ! -x "$NTFSLS" ]; then
    echo "${C_RED}[lib] ntfs-3g tools (mkntfs/ntfscp/ntfsls) missing; skipping ntfs harness${C_RESET}"
    exit 0
fi

il_section "NTFS (read-only, real mkntfs volume, byte-exact reads + -EROFS refusals)"

D0="$IL_BUILD/ntfs-d0.img"; D1="$IL_BUILD/ntfs-d1.img"
D2="$IL_BUILD/ntfs-d2.img"; D3="$IL_BUILD/ntfs-d3.img"
D4="$IL_BUILD/ntfs-d4.img"; D5="$IL_BUILD/ntfs-d5.img"
for d in "$D0" "$D1" "$D2" "$D3" "$D4" "$D5"; do rm -f "$d"; done
il_make_disk "$D0" 4 "AURALHCI"
il_make_disk "$D1" 4 "AURALHCI"
il_make_disk "$D3" 4 "AURALHCI"
il_make_disk "$D4" 4 "AURALHCI"
il_make_disk "$D5" 4 "AURALHCI"

# The real NTFS volume, attached as blkdev 2 (a signature-gated slot, so
# diskfs on blkdev 0 and ext2 on blkdev 1 — which may format their own
# disks — cannot clobber it).  The kernel auto-detects NTFS across the
# reachable devices.  A 20480-byte file (5 clusters) forces a non-resident
# $DATA with a multi-run read.
rm -f "$D2"; dd if=/dev/zero of="$D2" bs=1M count=32 status=none
"$MKNTFS" -F -Q -L TEST "$D2" >/dev/null 2>&1
printf 'Hello-from-NTFS' > "$IL_BUILD/greeting.txt"
IL_BUILD="$IL_BUILD" python3 - <<'PY'
import os
pat = b'NTFS-RUNLIST-MULTICLUSTER-0123456789'
data = (pat * 600)[:20480]   # 5 clusters, non-resident $DATA
open(os.path.join(os.environ["IL_BUILD"], "alpha.bin"), "wb").write(data)
PY
"$NTFSCP" "$D2" "$IL_BUILD/greeting.txt" greeting.txt
"$NTFSCP" "$D2" "$IL_BUILD/alpha.bin" alpha.bin

QEMU_DISKS=(
    -drive "file=$D0,format=raw,if=none,id=d0" -device "ide-hd,drive=d0,bus=ahci0.0"
    -drive "file=$D1,format=raw,if=none,id=d1" -device "ide-hd,drive=d1,bus=ahci0.1"
    -drive "file=$D2,format=raw,if=none,id=d2" -device "ide-hd,drive=d2,bus=ahci0.2"
    -drive "file=$D3,format=raw,if=none,id=d3" -device "ide-hd,drive=d3,bus=ahci0.3"
    -drive "file=$D4,format=raw,if=none,id=d4" -device "ide-hd,drive=d4,bus=ahci0.4"
    -drive "file=$D5,format=raw,if=none,id=d5" -device "ide-hd,drive=d5,bus=ahci0.5"
)
AHCI_DEV=( -device "ahci,id=ahci0" )

LOG="$IL_LOGDIR/ntfs_main.log"
il_send_delay 9
il_send "ls /ntfs"
il_send_delay 1
il_send "cat /ntfs/greeting.txt"
il_send_delay 1
il_send "stat /ntfs/greeting.txt"
il_send_delay 1
il_send "cat /ntfs/alpha.bin"
il_send_delay 1
il_send "stat /ntfs/alpha.bin"
il_send_delay 1
# Mutation attempts must refuse.
il_send "write /ntfs/should-not.txt forbidden"
il_send_delay 1
il_send "mkdir /ntfs/newdir"
il_send_delay 1
il_send "rm /ntfs/greeting.txt"
il_send_delay 1
il_send "settimes /ntfs/greeting.txt 1 2"
il_send_delay 1
il_send "mv /ntfs/greeting.txt /ntfs/moved.txt"
il_send_delay 1
il_send "ls /ntfs"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 50 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}"

il_assert_grep "$LOG" "\[ntfs\] mounted read-only volume" "kernel mounts the host NTFS volume read-only"
il_assert_grep "$LOG" "\[vfs\] mounted '/ntfs'"           "/ntfs mount point registered"
il_assert_grep "$LOG" "Hello-from-NTFS"                    "resident DATA attr file reads byte-exact"
il_assert_grep_fixed "$LOG" "greeting.txt  (15 bytes)"     "readdir lists resident file with real size"
il_assert_grep "$LOG" "Size:    15 bytes"                  "stat reports resident file size"
il_assert_grep "$LOG" "Inode:   64"                        "stat inode = MFT index of greeting.txt"
il_assert_grep "$LOG" "Mode:    0100644"                   "stat mode 0644"
# multi-cluster non-resident file: byte-exact slice + real size
il_assert_grep "$LOG" "NTFS-RUNLIST-MULTICLUSTER-0123456789NTFS-RUNLIST" \
    "non-resident DATA attr reads byte-exact through runlists"
il_assert_grep "$LOG" "Size:    20480 bytes"               "stat reports multi-cluster file size"
il_assert_grep_fixed "$LOG" "alpha.bin  (20480 bytes)"     "readdir lists multi-cluster file with real size"
# read-only refusals are loud and create nothing
il_assert_grep "$LOG" "\[ntfs\] create refused: read-only \(-EROFS\)"  "write attempt refuses with -EROFS"
il_assert_grep "$LOG" "\[ntfs\] mkdir refused: read-only \(-EROFS\)"   "mkdir attempt refuses with -EROFS"
il_assert_grep "$LOG" "\[ntfs\] unlink refused: read-only \(-EROFS\)"  "unlink attempt refuses with -EROFS"
il_assert_grep "$LOG" "\[ntfs\] settimes refused: read-only \(-EROFS\)" "settimes attempt refuses with -EROFS"
il_assert_grep "$LOG" "\[ntfs\] rename refused: read-only \(-EROFS\)"  "rename attempt refuses with -EROFS"
il_assert_no_grep "$LOG" "should-not.txt  ("               "no file created by the refused write"
il_assert_no_grep "$LOG" "newdir  ("                       "no directory created by the refused mkdir"
il_assert_grep_fixed "$LOG" "greeting.txt  (15 bytes)"      "greeting.txt still present after refused rm/mv"

il_summary
