#!/usr/bin/env bash
# test_selfhost_mkiso.sh — SELFHOST_PLAN.md SH7d: in-guest MBR+GPT+FAT32 writer.
#
# SH7d is the largest "image tooling in C" twin: a /bin/mkiso that lays down
# the whole hybrid disk (MBR partition table, GPT header+array+backup, and a
# FAT32 ESP with the kernel/EFI/initrd files), replacing the host
# mformat/mcopy pair and the inline python3 BPB patch.  The host unit test
# (test_mkiso.c) parses the produced image and the same image boots on both
# SeaBIOS and OVMF; this case proves the stripped ELF runs on AuraLite and the
# in-guest shell can drive it end to end.
#
# The guest probe (sh7d_probe.sh) covers: the built-in geometry/CRC/BPB/FAT
# self check, assembling a real hybrid image from the staged boot blobs
# (/tests/mbr_dual.bin + /tests/stage2.bin) onto /fat, and a negative control
# (a sub-40 MiB ESP is refused).  Needs no guest toolchain: /bin/mkiso is a
# normal user ELF, so like SH7a/SH7b/SH7c this never skips on a plain `make iso`.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host image tooling (SH7d): in-guest mkiso MBR+GPT+FAT32 writer"

# The probe writes the ~48 MiB image to /fat, so unlike the SH7a–SH7c gates
# this one needs a real, writable /fat volume (the same shape the SH7e and
# SH8 gates use; il_run_qemu attaches only the boot ISO otherwise).  The
# guest's FAT32 driver formats a 4 MiB superfloppy by default, which cannot
# hold a >=40 MiB image, so pre-format the volume at its true size with the
# same BPB/FSInfo/FAT shape kernel/fs/fat32.c's parse_or_format accepts.
DISK="$IL_BUILD/selfhost-sh7d-fat.img"
rm -f "$DISK"
mkdir -p "$(dirname "$DISK")"
dd if=/dev/zero of="$DISK" bs=512 count=131000 status=none
python3 - "$DISK" <<'PY'
import sys, struct
path = sys.argv[1]
bs = 512
BASE = 64; T = 131000; RES = 32; NF = 2
fat = ((T - 1) // 128) + 1
clusters = T - (RES + NF * fat)
def w16(v): return struct.pack('<H', v)
def w32(v): return struct.pack('<I', v)
disk = bytearray((BASE + T) * bs)
mb = bytearray(bs)
mb[510] = 0x55; mb[511] = 0xAA
mb[446 + 0] = 0x80; mb[446 + 4] = 0x0C
struct.pack_into('<I', mb, 446 + 8, BASE)
struct.pack_into('<I', mb, 446 + 12, T)
disk[0:bs] = mb
def put(lba, data): disk[lba*bs:(lba+1)*bs] = data
bpb = bytearray(bs)
bpb[0] = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90
bpb[3:11] = b'AURALITE'
bpb[11:13] = w16(512); bpb[13] = 1
bpb[14:16] = w16(RES); bpb[16] = NF
bpb[21] = 0xF8; bpb[24:26] = w16(63); bpb[26:28] = w16(255)
bpb[28:32] = w32(BASE); bpb[32:36] = w32(T)
bpb[36:40] = w32(fat); bpb[44:48] = w32(2)
bpb[48:50] = w16(1); bpb[50:52] = w16(6)
bpb[64] = 0x80; bpb[66] = 0x29; bpb[67:71] = w32(0xA2026022)
bpb[71:82] = b'AURALITE   '; bpb[82:90] = b'FAT32   '
bpb[510] = 0x55; bpb[511] = 0xAA
put(BASE, bpb); put(BASE + 6, bpb)
fs = bytearray(bs)
fs[0:4] = w32(0x41615252); fs[484:488] = w32(0x61417272)
fs[488:492] = w32(clusters - 1); fs[492:496] = w32(3); fs[508:512] = w32(0xAA550000)
put(BASE + 1, fs); put(BASE + 7, fs)
for j in range(NF):
    fla = bytearray(bs)
    fla[0:4] = w32(0x0FFFFFF8); fla[4:8] = w32(0x0FFFFFFF); fla[8:12] = w32(0x0FFFFFFF)
    put(BASE + RES + j * fat, fla)
open(path, "wb").write(disk)
PY

AHCI=(
    -drive "file=$DISK,format=raw,if=none,id=sh7ddisk"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=sh7ddisk,bus=ahci0.0"
)

LOG="$IL_LOGDIR/selfhost_mkiso.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh7d_probe.sh"
# assembling a 48 MiB image in the TCG guest takes a while; give it room.
il_send_delay 20
il_send "echo SH7D_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 90 "${AHCI[@]}"

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] mkiso PASS: .* written in-guest" \
    "the probe ran to completion and printed the SH7d receipt"

# ---- S1: the built-in geometry/CRC/BPB/FAT self check passed in-guest ----
il_assert_grep "$LOG" "SH7d mkiso selftest checks passed" \
    "the tool's --selftest (CRC32 vector, FAT32 geometry, BPB, FSInfo) passed"

# ---- S2: a real hybrid image was assembled in-guest from the staged blobs ----
il_assert_grep "$LOG" "sh7d: s2-image-assembled" \
    "mkiso laid down MBR+GPT+FAT32 onto /fat in-guest"
il_assert_no_grep "$LOG" "UNREACHABLE-ASSEMBLY" \
    "the in-guest image assembly did not fail"

# ---- S3: the negative control fired (a sub-floor ESP is refused) ----
il_assert_grep "$LOG" "sh7d: s3-small-esp-rejected" \
    "a sub-40 MiB ESP (<65525 clusters) is rejected rather than shipped"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no probe branch that should be unreachable was taken"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH7D_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
