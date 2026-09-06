#!/usr/bin/env bash
# test_usb_fat32_write.sh — RESIDUE2 T6 gate: WRITABLE FAT32 on USB.
#
# The FAT32 layer in usbfs was read-only.  This gate closes the honest
# first half of writability: in-place overwrites of an existing file
# (plus growth into the last cluster's slack), written through MSC
# WRITE(10) and VERIFIED ON THE HOST from the raw image after the
# guest exits — persistence on the media, not just a guest-side echo.
#
# New-cluster allocation (creating files) stays future work; the
# receipt wording below says exactly that.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "usbfs writable FAT32 (RESIDUE2 T6)"

USB="$IL_BUILD/usbfs-fat32-write.img"
python3 - "$USB" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
size = 8 * 1024 * 1024
sec = 512
img = bytearray(size)
# FAT32 superfloppy (same layout as test_usbfs_fat32.sh):
# reserved=32, 1 FAT x 64 sectors, data at LBA 96, cluster = 1 sector.
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
for cl, val in [(0, 0x0ffffff8), (1, 0x0fffffff), (2, 0x0fffffff),
                (3, 0x0fffffff), (4, 0x0fffffff), (5, 0x0fffffff)]:
    img[fat_off + cl*4:fat_off + cl*4 + 4] = val.to_bytes(4, 'little')
root = 96 * sec
hello = b'USB FAT32 OK\n'
img[root:root+11] = b'HELLO   TXT'
img[root+11] = 0x20
img[root+26:root+28] = (3).to_bytes(2, 'little')
img[root+28:root+32] = len(hello).to_bytes(4, 'little')
img[97*sec:97*sec + len(hello)] = hello
# WRITE.TXT: 512 bytes of old content in cluster 4 (LBA 98) — the
# write target, with slack to grow into.  NOTE: the entry sits directly
# after HELLO.TXT (offset 32): a zero byte ends a FAT directory scan.
old = b'T6-OLD-CONTENT ' + b'O' * (512 - 15)
e2 = root + 32
img[e2:e2+11] = b'WRITE   TXT'
img[e2+11] = 0x20
img[e2+26:e2+28] = (4).to_bytes(2, 'little')
img[e2+28:e2+32] = (512).to_bytes(4, 'little')
img[98*sec:99*sec] = old
# GROW.TXT: 10 bytes in cluster 5 (LBA 99) — size < cluster, so a
# longer write grows into the slack and must patch the dirent size.
grow_old = b'GROW-OLD-XX'
e3 = root + 64
img[e3:e3+11] = b'GROW    TXT'
img[e3+11] = 0x20
img[e3+26:e3+28] = (5).to_bytes(2, 'little')
img[e3+28:e3+32] = (10).to_bytes(4, 'little')
img[99*sec:99*sec + len(grow_old)] = grow_old
p.write_bytes(img)
PY

LOG="$IL_LOGDIR/usbfs_fat32_write.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 14
il_send "cat /usb/fat/WRITE.TXT"
il_send "cat /usb/fat/GROW.TXT"
il_send_delay 2
il_send "echo T6-FAT32-WRITE-PERSIST > /usb/fat/WRITE.TXT"
il_send "echo T6-GROW-0123456789ABCDEFGHI > /usb/fat/GROW.TXT"
il_send_delay 2
il_send "cat /usb/fat/WRITE.TXT"
il_send "cat /usb/fat/GROW.TXT"
il_send_delay 2
il_send "echo gate-end"
il_send "exit"

# cache.writeback=off: the MSC layer sends no SYNCHRONIZE CACHE, so a
# writeback cache would swallow the persistence this gate exists to
# prove when QEMU is terminated at the end of the run.
il_run_qemu "$LOG" 60 \
    -device "piix3-usb-uhci,id=uhci" \
    -drive "file=$USB,format=raw,if=none,id=usbfat,cache.writeback=off" \
    -device "usb-storage,bus=uhci.0,drive=usbfat"

il_assert_grep "$LOG" "\\[usbfs\\] FAT32 detected" "usbfs detected FAT32"
il_assert_grep "$LOG" "OLD-CONTENT O" "WRITE.TXT's old content read pre-write"
il_assert_grep "$LOG" "GROW-OLD-X" "GROW.TXT's old content read pre-write"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived"

# The markers must appear TWICE each: once as the typed echo command
# and once as the post-write `cat` output — a read-back via READ(10).
for M in T6-FAT32-WRITE-PERSIST T6-GROW-0123456789ABCDEFGHI; do
    N=$(grep -c "$M" "$LOG" || true)
    if [ "${N:-0}" -ge 2 ]; then
        il_pass "the guest read '$M' back after the write"
    else
        il_fail "post-write cat did not show '$M' (count=$N)"
    fi
done

# Host-side persistence proof: parse the raw image the guest wrote.
python3 - "$USB" <<'PY'
import pathlib, sys
img = pathlib.Path(sys.argv[1]).read_bytes()
sec = 512
root = 96 * sec
def dirent(off):
    return img[root+off:root+off+11], \
           int.from_bytes(img[root+off+28:root+off+32], 'little')
# WRITE.TXT (root+32, cluster 4 @ LBA 98): in-place overwrite — the
# size stays 512 (no shrink), the data starts with the new marker and
# the untouched tail keeps the old bytes.
wname, wsize = dirent(32)
wdata = img[98*sec:98*sec + 512]
ok_w = (wname == b'WRITE   TXT' and wsize == 512 and
        wdata.startswith(b'T6-FAT32-WRITE-PERSIST') and
        wdata[32] == ord('O'))
# GROW.TXT (root+64, cluster 5 @ LBA 99): the write outgrew the old
# 10-byte size inside the cluster — the dirent size must now be the
# written length (marker + newline).
gname, gsize = dirent(64)
gdata = img[99*sec:99*sec + 64]
grow_marker = b'T6-GROW-0123456789ABCDEFGHI'
ok_g = (gname == b'GROW    TXT' and
        gsize == len(grow_marker) + 1 and
        gdata.startswith(grow_marker))
ok = ok_w and ok_g
print("HOST-VERIFY %s: write size=%u tail=%r grow size=%u" %
      ("OK" if ok else "FAIL", wsize, wdata[30:34], gsize))
sys.exit(0 if ok else 1)
PY
if [ $? -eq 0 ]; then
    il_pass "the write persisted on the media (host parsed the image)"
else
    il_fail "host-side image verification"
fi

il_assert_no_grep "$LOG" "Page Fault|kernel panic|\\[msc\\] FAIL" \
    "no usbfs FAT32 faults"
il_summary
