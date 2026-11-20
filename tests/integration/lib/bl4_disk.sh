#!/usr/bin/env bash
# Shared BIOS fixture for the BL4 tests that boot the real kernel ELF.
bl4_make_kernel_disk() {
    local build=$1 image=$2 fat="${2%.img}_part.img" lba=128
    local bytes sectors
    bytes=$(wc -c < "$build/kernel.elf")
    # 64 MiB minimum: enough clusters for FAT32 with 512-byte sectors.
    # Round up and leave room for the boot sectors and both FAT copies.
    sectors=$(( ((bytes + 8 * 1024 * 1024 + 1048575) / 1048576) * 2048 ))
    if (( sectors < 64 * 2048 )); then sectors=$((64 * 2048)); fi

    rm -f "$image" "$fat"
    truncate -s "$(((lba + sectors) * 512))" "$image"
    truncate -s "$((sectors * 512))" "$fat"
    mformat -i "$fat" -F -T "$sectors" ::
    mcopy -i "$fat" "$build/kernel.elf" ::KERNEL.ELF
    dd if="$build/boot/mbr.bin" of="$image" bs=512 count=1 conv=notrunc status=none
    dd if="$build/boot/stage2.bin" of="$image" bs=512 seek=1 conv=notrunc status=none
    dd if="$fat" of="$image" bs=512 seek="$lba" conv=notrunc,sparse status=none
    rm -f "$fat"
    python3 - "$image" "$lba" "$sectors" <<'PY'
import struct, sys
with open(sys.argv[1], 'r+b') as img:
    img.seek(0x1be)
    img.write(struct.pack('<BBBBBBBBII', 0x80, 0, 2, 0, 0x0c, 0, 0, 0,
                          int(sys.argv[2]), int(sys.argv[3])))
PY
}
