#!/usr/bin/env bash
# test_usbfs_fat32.sh — usbfs FAT32 auto-detect and read-only root file access.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "usbfs FAT32 auto-detect"

USB="$IL_BUILD/usbfs-fat32.img"
python3 - "$USB" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
size = 8 * 1024 * 1024
sec = 512
img = bytearray(size)
# FAT32 superfloppy: reserved=32, FAT sectors=64, data starts at LBA 96.
b = bytearray(512)
b[0:3] = b'\xeb\x58\x90'
b[3:11] = b'MSWIN4.1'
b[11:13] = (512).to_bytes(2, 'little')
b[13] = 1
b[14:16] = (32).to_bytes(2, 'little')
b[16] = 1
b[21] = 0xF8
b[32:36] = (size // sec).to_bytes(4, 'little')
b[36:40] = (64).to_bytes(4, 'little')
b[44:48] = (2).to_bytes(4, 'little')
b[48:50] = (1).to_bytes(2, 'little')
b[50:52] = (6).to_bytes(2, 'little')
b[64] = 0x80
b[66] = 0x29
b[67:71] = (0xA1231234).to_bytes(4, 'little')
b[71:82] = b'AURAL USB  '
b[82:90] = b'FAT32   '
b[510] = 0x55
b[511] = 0xAA
img[0:512] = b
f = bytearray(512)
f[0:4] = (0x41615252).to_bytes(4, 'little')
f[484:488] = (0x61417272).to_bytes(4, 'little')
f[488:492] = (0xffffffff).to_bytes(4, 'little')
f[492:496] = (4).to_bytes(4, 'little')
f[508:512] = (0xaa550000).to_bytes(4, 'little')
img[512:1024] = f
fat_off = 32 * sec
for cl, val in [(0, 0x0ffffff8), (1, 0x0fffffff), (2, 0x0fffffff), (3, 0x0fffffff)]:
    img[fat_off + cl*4:fat_off + cl*4 + 4] = val.to_bytes(4, 'little')
root = 96 * sec
content = b'USB FAT32 OK\n'
img[root:root+11] = b'HELLO   TXT'
img[root+11] = 0x20
img[root+26:root+28] = (3).to_bytes(2, 'little')
img[root+28:root+32] = len(content).to_bytes(4, 'little')
img[root+32] = 0
img[97*sec:97*sec + len(content)] = content
p.write_bytes(img)
PY

LOG="$IL_LOGDIR/usbfs_fat32.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "ls /usb"
il_send "cat /usb/info"
il_send "ls /usb/fat"
il_send "cat /usb/fat/HELLO.TXT"
il_send "exit"

il_run_qemu "$LOG" 45 \
    -device "qemu-xhci,id=xhci" \
    -drive "file=$USB,format=raw,if=none,id=usbfat" \
    -device "usb-storage,bus=xhci.0,drive=usbfat"

il_assert_grep "$LOG" "\[usbfs\] FAT32 detected" "usbfs detected FAT32"
il_assert_grep "$LOG" "fat32: detected" "usbfs info reports FAT32"
il_assert_grep "$LOG" "fat/" "usbfs exposes fat directory"
il_assert_grep "$LOG" "HELLO\.TXT" "FAT32 root lists HELLO.TXT"
il_assert_grep "$LOG" "USB FAT32 OK" "FAT32 file read works"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[msc\] FAIL|cat: /usb/fat/HELLO.TXT" \
    "no usbfs FAT32 faults"

il_summary
