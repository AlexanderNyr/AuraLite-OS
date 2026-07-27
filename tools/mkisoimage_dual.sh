#!/usr/bin/env bash
# tools/mkisoimage_dual.sh -- assemble the BL7 dual-boot ISO.
#
# The output is a single hybrid MBR disk image that boots both:
#
#   * Legacy BIOS  -> sector 0 MBR (BL2) -> LBA 1..N Stage 2 (BL3+BL4)
#                     -> reads KERNEL.ELF from the FAT32 partition at
#                        LBA 128 (8.3 root entry).
#   * UEFI         -> firmware enumerates the FAT32 partition, spots
#                     the /EFI/BOOT/BOOTX64.EFI fallback path, runs
#                     it, and the EFI application reads KERNEL.ELF
#                     from /EFI/BOOT/KERNEL.ELF via SimpleFileSystem.
#
# Because both loaders open the FAT32 partition through completely
# different code paths, KERNEL.ELF is placed at TWO locations:
#     /KERNEL.ELF            (8.3 root entry, read by BIOS Stage 2)
#     /EFI/BOOT/KERNEL.ELF   (read by BOOTX64.EFI via SFS)
# The two entries reference the same file bytes but FAT does not
# implement hard links inside mformat, so we mcopy it twice.
# The overhead (one extra copy of ~1.7 MiB kernel) is negligible for
# a 32 MiB image.
#
# Partition type 0x0C (FAT32-LBA) is used instead of 0xEF (EFI System
# Partition) so the same partition doubles as a legacy boot partition
# for the BIOS Stage 2.  UEFI firmwares happily follow the fallback
# path on any FAT32 partition when no ESP-typed partition exists.
#
# Usage: mkisoimage_dual.sh <kernel.elf> <BOOTX64.EFI> <out.iso>
set -euo pipefail

KERNEL_ELF="${1:?usage: $0 <kernel.elf> <BOOTX64.EFI> <out.iso>}"
EFI_APP="${2:?usage: $0 <kernel.elf> <BOOTX64.EFI> <out.iso>}"
ISO_OUT="${3:?usage: $0 <kernel.elf> <BOOTX64.EFI> <out.iso>}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"

# ---- ESP sizing -----------------------------------------------------------
# Size of the FAT32 partition in MiB.  Override with e.g.
#     make iso ESP_MB=256          (or ESP_MB=48 tools/mkisoimage_dual.sh ...)
#
# Lower bound: a genuine FAT32 volume needs >= 65525 data clusters, otherwise
# Microsoft's spec (and OVMF's strict FatPkg driver) classify it as FAT16 and
# refuse to mount it.  We format with -c 1 (1 sector = 512 B per cluster), so
# the floor is 65525 * 512 = ~32 MiB of *data* plus FAT/reserved overhead.
# 48 MiB clears that comfortably (~98k clusters) while leaving ~36 MiB free
# for a kernel + initrd that currently total ~12 MiB (each stored twice).
ESP_MB="${ESP_MB:-48}"

if [ "$ESP_MB" -lt 40 ]; then
    echo "[mkiso-dual] ERROR: ESP_MB=$ESP_MB is below the 40 MiB FAT32 floor" >&2
    echo "[mkiso-dual] hint: <65525 clusters makes OVMF reject the volume as FAT16" >&2
    exit 1
fi
# mformat geometry: heads(32) * sectors(32) * tracks = total sectors.
# tracks = ESP_MB * 1024 * 1024 / 512 / (32 * 32) = ESP_MB * 2
ESP_TRACKS=$(( ESP_MB * 2 ))
ESP_SECTORS=$(( ESP_MB * 2048 ))
MBR_BIN="$BUILD/boot/mbr_dual.bin"    # dual-boot variant reads Stage 2 from LBA 34
STAGE2_BIN="$BUILD/boot/stage2.bin"

# ---- Sanity ---------------------------------------------------------------
for f in "$KERNEL_ELF" "$EFI_APP" "$MBR_BIN" "$STAGE2_BIN"; do
    if [ ! -s "$f" ]; then
        echo "[mkiso-dual] ERROR: missing artefact $f" >&2
        echo "[mkiso-dual] hint: run 'make mbr stage2 efi kernel' first" >&2
        exit 1
    fi
done

# MBR must be 512 B with 0x55AA signature.
sz=$(wc -c < "$MBR_BIN")
[ "$sz" -eq 512 ] || { echo "[mkiso-dual] ERROR: MBR is $sz B (must be 512)" >&2; exit 1; }
sig=$(od -An -tx1 -N2 -j510 "$MBR_BIN" | tr -d ' \n')
[ "$sig" = "55aa" ] || { echo "[mkiso-dual] ERROR: bad MBR signature 0x$sig" >&2; exit 1; }

# Stage 2 must fit inside the 126 sectors the MBR loads.
sz=$(wc -c < "$STAGE2_BIN")
maxsz=$((126*512))
[ "$sz" -le "$maxsz" ] || { echo "[mkiso-dual] ERROR: Stage 2 is $sz B (max $maxsz)" >&2; exit 1; }

# ---- Build a raw hybrid image (32 MiB) -----------------------------------
RAW_IMG="$BUILD/auralite-dual.raw"
FAT_IMG="$BUILD/auralite-dual.fat.img"

# Image sizing: the FAT32 partition must hold at least 65525 data
# clusters or Microsoft's own FAT32 spec (and OVMF's strict FS
# driver) rejects it as "actually FAT16".  With 1-sector clusters
# that means >= 65525 * 512 = ~32 MiB of DATA, so we round up to a
# ESP_MB FAT32 partition.  Plus 1 MiB of MBR + Stage 2 + GPT reserved
# area at the front = (ESP_MB + 1) MiB total image.
echo "[mkiso-dual] assembling raw hybrid image at $RAW_IMG (ESP ${ESP_MB} MiB)"
dd if=/dev/zero of="$RAW_IMG" bs=1M count=$(( ESP_MB + 1 )) status=none

# Final disk layout (LBA = 512-byte sector):
#
#   LBA 0            MBR (mbr_dual variant that reads Stage 2 from LBA 34)
#   LBA 1            GPT primary header
#   LBA 2..33        GPT primary partition array
#   LBA 34..159      Stage 2 flat binary (126 sectors max = 63 KiB)
#   LBA 160..255     free
#   LBA 256..N-33    ESP FAT32 partition (ESP_MB MiB, holds kernel.elf,
#                    BOOTX64.EFI, optional initrd.tar)
#   LBA N-33..N-2    GPT backup partition array
#   LBA N-1          GPT backup header
#
# The 34..159 slot for Stage 2 sits inside the "first usable LBA"
# window that GPT reserves after the primary header + array; it is
# not covered by any GPT partition entry, so GPT parsers ignore it
# while INT 13h AH=42h can still read it.  BIOS Stage 2's fat_init
# is hard-coded to LBA 256 -- see stage2_start.asm's `mov eax, 256`
# argument to fat_init.
dd if="$MBR_BIN"    of="$RAW_IMG" bs=512 count=1  conv=notrunc status=none
dd if="$STAGE2_BIN" of="$RAW_IMG" bs=512 seek=34  conv=notrunc status=none

# Format the FAT32 partition (31 MiB) with kernel.elf in TWO locations
# so both BIOS Stage 2 (root entry) and UEFI (SFS path) find it.
#
# mformat quirks (see mtools(1) and mformat(1)):
#   -F                       force FAT32 (mformat defaults to FAT16
#                            below ~64 MiB and OVMF's FS driver only
#                            enumerates FAT32 partitions cleanly).
#   -h 32 -s 32 -t 992       CHS geometry matching a 31 MiB volume
#                            (32 * 32 * 992 * 512 = 31.75 MiB).
#   -c 4                     4 sectors/cluster (2 KiB clusters): more
#                            robust with OVMF than mformat's default 1.
#   -N 0x12345678            filesystem serial number (non-zero).
#   -v AURALITE              volume label.
#   -H 128                   hidden-sector count = the LBA at which
#                            the partition starts on the parent disk.
#                            Required by the FAT spec (BPB_HiddSec) and
#                            by OVMF's FAT driver validation.
# ESP_MB FAT32 partition.  Geometry -h 32 -s 32 -t $ESP_TRACKS gives
# exactly ESP_MB MiB, matching the backing file created below.
# Cluster size 1 sector (512 B) keeps the cluster count above the
# 65525-cluster floor Microsoft mandates for genuine FAT32 for any
# ESP_MB >= ~33; the guard above enforces 40 MiB minimum.
dd if=/dev/zero of="$FAT_IMG" bs=1M count=$ESP_MB status=none
mformat -i "$FAT_IMG" -F -h 32 -s 32 -t $ESP_TRACKS -c 1 \
        -N 0x12345678 -v AURALITE -H 256 ::

# mformat writes BPB_TotSec16 = 63488 and leaves BPB_TotSec32 = 0.
# The FAT32 spec (Microsoft Extensible Firmware Initiative FAT32
# File System Spec 1.03 s.3.5) requires BPB_TotSec16 == 0 for FAT32
# so that TotSec32 is the sole source of truth.  OVMF's FatPkg
# validates this strictly and refuses to mount the volume otherwise.
# Patch the BPB in-place: zero TotSec16, mirror the count into
# TotSec32.
python3 - "$FAT_IMG" <<'PY'
import struct, sys
with open(sys.argv[1], 'r+b') as f:
    # Patch primary boot sector (LBA 0).
    def fix(base):
        f.seek(base + 19); tot16 = struct.unpack('<H', f.read(2))[0]
        if tot16 != 0:
            f.seek(base + 19); f.write(struct.pack('<H', 0))
            f.seek(base + 32); f.write(struct.pack('<I', tot16))
    fix(0)
    # Read BPB_BkBootSec (offset 50, u16) from the primary and mirror
    # the fix into the backup boot sector too.
    f.seek(50); bk = struct.unpack('<H', f.read(2))[0]
    if bk:
        fix(bk * 512)
PY
mmd    -i "$FAT_IMG" ::/EFI
mmd    -i "$FAT_IMG" ::/EFI/BOOT
mcopy  -i "$FAT_IMG" "$EFI_APP"    ::/EFI/BOOT/BOOTX64.EFI
mcopy  -i "$FAT_IMG" "$KERNEL_ELF" ::/EFI/BOOT/KERNEL.ELF
mcopy  -i "$FAT_IMG" "$KERNEL_ELF" ::/KERNEL.ELF
if [ -f "$BUILD/initrd.tar" ]; then
    # BIOS Stage 2 loads the archive at 24 MiB inside its fixed 0..32 MiB
    # early-boot reservation, leaving an 8 MiB slot.  Fail the build rather
    # than shipping an image whose BIOS path silently omits userspace.
    initrd_size=$(wc -c < "$BUILD/initrd.tar")
    if [ "$initrd_size" -gt $((8 * 1024 * 1024)) ]; then
        echo "[mkiso-dual] ERROR: initrd.tar is $initrd_size bytes (BIOS loader max: 8 MiB)" >&2
        exit 1
    fi
    mcopy -i "$FAT_IMG" "$BUILD/initrd.tar" ::/EFI/BOOT/INITRD.TAR
    mcopy -i "$FAT_IMG" "$BUILD/initrd.tar" ::/INITRD.TAR
fi

# Splice the FAT partition into the raw disk at LBA 128 (=0x10000).
dd if="$FAT_IMG" of="$RAW_IMG" bs=512 seek=256 conv=notrunc status=none

# Write the MBR partition-table entry for the FAT partition.  We use
# type 0xEF (EFI System Partition) so UEFI firmwares recognise it as
# an ESP and follow the /EFI/BOOT/BOOTX64.EFI fallback boot path.
# Our BIOS Stage 2 does NOT consult the MBR partition table -- it
# reads FAT sectors starting at a hard-coded LBA 128 -- so the type
# byte is irrelevant on that path.  Any bootable partition type that
# UEFI accepts as an ESP would work; 0xEF is the canonical choice.
python3 - "$RAW_IMG" "$ESP_SECTORS" <<'PY'
"""
Install a hybrid MBR + GPT layout on the raw disk image so that:

  * Legacy BIOS reads sector 0, sees our BL2 MBR (unchanged from the
    BIOS-only path) with a bootable partition of type 0x0C for the
    FAT32 slot at LBA 128.  BIOS Stage 2 keeps working exactly as
    it did on BL5.
  * UEFI firmware reads sector 1 onwards, sees a valid GPT header
    with signature "EFI PART" that points at a partition array
    describing the FAT32 partition as an EFI System Partition
    (type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B).  Firmware
    then enumerates the FAT32 filesystem inside that partition and
    follows the /EFI/BOOT/BOOTX64.EFI fallback path.

The layout on disk (LBA 512-byte sectors):

  LBA 0            Protective/hybrid MBR (already contains BL2 code
                   + a partition table entry we wrote earlier)
  LBA 1            GPT header (this script writes)
  LBA 2..33        GPT partition array (32 sectors, 128 entries * 128 B)
  LBA 34..127      free
  LBA 128..N-33    ESP (FAT32 partition, already spliced)
  LBA N-33..N-2    Backup GPT partition array
  LBA N-1          Backup GPT header

Where N = image size in sectors.
"""
import struct, sys, zlib, uuid, os

path = sys.argv[1]
disk_size = os.path.getsize(path)
disk_sectors = disk_size // 512

esp_lba_start = 256                                       # past Stage 2 slot (LBA 34..159)
esp_lba_count = int(sys.argv[2])                          # ESP size in sectors
esp_lba_end   = esp_lba_start + esp_lba_count - 1

# --- Rewrite the MBR (0x1B8 disk sig + partition entry). --------------------
# This mirrors mkisoimage_bios.sh but with type 0x0C (FAT32-LBA) so
# BIOS Stage 2 stays happy.  UEFI ignores the MBR when a valid GPT is
# present, so the type byte is irrelevant on the UEFI path.
with open(path, 'r+b') as img:
    img.seek(0x1B8)
    img.write(struct.pack('<I', 0xAA55DEAD))

    # Hybrid MBR layout: the first entry is a bootable FAT32-LBA
    # partition (0x0C) pointing at the ESP so SeaBIOS's boot-order
    # scan picks it and runs our BL2 MBR code.  Some BIOSes (SeaBIOS
    # among them) will REFUSE to boot the disk when entry 1 is
    # type 0xEE (they treat the disk as "GPT-only, hand off to UEFI").
    # The GPT protective entry therefore lives in slot 2.  UEFI does
    # not care about the MBR entry order once it has recognised the
    # 0xEE marker anywhere in the four slots.
    img.seek(0x1BE)
    # Slot 1: legacy bootable ESP -- for BIOS Stage 1.
    img.write(struct.pack('<BBBBBBBBII',
        0x80,               # bootable
        0xFE, 0xFF, 0xFF,
        0x0C,               # FAT32-LBA
        0xFE, 0xFF, 0xFF,
        esp_lba_start,
        min(esp_lba_count, 0xFFFFFFFF)))
    # Slot 2: GPT protective marker.  Must exist for UEFI firmware
    # to acknowledge the GPT header at LBA 1.  Size field is capped
    # to 2^32 - 1 sectors (32-bit sector_count) which is fine for any
    # realistic disk.
    img.write(struct.pack('<BBBBBBBBII',
        0x00,               # not bootable
        0xFE, 0xFF, 0xFF,
        0xEE,               # partition type: GPT protective
        0xFE, 0xFF, 0xFF,
        1,                  # LBA_start = 1 (GPT header)
        min(disk_sectors - 1, 0xFFFFFFFF)))

    # ---- Build the GPT partition array (LBA 2..33). --------------------
    # UEFI 2.10 spec s.5.3.  128 * 128-byte entries = 16 KiB.
    ESP_TYPE_GUID = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
    part_guid     = uuid.uuid4()
    disk_guid     = uuid.uuid4()

    def guid_to_le(u):
        # UEFI stores GUIDs with the first three fields in little-
        # endian byte order and the last two in "network" order.
        b = u.bytes
        return (b[3::-1] + b[5:3:-1] + b[7:5:-1] + b[8:])

    entry = bytearray(128)
    entry[  0:16] = guid_to_le(ESP_TYPE_GUID)      # PartitionTypeGUID
    entry[ 16:32] = guid_to_le(part_guid)          # UniquePartitionGUID
    entry[ 32:40] = struct.pack('<Q', esp_lba_start)
    entry[ 40:48] = struct.pack('<Q', esp_lba_end)
    entry[ 48:56] = struct.pack('<Q', 0)           # Attributes
    name_utf16    = 'AURALITE ESP'.encode('utf-16-le')
    entry[ 56:56+len(name_utf16)] = name_utf16     # PartitionName

    part_array = bytes(entry) + b'\x00' * (128 * 127)
    part_array_crc = zlib.crc32(part_array) & 0xFFFFFFFF

    # ---- Primary GPT header at LBA 1. ---------------------------------
    hdr = bytearray(92)
    hdr[  0: 8] = b'EFI PART'
    hdr[  8:12] = struct.pack('<I', 0x00010000)    # Revision 1.0
    hdr[ 12:16] = struct.pack('<I', 92)            # HeaderSize
    # CRC32 of header (bytes 0..91 with the CRC field zeroed) --
    # patch in a second pass.
    hdr[ 16:20] = struct.pack('<I', 0)             # HeaderCRC placeholder
    hdr[ 20:24] = struct.pack('<I', 0)             # Reserved
    hdr[ 24:32] = struct.pack('<Q', 1)             # MyLBA
    hdr[ 32:40] = struct.pack('<Q', disk_sectors - 1)  # AlternateLBA
    hdr[ 40:48] = struct.pack('<Q', 34)                # FirstUsableLBA
    hdr[ 48:56] = struct.pack('<Q', disk_sectors - 34) # LastUsableLBA
    hdr[ 56:72] = guid_to_le(disk_guid)
    hdr[ 72:80] = struct.pack('<Q', 2)             # PartitionEntryLBA
    hdr[ 80:84] = struct.pack('<I', 128)           # NumberOfPartitionEntries
    hdr[ 84:88] = struct.pack('<I', 128)           # SizeOfPartitionEntry
    hdr[ 88:92] = struct.pack('<I', part_array_crc)

    # Patch header CRC.
    hdr[16:20] = struct.pack('<I', zlib.crc32(bytes(hdr)) & 0xFFFFFFFF)

    img.seek(1 * 512)
    img.write(bytes(hdr) + b'\x00' * (512 - 92))   # LBA 1
    img.seek(2 * 512)
    img.write(part_array)                          # LBA 2..33

    # ---- Backup GPT (mirror of the primary). --------------------------
    # Header at the LAST sector; partition array in the 32 sectors
    # before it.  UEFI requires the backup for spec compliance.
    backup_arr_lba = disk_sectors - 33
    backup_hdr = bytearray(hdr)
    backup_hdr[24:32] = struct.pack('<Q', disk_sectors - 1)     # MyLBA
    backup_hdr[32:40] = struct.pack('<Q', 1)                    # AlternateLBA
    backup_hdr[72:80] = struct.pack('<Q', backup_arr_lba)       # PartitionEntryLBA
    backup_hdr[16:20] = struct.pack('<I', 0)
    backup_hdr[16:20] = struct.pack('<I', zlib.crc32(bytes(backup_hdr)) & 0xFFFFFFFF)

    img.seek(backup_arr_lba * 512)
    img.write(part_array)
    img.seek((disk_sectors - 1) * 512)
    img.write(bytes(backup_hdr) + b'\x00' * (512 - 92))
PY

# The final .iso is just the raw image.  On modern hardware and
# under QEMU this boots via:
#     qemu -drive format=raw,file=<iso>,if=ide       (BIOS)
#     qemu -bios OVMF_CODE.fd -drive fat:rw:/EFI/... (UEFI, live tree)
# or via `dd if=<iso> of=/dev/sdX` for a bootable USB stick.
cp "$RAW_IMG" "$ISO_OUT"

sz=$(du -h "$ISO_OUT" | cut -f1)
echo "[mkiso-dual] wrote $ISO_OUT ($sz, dual-boot BIOS+UEFI hybrid image)"
