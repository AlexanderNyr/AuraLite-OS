#!/usr/bin/env bash
# test_ahci_matrix.sh — RESIDUE2 T3: AHCI breadth.
#
# The gate for "broaden AHCI beyond the QEMU test path" is a MATRIX —
# what ELSE the driver boots on — not a unit test:
#
#   Lane A  pc  + one  -device ahci, disk at port 0   (baseline shape)
#   Lane B  pc  + one  -device ahci, disk at port 1   (empty port 0 skipped)
#   Lane C  pc  + TWO  -device ahci, one disk each    (multi-controller scan)
#   Lane D  q35 chipset with its onboard ICH9 AHCI    (different platform)
#
# Every lane must reach the interactive shell and exercise its disks.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "ahci matrix: breadth across controller topologies (RESIDUE2 T3)"

BUILD="${IL_BUILD:-build}"
DISK_A="$BUILD/ahci-matrix-a.img"
DISK_C0="$BUILD/ahci-matrix-c0.img"
DISK_C1="$BUILD/ahci-matrix-c1.img"
DISK_D="$BUILD/ahci-matrix-d.img"
rm -f "$DISK_A" "$DISK_C0" "$DISK_C1" "$DISK_D"
il_make_disk "$DISK_A"  16 "AHCIMATA"
il_make_disk "$DISK_C0" 16 "AHCIMC00"
il_make_disk "$DISK_C1"  8 "AHCIMC01"
il_make_disk "$DISK_D"  16 "AHCIMQ35"

# ----------------------------------------------------------------------------
# Lane A — baseline: single controller, disk at port 0.
# ----------------------------------------------------------------------------
LOGA="$IL_LOGDIR/ahci_matrix_a.log"
IL_LAST_LOG="$LOGA"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "write /fat/MATRIXA.TXT lane_a_ok"
il_send_delay 1
il_send "cat /fat/MATRIXA.TXT"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOGA" 30 \
    -drive "file=$DISK_A,format=raw,if=none,id=ma0" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ma0,bus=ahci0.0"

il_assert_grep "$LOGA" "\[ahci\] controller 0 at PCI" "lane A: controller 0 bound"
il_assert_grep "$LOGA" "\[ahci\] hw port 0: SATA disk" "lane A: disk on hw port 0"
il_assert_grep "$LOGA" "\[ahci\] PASS: SATA read/write DMA" "lane A: DMA self-test"
il_assert_grep "$LOGA" "lane_a_ok" "lane A: FAT32 file round-trip"

# ----------------------------------------------------------------------------
# Lane B — same controller, disk at port 1, port 0 left empty.
# ----------------------------------------------------------------------------
LOGB="$IL_LOGDIR/ahci_matrix_b.log"
IL_LAST_LOG="$LOGB"

il_send_delay 6
il_send "write /fat/MATRIXB.TXT lane_b_ok"
il_send_delay 1
il_send "cat /fat/MATRIXB.TXT"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOGB" 30 \
    -drive "file=$DISK_A,format=raw,if=none,id=mb0" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=mb0,bus=ahci0.1"

il_assert_grep "$LOGB" "\[ahci\] hw port 1: SATA disk" "lane B: disk on hw port 1"
il_assert_no_grep "$LOGB" "\[ahci\] hw port 0: SATA disk" "lane B: empty port 0 skipped"
il_assert_grep "$LOGB" "lane_b_ok" "lane B: FAT32 works on port 1"

# ----------------------------------------------------------------------------
# Lane C — two AHCI controllers, one disk on each.
# ----------------------------------------------------------------------------
LOGC="$IL_LOGDIR/ahci_matrix_c.log"
IL_LAST_LOG="$LOGC"

il_send_delay 6
il_send "write /fat/MATRIXC.TXT lane_c_fat"
il_send_delay 1
il_send "cat /fat/MATRIXC.TXT"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOGC" 40 \
    -drive "file=$DISK_C0,format=raw,if=none,id=mc0" \
    -drive "file=$DISK_C1,format=raw,if=none,id=mc1" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=mc0,bus=ahci0.0" \
    -device "ahci,id=ahci1" \
    -device "ide-hd,drive=mc1,bus=ahci1.0"

il_assert_grep "$LOGC" "\[ahci\] controller 0 at PCI" "lane C: controller 0 bound"
il_assert_grep "$LOGC" "\[ahci\] controller 1 at PCI" "lane C: controller 1 bound"
il_assert_grep "$LOGC" "2 controller\(s\), 2 SATA device\(s\) ready" \
    "lane C: both controllers yield a device"
il_assert_grep "$LOGC" "\[fat32\] PASS:" "lane C: FAT32 on controller 0 disk"
il_assert_grep "$LOGC" "\[ext2\]"        "lane C: ext2 takes controller 1 disk"
il_assert_grep "$LOGC" "lane_c_fat"      "lane C: file round-trip"

# ----------------------------------------------------------------------------
# Lane D — the q35 chipset: onboard ICH9 AHCI instead of -device ahci.
# ----------------------------------------------------------------------------
LOGD="$IL_LOGDIR/ahci_matrix_d.log"
IL_LAST_LOG="$LOGD"

il_send_delay 6
il_send "write /fat/MATRIXD.TXT lane_d_ok"
il_send_delay 1
il_send "cat /fat/MATRIXD.TXT"
il_send_delay 1
il_send "exit"

IL_SELFTEST=off il_run_qemu "$LOGD" 40 \
    -machine q35 \
    -drive "file=$DISK_D,format=raw,if=none,id=md0" \
    -device "ide-hd,drive=md0,bus=ide.1"

il_assert_grep "$LOGD" "\[ahci\] controller 0 at PCI" "lane D: q35 AHCI bound"
il_assert_grep "$LOGD" "\[ahci\] hw port 0: SATA disk" "lane D: disk seen on q35"
il_assert_grep "$LOGD" "\[ahci\] PASS: SATA read/write DMA" "lane D: DMA on q35"
il_assert_grep "$LOGD" "lane_d_ok" "lane D: FAT32 round-trip on q35"

il_summary
