#!/usr/bin/env bash
# test_fscheck.sh — RESIDUE2 T3: the FAT32/ext2 consistency walkers.
#
# Lane 1 (clean): a /fat and an /ext2 volume are formatted and written
# by the guest in boot 1, re-mounted in boot 2 with the fscheck knob
# armed, and both walkers must report CLEAN.
#
# Lane 2 (corrupt): the SAME images are damaged on the host between
# boots — FSInfo free-count drift on FAT32, superblock free-block
# drift on ext2 — and boot 3 must print the NAMED findings.  This is
# the guest-side twin of tests/unit/test_fscheck.c: same walker object,
# same finding strings, real volumes instead of crafted ones.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "fscheck: FAT32/ext2 consistency walkers (RESIDUE2 T3)"

BUILD="${IL_BUILD:-build}"
FAT_DISK="$BUILD/fscheck-fat.img"
EXT2_DISK="$BUILD/fscheck-ext2.img"
rm -f "$FAT_DISK" "$EXT2_DISK"
il_make_disk "$FAT_DISK" 16 "AURALHCI"
il_make_disk "$EXT2_DISK" 8 "AURALEXT"

QARGS=(
    -drive "file=$FAT_DISK,format=raw,if=none,id=fc0"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=fc0,bus=ahci0.0"
    -drive "file=$EXT2_DISK,format=raw,if=none,id=fc1"
    -device "ide-hd,drive=fc1,bus=ahci0.1"
)

# ============================================================================
# Boot 1 — format both volumes and leave non-trivial content on them.
# ============================================================================
LOG1="$IL_LOGDIR/fscheck_boot1.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "write /fat/FSCHECK.TXT clean-lane-marker"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOG1" 30 "${QARGS[@]}"
il_assert_grep "$LOG1" "\[fat32\] PASS:" "boot1: FAT32 mounted/formatted"
il_assert_grep "$LOG1" "\[ext2\]"        "boot1: ext2 present"

# ============================================================================
# Boot 2 — clean volumes, knob armed: both walkers say CLEAN.
# ============================================================================
LOG2="$IL_LOGDIR/fscheck_clean.log"
IL_LAST_LOG="$LOG2"

il_send_delay 6
il_send "cat /fat/FSCHECK.TXT"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOG2" 30 \
    -fw_cfg "name=opt/auralite.fscheck,string=1" "${QARGS[@]}"

il_assert_grep "$LOG2" "\[fscheck\] consistency walk: ENABLED \(fw_cfg\)" \
    "the fscheck gate arms from fw_cfg"
il_assert_grep "$LOG2" "\[fscheck\] fat32: CLEAN" \
    "the FAT32 walker reports a clean volume"
il_assert_grep "$LOG2" "\[fscheck\] ext2: CLEAN" \
    "the ext2 walker reports a clean volume"
il_assert_grep "$LOG2" "clean-lane-marker" \
    "the volume still serves its content"

# ============================================================================
# Boot 3 — corrupt both images on the host: named findings must appear.
# ============================================================================
python3 - "$FAT_DISK" "$EXT2_DISK" <<'PY'
import sys
fat, ext2 = sys.argv[1], sys.argv[2]

# FAT32: the superfloppy's FSInfo sits at volume sector 1 → absolute
# sector 64 + 1; the free-count hint is a u32 at byte 488.  Lie about
# five free clusters.
with open(fat, "r+b") as f:
    f.seek((64 + 1) * 512 + 488)
    v = int.from_bytes(f.read(4), "little")
    f.seek((64 + 1) * 512 + 488)
    f.write((v + 5).to_bytes(4, "little"))

# ext2: superblock free-blocks count is a u32 at byte 1024 + 12.
# Lie about seven blocks.
with open(ext2, "r+b") as f:
    f.seek(1024 + 12)
    v = int.from_bytes(f.read(4), "little")
    f.seek(1024 + 12)
    f.write((v + 7).to_bytes(4, "little"))
PY

LOG3="$IL_LOGDIR/fscheck_corrupt.log"
IL_LAST_LOG="$LOG3"

il_send_delay 6
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOG3" 30 \
    -fw_cfg "name=opt/auralite.fscheck,string=1" "${QARGS[@]}"

il_assert_grep "$LOG3" "\[fscheck\] fat32: FINDING: FSInfo free count" \
    "FAT32 walker names the FSInfo drift"
il_assert_grep "$LOG3" "\[fscheck\] fat32: [1-9][0-9]* finding\(s\)" \
    "FAT32 walker counts its findings"
il_assert_grep "$LOG3" "\[fscheck\] ext2: FINDING: superblock free blocks" \
    "ext2 walker names the superblock drift"
il_assert_grep "$LOG3" "\[fscheck\] ext2: [1-9][0-9]* finding\(s\)" \
    "ext2 walker counts its findings"

il_summary
