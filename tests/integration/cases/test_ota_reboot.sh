#!/usr/bin/env bash
# test_ota_reboot.sh — OTA_PLAN O2: the machine can restart itself, and a
# build can carry a distinct version identity.
#
# Lane A (reboot, one serial log): boot the dual image from AHCI WITHOUT
# -no-reboot, run `reboot`, and the SAME log must show the second boot —
# kernel banner, stage2-mounted ESP (the O1 invariant holds across the
# reset), shell prompt, and an answered `uname`.  This is the OTA flow's
# second-boot leg in miniature: the reset re-enters the bootloader on the
# same in-process snapshot overlay, exactly what `ota apply` + reboot will
# do to boot a freshly installed kernel.
#
# Lane B (identity): a cold build with AURALITE_VERSION=0.0.2-ota (throwaway
# BUILD_DIR — object files do not depend on the Makefile, so an override on
# a warm tree can silently keep stale objects; cold is the only honest
# build) must print the overridden version in ALL THREE places: the kernel
# boot receipt, the shell banner, and `uname`.  This is the O4 proof
# vehicle: "the second boot prints the new version" needs the version to be
# a build knob first.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3 make

il_section "OTA O2: reboot and version identity"

# ---- Lane A: reboot; both boots in one serial log -------------------------
LOGA="$IL_LOGDIR/ota_reboot_a.log"
IL_LAST_LOG="$LOGA"
rm -f "$LOGA"

# Hand-rolled QEMU, deliberately WITHOUT -no-reboot (il_run_qemu hardwires
# it): the 8042 pulse must reset the machine, and the same process's serial
# log must carry both boots.  Input is a PIPE -- a still-empty file would
# hand QEMU an immediate EOF on stdin (see test_ota_bootvol.sh's lane A
# note).  Boot-to-prompt with selftest=fast measures ~13 s; the second boot
# gets 22 s before `uname` is typed.
( { sleep 14; printf 'uname\n'; sleep 2; printf 'reboot\n'; sleep 22;
    printf 'uname\n'; sleep 2; printf 'exit\n'; } \
  | timeout 120 qemu-system-x86_64 \
    -drive "file=$IL_ISO,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci0 -device ide-hd,drive=btndisk,bus=ahci0.0 \
    -m 512M -smp 2 -display none -serial stdio -cpu qemu64 -boot order=c \
    -fw_cfg "name=opt/auralite.selftest,string=fast" \
    > "$LOGA" 2>&1 ) || true

il_assert_grep "$LOGA" "\[reboot\] SYS_REBOOT: flushing filesystems" \
    "the kernel flushes filesystems before the reset"
il_assert_grep "$LOGA" "\[reboot\] pulsing 8042 reset" \
    "and pulses the 8042 reset line"
il_assert_no_grep "$LOGA" "8042 pulse ignored" \
    "the reset was real, not the halt fallback"
il_assert_count "$LOGA" "Hello from AuraLite" 2 \
    "the same serial log carries TWO boots"
il_assert_count "$LOGA" "\[kernel\] AuraLite OS version" 2 \
    "both boots printed the kernel identity receipt"
il_assert_count "$LOGA" "found FAT32 partition at LBA 256 \(via GPT\)" 2 \
    "the O1 boot-volume invariant survives the reset (ESP via GPT, twice)"
il_assert_grep "$LOGA" "AuraLite OS 0.0.1 x86_64" \
    "uname answers with the default identity (stock build)"

# ---- Lane B: AURALITE_VERSION override, cold build -------------------------
LOGB="$IL_LOGDIR/ota_reboot_b.log"
IL_LAST_LOG="$LOGB"
rm -f "$LOGB"

BUILDD="$(mktemp -d /tmp/ota-o2-build.XXXXXX)"
trap 'rm -rf "$BUILDD"' EXIT

# Cold build in a throwaway BUILD_DIR: nothing in build/ is touched, and
# every object is guaranteed to carry the override define.
make -C "$IL_ROOT" iso BUILD_DIR="$BUILDD" AURALITE_VERSION=0.0.2-ota \
    > "$IL_LOGDIR/ota_reboot_b_build.log" 2>&1
if [ $? -ne 0 ]; then
    tail -5 "$IL_LOGDIR/ota_reboot_b_build.log"
    il_assert_grep "$IL_LOGDIR/ota_reboot_b_build.log" "^THIS_WILL_NOT_MATCH$" \
        "the AURALITE_VERSION=0.0.2-ota build completes (see ota_reboot_b_build.log)"
    il_summary
    exit 1
fi

il_send_delay 2
il_send "uname"
il_send_delay 1
il_send "exit"

IL_ISO="$BUILDD/auralite.iso" IL_SELFTEST=fast \
    il_run_qemu "$LOGB" 90 \
    -drive "file=$BUILDD/auralite.iso,format=raw,if=none,id=btndisk,snapshot=on" \
    -device ahci,id=ahci1 -device ide-hd,drive=btndisk,bus=ahci1.0

il_assert_grep "$LOGB" "\[kernel\] AuraLite OS version 0.0.2-ota" \
    "the kernel boot receipt carries the overridden version"
il_assert_grep "$LOGB" "AuraLite OS v0.0.2-ota — Interactive Shell" \
    "the shell banner carries the overridden version"
il_assert_grep "$LOGB" "AuraLite OS 0.0.2-ota x86_64" \
    "uname carries the overridden version"
il_assert_no_grep "$LOGB" "0\.0\.1" \
    "no stale stock-version print anywhere in the override boot"

il_summary
