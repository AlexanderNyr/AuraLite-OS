#!/usr/bin/env bash
# test_ahci_rw.sh — AHCI DMA read/write + diskfs end-to-end.
#
# Verifies:
#   1. Kernel AHCI self-test (sector 0 read + sector 1 write/readback).
#   2. /disk filesystem mount (diskfs).
#   3. From userspace: write a file to /disk, read it back via `cat`.

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

il_run_qemu "$LOG" 25 \
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

il_summary
