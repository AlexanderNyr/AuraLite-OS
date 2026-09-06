#!/usr/bin/env bash
# test_usb_ext2_automount.sh — RESIDUE2 T6 gate: ext2 hotplug automount.
#
# usbfs auto-detects an ext2 filesystem on the attached USB mass
# storage device (superblock magic + geometry + group-0 inode table)
# and exposes a READ-ONLY view at /usb/ext2 — the TODO box's second
# half.  The image is built host-side with the real mke2fs and
# populated with debugfs (external interop, the FSFULL precedent), so
# the guest parses genuine ext2 bytes, not a fixture we also wrote.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3
il_have /sbin/mke2fs
il_have /sbin/debugfs

il_section "usbfs ext2 hotplug automount (RESIDUE2 T6)"

USB="$IL_BUILD/usbfs-ext2.img"
GREET="$IL_BUILD/greet.txt"
truncate -s 8M "$USB"
printf 'EXT2-AUTOMOUNT-OK\n' > "$GREET"
/sbin/mke2fs -q -t ext2 -F -b 1024 "$USB" >/dev/null 2>&1
/sbin/debugfs -w -R "write $GREET GREET.TXT" "$USB" >/dev/null 2>&1

LOG="$IL_LOGDIR/usbfs_ext2_automount.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 14
il_send "ls /usb/ext2"
il_send_delay 2
il_send "cat /usb/ext2/GREET.TXT"
il_send_delay 2
il_send "cat /usb/info"
il_send_delay 2
il_send "echo gate-end"
il_send "exit"

il_run_qemu "$LOG" 55 \
    -device "piix3-usb-uhci,id=uhci" \
    -drive "file=$USB,format=raw,if=none,id=usbext2" \
    -device "usb-storage,bus=uhci.0,drive=usbext2"

il_assert_grep "$LOG" "\\[usbfs\\] ext2 detected: volume=[^ ]* block_size=1024" \
    "ext2 auto-detected at hotplug (magic + geometry)"
il_assert_grep "$LOG" "device available at /usb .*ext2/" \
    "the automount advertised /usb/ext2"
il_assert_grep "$LOG" "GREET\\.TXT" "the ext2 root listed GREET.TXT"
il_assert_grep_fixed "$LOG" "EXT2-AUTOMOUNT-OK" \
    "a real mke2fs/debugfs-written file read back in the guest"
il_assert_grep "$LOG" "ext2: detected" "/usb/info reports the ext2 view"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\\[msc\\] FAIL" \
    "no usbfs ext2 faults"
il_summary
