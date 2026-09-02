#!/usr/bin/env bash
# test_exfat.sh — F5 exFAT harness.
#
# exFAT uses the *real* on-disk format, so — unlike f2fs/btrfs — the host's
# exfatprogs binaries are fully applicable and the structural gate is a real
# interop check, exactly like ext4:
#   - host `fsck.exfat -fn` must report the volume CLEAN after the kernel's
#     own formatter AND after the full mutation surface (mkdir/write/read,
#     rename, stat, settimes, rm, rmdir, multi-cluster read).
#   - the kernel must read a volume created by host `mkfs.exfat` (external
#     lane): a Linux-created file is seeded onto the image and AuraLite
#     lists it, stats it (real size / cluster), and `cat`s its content
#     byte-exact.
#
# Three passes:
#   1. fsformat=1: the kernel formats a blank disk, then drives the full
#      mutation surface from the shell; the host then runs fsck.exfat on the
#      kernel-formatted volume and requires "clean".
#   2. external lane: host mkfs.exfat + seed_exfat.py place Linux files on
#      D2; the kernel mounts read-only-ish (fsformat=0) and reads them.
#      fsck.exfat again requires "clean".
#   3. persistence: the kernel-formatted volume remounts on fsformat=0
#      without being reformatted.
#
# Requires: qemu-system-x86_64, mkfs.exfat + fsck.exfat (exfatprogs), python3.
# Skips cleanly when the host tools are absent.

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
    echo "${C_RED}[lib] qemu-system-x86_64 missing; skipping exfat harness${C_RESET}"
    exit 0
fi
# exfatprogs may live under /usr/sbin, which is not always on PATH.
MKFS_EXFAT=$(command -v mkfs.exfat || echo /usr/sbin/mkfs.exfat)
FSCK_EXFAT=$(command -v fsck.exfat || echo /usr/sbin/fsck.exfat)
if [ ! -x "$MKFS_EXFAT" ] || [ ! -x "$FSCK_EXFAT" ]; then
    echo "${C_RED}[lib] exfatprogs (mkfs.exfat/fsck.exfat) missing; skipping exfat harness${C_RESET}"
    exit 0
fi
if ! il_have python3; then
    echo "${C_RED}[lib] python3 missing; skipping exfat harness${C_RESET}"
    exit 0
fi

il_section "exFAT (real on-disk format, full mutation surface + fsck.exfat + host interop)"

D0="$IL_BUILD/exfat-d0.img"; D1="$IL_BUILD/exfat-d1.img"
D2="$IL_BUILD/exfat-d2.img"; D3="$IL_BUILD/exfat-d3.img"
D4="$IL_BUILD/exfat-d4.img"; D5="$IL_BUILD/exfat-d5.img"
for d in "$D0" "$D1" "$D2" "$D3" "$D4" "$D5"; do rm -f "$d"; done
il_make_disk "$D0" 16 "AURALHCI"
il_make_disk "$D1" 16 "AURALHCI"
il_make_disk "$D2" 64 "AURALHCI"     # exFAT volume (blkdev 2 -> /exfat)
il_make_disk "$D3" 16 "AURALHCI"
il_make_disk "$D4" 16 "AURALHCI"
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
LOG1="$IL_LOGDIR/exfat_p1.log"
il_send_delay 9
il_send "mkdir /exfat/sub"
il_send_delay 1
il_send "write /exfat/sub/f1.txt hello-from-exfat"
il_send_delay 1
il_send "cat /exfat/sub/f1.txt"
il_send_delay 1
il_send "mv /exfat/sub/f1.txt /exfat/sub/f2.txt"
il_send_delay 1
il_send "stat /exfat/sub/f2.txt"
il_send_delay 1
il_send "mkdir /exfat/edir"
il_send_delay 1
il_send "write /exfat/multi.bin ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
il_send_delay 1
il_send "cat /exfat/multi.bin"
il_send_delay 1
il_send "settimes /exfat/sub/f2.txt 1700000000 1700000002"
il_send_delay 1
il_send "stat /exfat/sub/f2.txt"
il_send_delay 1
il_send "rm /exfat/sub/f2.txt"
il_send_delay 1
il_send "rmdir /exfat/sub"
il_send_delay 1
il_send "ls /exfat"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG1" 50 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=1"

il_assert_grep "$LOG1" "\\[exfat\\] format complete"      "kernel formatted blank exFAT volume"
il_assert_grep "$LOG1" "hello-from-exfat"                "exFAT write+read round-trip"
il_assert_grep "$LOG1" "mv: /exfat/sub/f1.txt -> /exfat/sub/f2.txt" "rename on exFAT"
il_assert_grep "$LOG1" "Size:    17 bytes"               "stat reports real size"
il_assert_grep "$LOG1" "Mode:    0100644"                "stat mode 0644"
il_assert_grep "$LOG1" "settimes: .*atime=1700000000 mtime=1700000002" "settimes accepted"
il_assert_grep "$LOG1" "ATime:   1700000000"             "settimes atime observed via stat"
il_assert_grep "$LOG1" "MTime:   1700000002"             "settimes mtime observed via stat"
il_assert_grep "$LOG1" "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" "multi-cluster read-back"
il_assert_grep "$LOG1" "rm: removed /exfat/sub/f2.txt"   "unlink on exFAT"
il_assert_grep "$LOG1" "rmdir: removed /exfat/sub"       "rmdir on exFAT"
il_assert_grep "$LOG1" "edir/"                           "ls shows directory"
il_assert_grep_fixed "$LOG1" "multi.bin  (37 bytes)"     "ls shows real file size"

# Host-side structural gate: the kernel-formatted volume must be fsck-clean.
FSCK_LOG="$IL_LOGDIR/exfat_p1_fsck.log"
"$FSCK_EXFAT" -n "$D2" > "$FSCK_LOG" 2>&1 || true
il_assert_grep "$FSCK_LOG" "clean" \
    "host fsck.exfat reports the kernel-formatted volume CLEAN"

# ---- Pass 2: external lane — read a host mkfs.exfat volume (fsformat=0) ----
LOG2="$IL_LOGDIR/exfat_p2.log"
rm -f "$D2"; il_make_disk "$D2" 64 "AURALHCI"
"$MKFS_EXFAT" -n EXT "$D2" >/dev/null 2>&1 || true
python3 "tests/exfat/seed_exfat.py" "$D2" greeting.txt "Hello-from-Linux-exFAT" || true
python3 "tests/exfat/seed_exfat.py" "$D2" alpha.bin "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" || true

il_send_delay 9
il_send "ls /exfat"
il_send_delay 1
il_send "cat /exfat/greeting.txt"
il_send_delay 1
il_send "cat /exfat/alpha.bin"
il_send_delay 1
il_send "stat /exfat/greeting.txt"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG2" 50 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=0"

il_assert_grep "$LOG2" "\\[exfat\\] mounted real exFAT volume" "kernel mounts a host-formatted exFAT volume"
il_assert_grep_fixed "$LOG2" "greeting.txt  (22 bytes)"  "readdir lists host file with real size"
il_assert_grep_fixed "$LOG2" "alpha.bin  (36 bytes)"     "readdir lists second host file"
il_assert_grep "$LOG2" "Hello-from-Linux-exFAT"         "kernel reads a Linux-created file byte-exact"
il_assert_grep "$LOG2" "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" "kernel reads second host file"

# Host-side gate for the external volume too.
FSCK2_LOG="$IL_LOGDIR/exfat_p2_fsck.log"
"$FSCK_EXFAT" -n "$D2" > "$FSCK2_LOG" 2>&1 || true
il_assert_grep "$FSCK2_LOG" "clean" \
    "host fsck.exfat reports the host-created volume CLEAN"

# ---- Pass 3: persistence — kernel volume remounts without reformat ----
LOG3="$IL_LOGDIR/exfat_p3.log"
rm -f "$D2"; il_make_disk "$D2" 64 "AURALHCI"
il_send_delay 9
il_send "write /exfat/keep.txt persisted-data"
il_send_delay 1
il_send "exit"
il_run_qemu "$LOG3" 40 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=1"

LOG3b="$IL_LOGDIR/exfat_p3b.log"
il_send_delay 9
il_send "cat /exfat/keep.txt"
il_send_delay 1
il_send "exit"
il_run_qemu "$LOG3b" 40 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=0"

il_assert_grep "$LOG3b" "\\[exfat\\] mounted real exFAT volume" "kernel volume remounted without reformat"
il_assert_grep "$LOG3b" "persisted-data"                       "data persisted across remount"

il_summary
