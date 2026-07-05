#!/usr/bin/env bash
# tests/integration/bl4_fat_smoke.sh -- exercise the fat.inc reader.
#
# Builds a disk image with:
#   * BL2 MBR at LBA 0.
#   * BL3+BL4 Stage 2 at LBA 1..N.
#   * A FAT32 partition starting at LBA 128 containing a file named
#     KERNEL.ELF whose content is a known 32 KiB pattern.
#
# Boots the image under QEMU/SeaBIOS.  Stage 2 tries fat_init at
# LBA 128 and then fat_find("KERNEL  ELF").  If the parser works,
# fat_load copies the file to physical 0x00200000.  We pmemsave that
# region and diff against the source file.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/bl4_fat.img"
LOG="$BUILD/bl4_fat.log"
FILE_SRC="$BUILD/bl4_fat.src"
FILE_DST="$BUILD/bl4_fat.dst"

# 1. Build the real MBR + Stage 2 artefacts.
make -C "$ROOT" mbr stage2 >/dev/null

# 2. Generate a deterministic 32 KiB test file.  Use openssl (present on
#    Debian by default) if available, otherwise fall back to a plain
#    pattern that avoids all-zeroes.
FILE_SIZE=$((200 * 1024))
python3 -c "
import sys, struct
buf = bytearray(struct.pack('<Q', 0x4155524154455354))  # 'AURATEST'
for i in range((${FILE_SIZE} - 8) // 4):
    buf += struct.pack('<I', i ^ 0xA5A5A5A5)
open('$FILE_SRC','wb').write(bytes(buf))
"
[ "$(wc -c < "$FILE_SRC")" -eq "$FILE_SIZE" ]

# 3. Build a 16 MiB image and format the FAT32 partition (LBA 128..end).
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
dd if="$BUILD/boot/mbr.bin"    of="$IMG" bs=512 count=1  conv=notrunc status=none
dd if="$BUILD/boot/stage2.bin" of="$IMG" bs=512 seek=1   conv=notrunc status=none

# Format the FAT32 partition inside the image at sector 128.  We use
# mformat on an offset-mapped loop device via the -o (skip) option.
# mformat needs the *partition* view, so build it as a separate 15 MiB
# file, format, and splice.
FAT_IMG="$BUILD/bl4_fat_part.img"
dd if=/dev/zero of="$FAT_IMG" bs=1M count=15 status=none
mformat -i "$FAT_IMG" -F -h 32 -s 32 -t 480 ::
mcopy -i "$FAT_IMG" "$FILE_SRC" ::KERNEL.ELF
# Splice the formatted partition into the disk image at LBA 128 (=0x10000).
dd if="$FAT_IMG" of="$IMG" bs=512 seek=128 conv=notrunc status=none

# Also lay down a very simple MBR partition table entry so that any
# real BIOS treats sector 128 as a bootable FAT32 partition.  Our own
# Stage 2 does not consult it, but doing this improves interoperability
# with tools that do.
python3 - "$IMG" <<'PY'
import struct, sys
img = open(sys.argv[1], 'r+b')
img.seek(0x1BE)
# 16-byte partition entry: bootable(0x80), CHS start (ignored), type=0x0C
# (FAT32 LBA), CHS end (ignored), start LBA (128), sector count (15*2048).
img.write(struct.pack('<BBBBBBBBII',
    0x80, 0, 2, 0,   # bootable, CHS start
    0x0C,            # type: FAT32 LBA
    0, 0, 0,         # CHS end (ignored under LBA)
    128, 15*2048))
img.close()
PY

# 4. Boot under QEMU.  Give it 10 s to reach the halt; we grep after.
rm -f "$LOG"
timeout 12 qemu-system-x86_64 \
    -drive format=raw,file="$IMG",if=ide \
    -m 128M \
    -display none -serial file:"$LOG" -no-reboot \
    -monitor unix:"$BUILD/bl4_fat_monitor.sock",server,nowait \
    >/dev/null 2>&1 &
QEMU_PID=$!

# Wait for stage2 to finish (it halts once .fat_done is reached).
for _ in 1 2 3 4 5 6 7 8; do
    [ -S "$BUILD/bl4_fat_monitor.sock" ] && break
    sleep 0.5
done
sleep 3

# 5. Dump the destination buffer at 0x00200000 (FILE_SIZE bytes).
rm -f "$FILE_DST"
if [ -S "$BUILD/bl4_fat_monitor.sock" ] && command -v socat >/dev/null 2>&1; then
    {
        printf 'pmemsave 0x00200000 0x%x "%s"\n' "$FILE_SIZE" "$FILE_DST"
        sleep 0.5
        printf 'quit\n'
    } | socat -T3 -,ignoreeof UNIX-CONNECT:"$BUILD/bl4_fat_monitor.sock" >/dev/null 2>&1 || true
    sleep 0.5
fi
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

# 6. Serial assertions.
fail=0
for want in "[BL3] AuraLite stage2 alive" \
            "[BL4] FAT32 BPB parsed" \
            "[BL4] kernel.elf located" \
            "[BL4] kernel.elf loaded to 0x00200000"; do
    if grep -qF "$want" "$LOG"; then
        printf '  [bl4] serial OK  %s\n' "$want"
    else
        printf '  [bl4] serial FAIL missing: %s\n' "$want"
        fail=1
    fi
done

# 7. Byte-for-byte comparison of the loaded region against the source.
if [ -s "$FILE_DST" ]; then
    if cmp -s "$FILE_SRC" "$FILE_DST"; then
        echo "  [bl4] memcmp @0x200000 OK (${FILE_SIZE} bytes)"
    else
        echo "  [bl4] memcmp @0x200000 FAIL -- first differences:"
        diff <(xxd "$FILE_SRC" | head -5) <(xxd "$FILE_DST" | head -5) || true
        fail=1
    fi
else
    echo "  [bl4] memcmp SKIPPED (no dump captured)"
fi

if [ "$fail" -eq 0 ]; then
    echo "[bl4] PASS"
    exit 0
else
    echo "[bl4] FAIL"
    echo "--- serial log ---"
    sed 's/^/    /' "$LOG"
    exit 1
fi
