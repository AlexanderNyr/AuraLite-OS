#!/usr/bin/env bash
# Writable FAT32 fixture for SH5d and SH8. Rust is a bootstrap input; do
# not seed C/asm objects, a finished kernel ELF, or the generated ISO.
selfhost_make_disk() {
    local disk=$1 root=$2 lba=64 sectors=$((128 * 2048))
    local fat="$disk@@$((lba * 512))"
    (cd "$root" && make selfhost-gui-input) || return 1
    rm -f "$disk"
    mkdir -p "$(dirname "$disk")" || return 1
    truncate -s "$(((lba + sectors) * 512))" "$disk" || return 1
    mformat -i "$fat" -F -c 1 -T "$sectors" -H "$lba" :: || return 1
    mcopy -i "$fat" "$root/build/selfhost/gui-core-link.o" ::/GUICORE.O || return 1
    python3 - "$disk" "$lba" "$sectors" <<'PY'
import struct, sys
with open(sys.argv[1], 'r+b') as image:
    image.seek(0x1be)
    image.write(struct.pack('<BBBBBBBBII', 0x80, 0, 2, 0, 0x0c, 0, 0, 0,
                            int(sys.argv[2]), int(sys.argv[3])))
    image.seek(510)
    image.write(b'\x55\xaa')
PY
}
