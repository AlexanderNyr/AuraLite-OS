#!/usr/bin/env bash
# test_fsformat_knob.sh — FSFULL_PLAN.md F1: auto-format is opt-in.
#
#   Lane 1 (build default, knob OFF): five foreign volumes are attached
#   in the experimental slots (blkdev 2..5) and must be REFUSED with a
#   named line each, never formatted, never mounted — and the disks must
#   be byte-identical after boot (hash before/after).
#
#   Lane 2 (fw_cfg opt/auralite.fsformat=1): the same disks are
#   formatted and mounted by ext4/f2fs/btrfs and exfat (the exFAT
#   formatter landed in F5), each probed with a shell round-trip; the
#   NTFS driver probes every reachable device, finds no NTFS volume
#   among the six disks (a 7th would not fit this single 6-port AHCI
#   controller) and says so honestly.
#
#   Post-mount scope: /f2fs readdir (ls) plus an /exfat create+readdir
#   round-trip (touch + ls).  /ext4 trips a PRE-EXISTING kernel bug (KCANARY, even on plain `ls`;
#   reproduced on pristine origin/main 06be2ab, unrelated to F1) and
#   /btrfs's VFS create is unimplemented (skeleton, F4b territory), so
#   exercising them would gate F1 on unrelated breakage.  Format+mount
#   coverage for ext4/btrfs stays.
#
# Slots: blkdev 0 (diskfs/fat32) and 1 (ext2) are the mature filesystems'
# own disks and are formatted by THEM on first boot — that is their
# documented behaviour, outside F1's scope; the hash check therefore
# covers only the experimental slots 2..5.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FSFULL F1: auto-format knob (default refuse, fw_cfg opt-in)"

BUILD="${IL_BUILD:-build}"
mkdir -p "$BUILD"

# --- disks ----------------------------------------------------------------
# Slot 0/1: mature-FS disks (will be formatted by diskfs/fat32/ext2).
D0="$BUILD/fsf_mature0.img"
D1="$BUILD/fsf_mature1.img"
# Slots 2..5: foreign volumes for the experimental filesystems.
D2="$BUILD/fsf_exfat.img"
D3="$BUILD/fsf_ext4.img"
D4="$BUILD/fsf_btrfs.img"
D5="$BUILD/fsf_f2fs.img"
EXP_DISKS=("$D2" "$D3" "$D4" "$D5")
# f2fs format writes its backup superblock at page 8194 (33.5 MB), so the
# f2fs disk must be >= 64 MB or format_f2fs fails writing past the end.
F2FS_DISK_MB=64

for d in "$D0" "$D1"; do
    rm -f "$d"
    dd if=/dev/zero of="$d" bs=1M count=64 status=none
done
for d in "$D2" "$D3" "$D4"; do
    rm -f "$d"
    dd if=/dev/zero of="$d" bs=1M count=8 status=none
done
for d in "$D5"; do
    rm -f "$d"
    dd if=/dev/zero of="$d" bs=1M count="$F2FS_DISK_MB" status=none
done
for d in "${EXP_DISKS[@]}"; do
    python3 - "$d" <<'PY'
import sys
path = sys.argv[1]
sector = bytearray(b'\x5A' * 512)
sector[3:11] = b"NOTAFS!X"         # 8-byte OEM field for exFAT/NTFS probes
sector[510] = 0x55                 # MBR-ish signature so the disk
sector[511] = 0xAA                 # looks like "someone's media"
with open(path, "r+b") as f:
    f.write(sector)
PY
done

EXP_HASH_BEFORE=$(sha256sum "${EXP_DISKS[@]}" | sha256sum | cut -d' ' -f1)

QARGS=(
    -device "ahci,id=ahci0"
    -drive "file=$D0,format=raw,if=none,id=ds0" -device "ide-hd,drive=ds0,bus=ahci0.0"
    -drive "file=$D1,format=raw,if=none,id=ds1" -device "ide-hd,drive=ds1,bus=ahci0.1"
    -drive "file=$D2,format=raw,if=none,id=ds2" -device "ide-hd,drive=ds2,bus=ahci0.2"
    -drive "file=$D3,format=raw,if=none,id=ds3" -device "ide-hd,drive=ds3,bus=ahci0.3"
    -drive "file=$D4,format=raw,if=none,id=ds4" -device "ide-hd,drive=ds4,bus=ahci0.4"
    -drive "file=$D5,format=raw,if=none,id=ds5" -device "ide-hd,drive=ds5,bus=ahci0.5"
)

# ============================================================================
# Lane 1 — default build: refuse, do not touch the disks
# ============================================================================
il_section "Lane 1: default (FS_MOUNT_FORMAT=0) — refuse + no writes"

LOG="$IL_LOGDIR/fsformat_default.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "ls /ext4"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOG" 90 "${QARGS[@]}"

# The knob state line is greppable and names the source.
il_assert_grep "$LOG" "\[fsformat\] auto-format: DISABLED \(build\)" \
    "auto-format gate is DISABLED from the build default"

# Each experimental FS names its refusal.
il_assert_grep "$LOG" "\[exfat\] not exFAT signature.*exfat not mounted" \
    "exFAT refuses the foreign OEM"
il_assert_grep "$LOG" "\[ext4\] not ext4 magic.*format disabled" \
    "ext4 refuses the foreign superblock"
il_assert_grep "$LOG" "\[btrfs\].*format disabled" \
    "btrfs refuses the foreign volume (unreadable or bad magic)"
il_assert_grep "$LOG" "\[f2fs\] not F2FS magic.*format disabled" \
    "f2fs refuses the foreign superblock"
il_assert_grep "$LOG" "\[ntfs\] no NTFS volume on a reachable device; /ntfs not mounted" \
    "NTFS probes all reachable devices and honestly reports none is NTFS"

# No formatter ran, no mount happened.
il_assert_no_grep "$LOG" "\[(ext4|f2fs|btrfs)\].*formatting" \
    "no experimental formatter ran (diskfs/fat32/ext2 format their own slots)"
il_assert_no_grep "$LOG" "mounted '/exfat'" \
    "/exfat never mounted"
il_assert_no_grep "$LOG" "mounted '/ext4'" \
    "/ext4 never mounted"
il_assert_no_grep "$LOG" "mounted '/btrfs'" \
    "/btrfs never mounted"
il_assert_no_grep "$LOG" "mounted '/f2fs'" \
    "/f2fs never mounted"
il_assert_no_grep "$LOG" "mounted '/ntfs'" \
    "/ntfs never mounted"

# The experimental disks are byte-identical: refusal wrote nothing.
EXP_HASH_AFTER=$(sha256sum "${EXP_DISKS[@]}" | sha256sum | cut -d' ' -f1)
if [ "$EXP_HASH_BEFORE" = "$EXP_HASH_AFTER" ]; then
    il_pass "foreign volumes byte-identical after boot (no writes)"
else
    il_fail "foreign volumes CHANGED after boot — a driver wrote to a refused disk"
fi

# ============================================================================
# Lane 2 — fw_cfg opt/auralite.fsformat=1: format + mount + round-trip
# ============================================================================
il_section "Lane 2: fw_cfg fsformat=1 — format and mount"

LOG2="$IL_LOGDIR/fsformat_enabled.log"
IL_LAST_LOG="$LOG2"

il_send_prompt "ls /f2fs"
il_send_prompt "touch /exfat/hello.txt"
il_send_prompt "ls /exfat"
il_send_prompt "exit"

IL_SMP=1 IL_SELFTEST=off il_run_qemu_prompt "$LOG2" 150 \
    -fw_cfg "name=opt/auralite.fsformat,string=1" "${QARGS[@]}"

il_assert_grep "$LOG2" "\[fsformat\] auto-format: ENABLED \(fw_cfg\)" \
    "auto-format gate ENABLED via fw_cfg"

il_assert_grep "$LOG2" "\[ext4\] not ext4 magic.*formatting" \
    "ext4 formats the foreign volume (opt-in)"
il_assert_grep "$LOG2" "\[vfs\] mounted '/ext4'" \
    "/ext4 mounted"
il_assert_grep "$LOG2" "\[f2fs\] not F2FS magic.*formatting" \
    "f2fs formats the foreign volume (opt-in)"
il_assert_grep "$LOG2" "\[vfs\] mounted '/f2fs'" \
    "/f2fs mounted"
il_assert_grep "$LOG2" "\[btrfs\].*formatting" \
    "btrfs formats the foreign volume (opt-in)"
il_assert_grep "$LOG2" "\[vfs\] mounted '/btrfs'" \
    "/btrfs mounted"

# The mounted /f2fs volume serves VFS I/O: `ls /f2fs` readdir returns
# the root entries ("./", "../").  A bare format+mount without a live
# driver would print nothing here.
il_assert_grep "$LOG2" "\.\./" \
    "f2fs root readdir served by the mounted volume (ls /f2fs)"

# exFAT has a formatter since F5: the opt-in lane formats, mounts and
# round-trips a file through VFS.
il_assert_grep "$LOG2" "\[exfat\] not exFAT signature, formatting" \
    "exFAT formatter runs on the foreign volume (opt-in)"
il_assert_grep "$LOG2" "\[vfs\] mounted '/exfat'" \
    "/exfat mounted"
il_assert_grep "$LOG2" "touch: /exfat/hello.txt" \
    "exFAT create via VFS works (touch /exfat/hello.txt)"
il_assert_grep "$LOG2" "hello.txt.*bytes" \
    "exFAT readdir serves the created file (ls /exfat)"

# NTFS has no volume it can reach in either lane and says so.
il_assert_grep "$LOG2" "\[ntfs\] no NTFS volume on a reachable device; /ntfs not mounted" \
    "NTFS honestly reports no reachable NTFS volume (opt-in lane)"

il_summary
