#!/usr/bin/env bash
# test_fat32_persistence.sh — write a file to /fat in one boot, reboot,
# read it back in the second boot.  Verifies that FAT32 actually persists
# data to the AHCI-backed disk image across power cycles.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FAT32 persistence across reboots"

# Use a private disk that lives across both boots.
DISK="$IL_BUILD/fat32-persist.img"
rm -f "$DISK"                       # always start from scratch
il_make_disk "$DISK" 32 "AURALHCI"

# Marker we'll write & expect back.
MARKER="PERSIST_$(date +%s)_$$"

# ---- Boot #1: write the file ----
LOG1="$IL_LOGDIR/fat32_persist_boot1.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

# No il_send_delay here: the feeder already gates the FIRST line on the
# shell's own prompt (lib.sh's CONSUMPTION-gated feed), so a leading sleep
# only ADDS to the budget.  It used to be 6 s of a 25 s window -- and a boot
# on this tree reaches its prompt at ~19 s, so the command was typed after
# QEMU's SIGTERM and the case failed for a reason that had nothing to do
# with persistence.  Budget = prompt (~20 s) + work, not prompt + sleep.
il_send "write /fat/PERSIST.TXT $MARKER"
il_send_delay 2
il_send "cat /fat/PERSIST.TXT"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG1" 60 \
    -drive "file=$DISK,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0"

il_assert_grep "$LOG1" "\[fat32\] PASS:"   "boot1: FAT32 mounted"
il_assert_grep "$LOG1" "$MARKER"           "boot1: marker written and read"

# ---- Boot #2: read it back (no write) ----
LOG2="$IL_LOGDIR/fat32_persist_boot2.log"
IL_LAST_LOG="$LOG2"

il_send "ls /fat"
il_send_delay 1
il_send "cat /fat/PERSIST.TXT"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG2" 60 \
    -drive "file=$DISK,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0"

il_assert_grep "$LOG2" "\[fat32\] PASS:"   "boot2: FAT32 mounted again"
il_assert_grep "$LOG2" "PERSIST.TXT"       "boot2: file listed in /fat"
il_assert_grep "$LOG2" "$MARKER"           "boot2: marker survived reboot"

il_summary
