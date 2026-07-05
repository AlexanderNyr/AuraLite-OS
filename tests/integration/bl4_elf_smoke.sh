#!/usr/bin/env bash
# tests/integration/bl4_elf_smoke.sh -- exercise ELF PT_LOAD copying.
#
# Uses the REAL kernel.elf (built with `make kernel`) as payload so
# that we exercise elf_load against a genuine higher-half ELF64
# image, not a synthetic pattern.  We do not yet enter long mode --
# that lands in the next BL4 sub-commit.  Instead we assert:
#
#   1. Stage 2 prints "[BL4] ELF PT_LOAD segments copied to phys".
#   2. Physical memory at 0x00100000 begins with the ELF file's first
#      PT_LOAD file bytes (verified by dumping and diff-ing).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/bl4_elf.img"
LOG="$BUILD/bl4_elf.log"
KERNEL_PROBE="$BUILD/bl4_kernel_probe.bin"     # expected bytes at 0x100000
KERNEL_DUMP="$BUILD/bl4_kernel_dump.bin"

# 1. Build MBR, Stage 2 and the actual kernel image.
make -C "$ROOT" mbr stage2 kernel >/dev/null

# 2. Extract the first PT_LOAD segment's bytes (file image) and their
#    physical destination.  Use Python's struct to parse the ELF64
#    header directly (no external deps).
python3 - "$BUILD/kernel.elf" "$KERNEL_PROBE" <<'PY'
import struct, sys
elf = open(sys.argv[1], 'rb').read()
assert elf[:4] == b'\x7fELF' and elf[4] == 2
e_phoff     = struct.unpack_from('<Q', elf, 32)[0]
e_phentsize = struct.unpack_from('<H', elf, 54)[0]
e_phnum     = struct.unpack_from('<H', elf, 56)[0]
KERNEL_VMA  = 0xFFFFFFFF80000000
first_load = None
for i in range(e_phnum):
    ph = elf[e_phoff + i*e_phentsize : e_phoff + (i+1)*e_phentsize]
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
        struct.unpack('<IIQQQQQQ', ph)
    if p_type == 1:  # PT_LOAD
        first_load = (p_offset, p_vaddr, p_paddr, p_filesz)
        break
assert first_load is not None
p_offset, p_vaddr, p_paddr, p_filesz = first_load
# Physical destination = p_paddr - KERNEL_VMA (see elf.inc for derivation).
phys = (p_paddr - KERNEL_VMA) & 0xFFFFFFFF
n = min(256, p_filesz)
probe = elf[p_offset : p_offset + n]
open(sys.argv[2], 'wb').write(probe)
print(f"probe: first PT_LOAD paddr=0x{p_paddr:x} -> phys=0x{phys:x}, saved {n} bytes")
PY

# 3. Build a bootable image with kernel.elf inside the FAT partition.
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
dd if="$BUILD/boot/mbr.bin"    of="$IMG" bs=512 count=1 conv=notrunc status=none
dd if="$BUILD/boot/stage2.bin" of="$IMG" bs=512 seek=1 conv=notrunc status=none

FAT_IMG="$BUILD/bl4_elf_part.img"
dd if=/dev/zero of="$FAT_IMG" bs=1M count=15 status=none
mformat -i "$FAT_IMG" -F -h 32 -s 32 -t 480 ::
mcopy -i "$FAT_IMG" "$BUILD/kernel.elf" ::KERNEL.ELF
dd if="$FAT_IMG" of="$IMG" bs=512 seek=128 conv=notrunc status=none

# MBR partition entry for LBA 128.
python3 - "$IMG" <<'PY'
import struct, sys
img = open(sys.argv[1], 'r+b')
img.seek(0x1BE)
img.write(struct.pack('<BBBBBBBBII',
    0x80, 0, 2, 0,
    0x0C,
    0, 0, 0,
    128, 15*2048))
img.close()
PY

# 4. Run under QEMU.
rm -f "$LOG"
timeout 25 qemu-system-x86_64 \
    -drive format=raw,file="$IMG",if=ide \
    -m 128M \
    -display none -serial file:"$LOG" -no-reboot \
    -monitor unix:"$BUILD/bl4_elf_monitor.sock",server,nowait \
    >/dev/null 2>&1 &
QEMU_PID=$!

for _ in 1 2 3 4 5 6 7 8; do
    [ -S "$BUILD/bl4_elf_monitor.sock" ] && break
    sleep 0.5
done
sleep 12   # ELF copy + 1.5 MiB BSS zero-fill takes a while under TCG

rm -f "$KERNEL_DUMP"
if [ -S "$BUILD/bl4_elf_monitor.sock" ] && command -v socat >/dev/null 2>&1; then
    {
        printf 'pmemsave 0x00100000 256 "%s"\n' "$KERNEL_DUMP"
        sleep 0.4
        printf 'quit\n'
    } | socat -T3 -,ignoreeof UNIX-CONNECT:"$BUILD/bl4_elf_monitor.sock" >/dev/null 2>&1 || true
    sleep 0.5
fi
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

# 5. Serial assertions.
fail=0
for want in "[BL4] FAT32 BPB parsed" \
            "[BL4] kernel.elf located" \
            "[BL4] kernel.elf loaded to 0x00200000" \
            "[BL4] ELF PT_LOAD segments copied to phys"; do
    if grep -qF "$want" "$LOG"; then
        printf '  [bl4-elf] serial OK  %s\n' "$want"
    else
        printf '  [bl4-elf] serial FAIL missing: %s\n' "$want"
        fail=1
    fi
done

# 6. Compare 256 bytes at physical 0x00100000 against the probe.
if [ -s "$KERNEL_DUMP" ]; then
    if cmp -s "$KERNEL_PROBE" "$KERNEL_DUMP"; then
        echo "  [bl4-elf] memcmp @0x100000 OK (256 bytes)"
    else
        echo "  [bl4-elf] memcmp @0x100000 FAIL -- probe/dump differ"
        echo "    probe:"
        od -An -tx1 -N32 "$KERNEL_PROBE" | sed 's/^/      /'
        echo "    dump:"
        od -An -tx1 -N32 "$KERNEL_DUMP" | sed 's/^/      /'
        fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then
    echo "[bl4-elf] PASS"
    exit 0
else
    echo "[bl4-elf] FAIL"
    echo "--- serial log ---"
    sed 's/^/    /' "$LOG"
    exit 1
fi
