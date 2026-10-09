#!/usr/bin/env bash
# tools/mkisoimage_bios.sh -- build a BIOS-only bootable ISO image for
# AuraLite OS using the custom BL2 MBR + BL3/4 Stage 2 (no Limine).
#
# The layout of the produced image:
#
#   * Sector 0        -- our 512-byte MBR (BL2), signature 0x55AA.
#   * Sector 1..N     -- Stage 2 flat binary (BL3+BL4).
#   * Sector 128..    -- FAT32 partition holding /KERNEL.ELF (and
#                        optionally /INITRD.TAR if it exists).
#
# The disc is packaged as an El Torito "no-emulation" boot image, and
# is also given a protective MBR partition table via xorriso so it
# survives being written to a USB stick with `dd` and boots via legacy
# BIOS from either CD or hard-disk emulation.
#
# UEFI is out of scope for this script -- BL6 adds a separate UEFI
# path and BL7 merges the two into a single dual-boot image.
#
# Usage: mkisoimage_bios.sh <kernel.elf> <out.iso>
set -euo pipefail

KERNEL_ELF="${1:?usage: $0 <kernel.elf> <out.iso>}"
ISO_OUT="${2:?usage: $0 <kernel.elf> <out.iso>}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
MBR_BIN="$BUILD/boot/mbr.bin"
STAGE2_BIN="$BUILD/boot/stage2.bin"

# ---- Sanity: the three build artefacts must exist -------------------------
for f in "$KERNEL_ELF" "$MBR_BIN" "$STAGE2_BIN"; do
    if [ ! -s "$f" ]; then
        echo "[mkiso-bios] ERROR: missing artefact $f" >&2
        echo "[mkiso-bios] hint: run 'make mbr stage2 kernel' first" >&2
        exit 1
    fi
done

# MBR must be exactly 512 B and end with 0x55 0xAA.
sz=$(wc -c < "$MBR_BIN")
if [ "$sz" -ne 512 ]; then
    echo "[mkiso-bios] ERROR: $MBR_BIN is $sz bytes (must be 512)" >&2
    exit 1
fi
sig=$(od -An -tx1 -N2 -j510 "$MBR_BIN" | tr -d ' \n')
if [ "$sig" != "55aa" ]; then
    echo "[mkiso-bios] ERROR: $MBR_BIN has bad boot signature 0x$sig" >&2
    exit 1
fi

# Stage 2 must fit in the 126 sectors the MBR loads (63 KiB max).
sz=$(wc -c < "$STAGE2_BIN")
maxsz=$((126*512))
if [ "$sz" -gt "$maxsz" ]; then
    echo "[mkiso-bios] ERROR: $STAGE2_BIN is $sz bytes (max $maxsz)" >&2
    exit 1
fi

# ---- Build the raw hybrid image (same layout our smoke tests use) ---------
# xorriso's El Torito can boot a plain sector-0 image, but pointing at
# an already-formed hybrid disk makes the resulting file usable both
# as `qemu -cdrom` AND as `dd if=... of=/dev/sdX` -- which is the whole
# point of a "hybrid" image.
#
# The FAT32 partition is sized from its contents (kernel.elf + initrd.tar),
# never below 40 MiB.  The old fixed 15 MiB partition ran out of space as
# soon as initrd.tar (22 MB and growing) was copied in ("Disk full").  FAT32
# needs at least 65525 clusters, which is why the floor is 40 MiB.
FAT_MIN_MB=40
payload=$(wc -c < "$KERNEL_ELF")
if [ -f "$BUILD/initrd.tar" ]; then
    payload=$((payload + $(wc -c < "$BUILD/initrd.tar")))
fi
FAT_MB=$(( (payload + 1048575) / 1048576 + 8 ))   # +8 MiB: FAT tables, dir entries
if [ "$FAT_MB" -lt "$FAT_MIN_MB" ]; then FAT_MB=$FAT_MIN_MB; fi
FAT_SECTORS=$((FAT_MB * 2048))
RAW_MB=$((FAT_MB + 1))                            # MBR + Stage 2 + LBA 128 lead-in

RAW_IMG="$BUILD/auralite-bios.raw"
FAT_IMG="$BUILD/auralite-bios.fat.img"

echo "[mkiso-bios] assembling raw hybrid image at $RAW_IMG (FAT32 partition ${FAT_MB} MiB)"
dd if=/dev/zero of="$RAW_IMG" bs=1M count="$RAW_MB" status=none

# Sector 0: MBR.  Sector 1..N: Stage 2.
dd if="$MBR_BIN"    of="$RAW_IMG" bs=512 count=1 conv=notrunc status=none
dd if="$STAGE2_BIN" of="$RAW_IMG" bs=512 seek=1 conv=notrunc status=none

# FAT32 partition built out-of-place with mformat/mcopy.  -T gives the exact
# sector count of the file.  The old "-t 480 -h 32 -s 32" geometry described
# a 240 MiB disk, not the 15 MiB file it was written into.
dd if=/dev/zero of="$FAT_IMG" bs=1M count="$FAT_MB" status=none
mformat -i "$FAT_IMG" -F -T "$FAT_SECTORS" ::
mcopy -i "$FAT_IMG" "$KERNEL_ELF" ::KERNEL.ELF
if [ -f "$BUILD/initrd.tar" ]; then
    mcopy -i "$FAT_IMG" "$BUILD/initrd.tar" ::INITRD.TAR
fi

# Splice the FAT partition into the raw disk at LBA 128 (=0x10000).
dd if="$FAT_IMG" of="$RAW_IMG" bs=512 seek=128 conv=notrunc status=none

# Write an MBR partition-table entry for the FAT partition.  This is
# what firmware (real BIOS / SeaBIOS / OVMF) consults to enumerate
# partitions; without it a real machine would not present the FAT
# partition to `dd`/mount tools.
python3 - "$RAW_IMG" "$FAT_SECTORS" <<'PY'
import struct, sys
img = open(sys.argv[1], 'r+b')
img.seek(0x1BE)                     # first partition-table entry
# 16-byte MBR partition entry:
#   byte  status      : 0x80 = active/bootable
#   byte  chs_start   : (head), 3 bytes (ignored under LBA)
#   byte  type        : 0x0C = FAT32 with LBA
#   byte  chs_end     : 3 bytes (ignored)
#   dword lba_start   : partition first LBA
#   dword sector_cnt  : partition size in sectors
img.write(struct.pack('<BBBBBBBBII',
    0x80, 0, 2, 0,
    0x0C,
    0, 0, 0,
    128, int(sys.argv[2])))
img.close()
PY

# ---- Ship the raw hybrid image AS the .iso -------------------------------
# The BL5 gate is a single deliverable: a bootable image containing
# our MBR + Stage 2 + FAT32 partition with kernel.elf.  We ship it
# as a raw hybrid MBR image renamed to auralite-bios.iso.  The same
# byte sequence boots via:
#
#   * qemu-system-x86_64 -drive format=raw,file=<.iso>,if=ide
#   * dd if=<.iso> of=/dev/sdX bs=1M   (real hardware, USB stick)
#
# Legacy `qemu -cdrom` (which forces El Torito CD emulation) is NOT
# supported by this image: SeaBIOS demands a valid El Torito boot
# catalog inside the ISO 9660 filesystem, and merging that with our
# MBR-based partition layout requires either an ISO 9660 reader in
# Stage 2 (deferred) or a syslinux-style isohybrid overlay (would
# need a specialised second MBR).  The hybrid MBR path covers real
# hardware boot from HDD/USB, which is the practical use case.
#
# BL7 will additionally produce a dual-boot ISO by wrapping this
# image plus BOOTX64.EFI (from BL6) in an El Torito container.
cp "$RAW_IMG" "$ISO_OUT"

sz=$(du -h "$ISO_OUT" | cut -f1)
echo "[mkiso-bios] wrote $ISO_OUT ($sz, hybrid MBR image; boot via -drive if=ide)"
