#!/usr/bin/env bash
# test_selfhost_build.sh — SELFHOST_PLAN.md SH6f: build.sh + D6 resume.
#
# Boot #1 stages the /fat worktree, stops at phase 6 of 9, then resumes
# through `sh build.sh kernel` and greps the §8 receipt.
# Boot #2 attaches the same FAT disk: the receipt prints again and
# KERNEL/INITRD are reported up to date (not rebuilt).  That is the
# difference between a resumable build and a shell script that starts
# over every boot.
#
# Needs no guest toolchain: recipes are sh6e_stamp, so like SH6a–SH6e
# this case never skips.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host entry (SH6f): build.sh on /fat, resume across reboot"

DISK="$IL_BUILD/selfhost-sh6f-fat.img"
rm -f "$DISK"
il_make_disk "$DISK" 32 "AURSH6F!"

AHCI=(
    -drive "file=$DISK,format=raw,if=none,id=sh6fdisk"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=sh6fdisk,bus=ahci0.0"
)

# ---- Boot #1: stop at 6, then resume to the receipt ----
LOG1="$IL_LOGDIR/selfhost_build_boot1.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh6f_boot1.sh"
il_send_delay 16
il_send "echo SH6F_BOOT1_ALIVE"
il_send_delay 2
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG1" 70 "${AHCI[@]}"

il_assert_grep "$LOG1" "\\[fat32\\] mounted FAT32 at /fat" \
    "boot1: FAT32 mounted at /fat"
il_assert_grep "$LOG1" "\\[selfhost\\] sh6f: stopped-at-6" \
    "boot1: the run stopped at phase 6 of 9"
il_assert_grep "$LOG1" "\\[selfhost\\] sh6e: gkern rebuilt" \
    "boot1: resume built KERNEL"
il_assert_grep "$LOG1" "\\[selfhost\\] sh6e: ginit rebuilt" \
    "boot1: resume built INITRD"
il_assert_grep_fixed "$LOG1" "[selfhost] build PASS: kernel+initrd built on /fat" \
    "boot1: the §8 receipt after resume"
il_assert_grep "$LOG1" "^SH6F_BOOT1_ALIVE$" \
    "boot1: the shell still takes commands afterwards"

# ---- Boot #2: same disk, nothing to rebuild ----
LOG2="$IL_LOGDIR/selfhost_build_boot2.log"
IL_LAST_LOG="$LOG2"

il_send_delay 8
il_send "sh /tests/sh6f_boot2.sh"
il_send_delay 12
il_send "echo SH6F_BOOT2_ALIVE"
il_send_delay 2
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG2" 60 "${AHCI[@]}"

il_assert_grep "$LOG2" "\\[fat32\\] mounted FAT32 at /fat" \
    "boot2: FAT32 remounted the same volume"
il_assert_grep_fixed "$LOG2" "[selfhost] build PASS: kernel+initrd built on /fat" \
    "boot2: the receipt printed again"
il_assert_grep "$LOG2" "\\[selfhost\\] sh6f: resumed-from-fat" \
    "boot2: the resume probe ran"
il_assert_no_grep "$LOG2" "gkern rebuilt" \
    "boot2: KERNEL was not rebuilt"
il_assert_no_grep "$LOG2" "ginit rebuilt" \
    "boot2: INITRD was not rebuilt"
il_assert_grep "$LOG2" "is up to date" \
    "boot2: shmake reported up-to-date targets"
il_assert_grep "$LOG2" "^SH6F_BOOT2_ALIVE$" \
    "boot2: the shell still takes commands afterwards"

il_summary
