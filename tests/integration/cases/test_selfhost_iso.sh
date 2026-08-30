#!/usr/bin/env bash
# test_selfhost_iso.sh — SELFHOST_PLAN.md SH7e: guest-built ISO, terminal gate.
#
# SH7e is the first end-to-end proof of Stage 1: `sh build.sh iso` runs the
# SH7a–SH7d twins in order and assembles /fat/auralite.iso in-guest from the
# prebuilt /fat artefacts, with no host compiler in the loop.  Nothing is
# compiled here — the full in-guest kernel/libc/userland compile is SH8.
#
# The guest probe (sh7e_probe.sh) stages the /fat worktree, runs the four
# twins in order (sha256sum --selftest, mkinitrd --selftest, bootoffsets
# --check, mkiso --selftest) and then drives `sh /fat/build.sh iso`.  It needs
# no guest toolchain: the twins are normal user ELFs, so like the SH7a–SH7d
# cases this never skips on a plain `make iso`.
#
# /fat must be a real, writable filesystem, so we attach a persistent AHCI
# disk.  The guest's FAT32 driver formats a 4 MiB superfloppy by default, which
# cannot hold the >=40 MiB image the SH7d writer requires, so we pre-format the
# volume at its true size on the host (the same BPB/FSInfo/FAT shape
# kernel/fs/fat32.c's format_default writes, but with the real sector count).
# The guest's parse_or_format accepts it because it only checks the boot
# signature, the FAT32 string and the size fields.
#
# The plan's terminal gate also has the host *boot* the guest-built ISO and
# grep the standard boot receipts.  That half additionally needs SH5's
# pre-built, bootable /fat/KERNEL.ELF (the guest-compiled kernel), which is not
# produced on a tree that never ran the SH5 guest-tcc gate.  This case asserts
# the tool-chain-free, durable artefacts (the §8 receipt and the SH7a–SH7d
# chain); booting the guest ISO to a shell is exercised once SH5's kernel is on
# /fat.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host terminal gate (SH7e): assemble the guest ISO in-guest"

DISK="$IL_BUILD/selfhost-sh7e-fat.img"
rm -f "$DISK"
mkdir -p "$(dirname "$DISK")"

# ---- a large, pre-formatted FAT32 /fat volume (guest-visible) ----
# 64 MiB raw disk, FAT32 volume at LBA 64.  The guest mounts the volume at the
# size declared in its BPB (see fat32.c parse_or_format), so /fat holds the
# ~48 MiB image build.sh iso writes.
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
# MBR partition table (partition 1 at LBA 64, type 0x0C FAT32-LBA)
mb = bytearray(bs)
mb[510] = 0x55; mb[511] = 0xAA
mb[446 + 0] = 0x80; mb[446 + 4] = 0x0C
struct.pack_into('<I', mb, 446 + 8, BASE)
struct.pack_into('<I', mb, 446 + 12, T)
disk[0:bs] = mb
def put(lba, data): disk[lba*bs:(lba+1)*bs] = data
# BPB (matches format_default's fields; real size)
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
    -drive "file=$DISK,format=raw,if=none,id=sh7edisk"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=sh7edisk,bus=ahci0.0"
)

LOG="$IL_LOGDIR/selfhost_iso.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh7e_probe.sh"
# assembling a 48 MiB image + packing the initrd payload takes a while.
il_send_delay 24
il_send "echo SH7E_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 100 "${AHCI[@]}"

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\\[selfhost\\] iso PASS: auralite.iso built in-guest" \
    "the SH7a–SH7d chain ran to completion and printed the SH7e receipt"

# ---- the twins ran, in SH7a–SH7d order ----
il_assert_grep "$LOG" "\\[selfhost\\] sh7e: twins-in-order" \
    "the four twins ran in order"
il_assert_grep "$LOG" "\\[fat32\\] mounted FAT32 at /fat" \
    "the pre-formatted /fat volume mounted"

# ---- each twin's own self-check fired ----
il_assert_grep "$LOG" "\\[selfhost\\] sha256 SELFTEST OK" \
    "SH7a sha256sum passed its FIPS vectors"
il_assert_grep "$LOG" "\\[selfhost\\] mkinitrd round-trip OK" \
    "SH7b mkinitrd packed and re-parsed its archive"
il_assert_grep "$LOG" "\\[selfhost\\] boot-offset header PASS:" \
    "SH7c bootoffsets reported the guest layout"
il_assert_grep "$LOG" "mkiso selftest checks passed" \
    "SH7d mkiso passed its geometry/CRC/BPB/FAT selftest"

# ---- the production recipe actually assembled the image ----
il_assert_grep "$LOG" "\\[selfhost\\] mkiso PASS: .*written in-guest" \
    "build.sh iso ran mkiso and wrote /fat/auralite.iso in-guest"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no probe branch that should be unreachable was taken"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH7E_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
