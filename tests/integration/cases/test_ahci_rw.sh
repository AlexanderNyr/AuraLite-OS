#!/usr/bin/env bash
# test_ahci_rw.sh — AHCI DMA read/write + diskfs end-to-end.
#
# Verifies:
#   1. Kernel AHCI self-test (sector 0 read + sector 1 write/readback).
#   2. /disk filesystem mount (diskfs).
#   3. From userspace: write a file to /disk, read it back via `cat`.
#   4. A disk with blank LBA0 is read successfully but never gets a scratch
#      write or a spurious DMA FAIL.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "AHCI /disk write-read"

DISK="$IL_BUILD/disk-ahci-test.img"
il_make_disk "$DISK" 16 "AURALHCI"

LOG="$IL_LOGDIR/ahci_rw.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Boot, drop into shell, exercise /disk and /fat.
il_send_delay 6
il_send "ls /disk"
il_send_delay 1
il_send "write /disk/ci.txt hello_from_ahci_$$"
il_send_delay 1
il_send "cat /disk/ci.txt"
il_send_delay 1
il_send "ls /fat"
il_send_delay 1
il_send "write /fat/CI.TXT fat32_works_$$"
il_send_delay 1
il_send "cat /fat/CI.TXT"
il_send_delay 1
il_send "exit"

# On a toolchain-staged ISO the first mount formats both diskfs and FAT32
# before the input queue runs; 25 seconds could expire just before the final
# FAT write/read without indicating an AHCI or filesystem failure.
il_run_qemu "$LOG" 60 \
    -drive "file=$DISK,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0"

# Kernel-side checks.
il_assert_grep "$LOG" "\[ahci\] .* SATA device"             "AHCI controller detects disk"
il_assert_grep "$LOG" "\[ahci\] PASS: SATA read/write DMA"  "AHCI DMA self-test"
il_assert_grep "$LOG" "\[diskfs\] PASS:"                    "diskfs mounted at /disk"
il_assert_grep "$LOG" "\[fat32\] PASS:"                     "FAT32 mounted at /fat"

# User-mode checks (file we just wrote can be read back).
il_assert_grep "$LOG" "hello_from_ahci_$$"                  "user wrote+read /disk/ci.txt"
il_assert_grep "$LOG" "fat32_works_$$"                      "user wrote+read /fat/CI.TXT"

# No errors during write.
il_assert_no_grep "$LOG" "\[ahci\] FAIL"                    "no AHCI failure"
il_assert_no_grep "$LOG" "\[diskfs\] FAIL"                  "no diskfs failure"
il_assert_no_grep "$LOG" "\[fat32\] FAIL"                   "no FAT32 failure"

# A raw FAT32 volume may start at LBA 64 (the Freedoom WAD layout), leaving
# LBA 0 entirely zero.  A successful read of zero bytes is NOT an AHCI DMA
# failure.  Do not write sector 1 of this unmarked disk as a scratch sector.
BLANK="$IL_BUILD/disk-ahci-blank.img"
dd if=/dev/zero of="$BLANK" bs=1M count=16 status=none
LOG2="$IL_LOGDIR/ahci_blank.log"
il_send_delay 8
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG2" 60 \
    -drive "file=$BLANK,format=raw,if=none,id=blankdisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=blankdisk,bus=ahci0.0"
il_assert_grep_fixed "$LOG2" "[ahci] self-test: blank LBA0 read successfully" \
    "zero-filled sector 0 is a valid read, not a DMA failure"
il_assert_no_grep "$LOG2" "\[ahci\] FAIL" \
    "blank disk never reports an AHCI failure"
il_assert_no_grep_fixed "$LOG2" "[ahci] self-test: writing scratch sector 1" \
    "blank data disk does not receive a scratch write"

il_summary
