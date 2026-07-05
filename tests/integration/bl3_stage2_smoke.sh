#!/usr/bin/env bash
# tests/integration/bl3_stage2_smoke.sh -- smoke test for BL3 Stage 2.
#
# Composes a disk image with the real BL2 MBR at sector 0 and the real
# BL3 Stage 2 at LBA 1..N, boots it under QEMU/SeaBIOS and asserts that
# the Stage-2 progress log appears on COM1.
#
# Passes the whole boot_info block at 0x00010000 through a QEMU memory
# dump too, verifying that BL3 wrote:
#   * BOOT_MAGIC (0x4155524142544C44) at offset 0
#   * hhdm_offset (0xFFFF800000000000) at its expected offset
#   * mmap_count > 0
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/bl3_smoke.img"
LOG="$BUILD/bl3_smoke.log"
MEMDUMP="$BUILD/bl3_smoke.mem"

# 1. Build the real BL2 + BL3 artefacts.
make -C "$ROOT" mbr stage2 >/dev/null

# 2. Assemble a 4 MiB flat image: MBR at LBA 0, Stage 2 from LBA 1.
dd if=/dev/zero of="$IMG" bs=1M count=4 status=none
dd if="$BUILD/boot/mbr.bin"    of="$IMG" bs=1  count=446 conv=notrunc status=none
# Restore the boot signature after the partial MBR copy above.
printf '\x55\xaa' | dd of="$IMG" bs=1 seek=510 count=2 conv=notrunc status=none
# Actually the safe way is to copy the whole 512 bytes at once.  The
# partial copy was a defensive earlier approach; overwrite in one shot.
dd if="$BUILD/boot/mbr.bin"    of="$IMG" bs=512 count=1 conv=notrunc status=none
dd if="$BUILD/boot/stage2.bin" of="$IMG" bs=512 seek=1 conv=notrunc status=none

# 3. Run under QEMU with COM1 -> file capture.  We use -kernel-less boot
#    from the raw disk.  8 seconds is plenty for the BL3 flow which ends
#    at a hlt.
rm -f "$LOG" "$MEMDUMP"
timeout 10 qemu-system-x86_64 \
    -drive format=raw,file="$IMG",if=ide \
    -m 128M \
    -display none -serial file:"$LOG" -no-reboot \
    -monitor unix:"$BUILD/bl3_monitor.sock",server,nowait \
    >/dev/null 2>&1 &
QEMU_PID=$!

# Wait for stage2 to reach the .hang loop, then dump memory via the
# QEMU monitor and kill the VM.
for _ in 1 2 3 4 5 6; do
    [ -S "$BUILD/bl3_monitor.sock" ] && break
    sleep 0.5
done
sleep 1.5

if [ -S "$BUILD/bl3_monitor.sock" ] && command -v socat >/dev/null 2>&1; then
    # Send monitor commands without any echo/prompt feedback loop by
    # driving the socket with STDIO=cronoly and closing it right after
    # the last command.  We escape the destination path so it survives
    # shell expansion inside socat's exec pipeline.
    {
        printf 'pmemsave 0x10000 0x2000 "%s"\n' "$MEMDUMP"
        sleep 0.4
        printf 'quit\n'
    } | socat -T3 -,ignoreeof UNIX-CONNECT:"$BUILD/bl3_monitor.sock" >/dev/null 2>&1 || true
    sleep 0.5
fi
# Make sure QEMU is gone even if the monitor exchange failed.
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

# 4. Assertions on the serial log.
fail=0
for want in "[BL3] AuraLite stage2 alive" "[BL3] E820 done" "[BL3] A20 gate on" "[BL3] real-mode services complete"; do
    if grep -qF "$want" "$LOG"; then
        printf '  [bl3] serial OK  %s\n' "$want"
    else
        printf '  [bl3] serial FAIL missing: %s\n' "$want"
        fail=1
    fi
done

# 5. Assertions on the boot_info memory dump (only if the monitor
#    capture worked -- some hosts lack socat).
if [ -s "$MEMDUMP" ]; then
    python3 - "$MEMDUMP" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
magic = struct.unpack_from('<Q', data, 0)[0]
BOOT_MAGIC = 0x4155524142544C44
BOOT_HHDM_OFF = 8 + 24 + 256*24 + 4 + 4
hhdm  = struct.unpack_from('<Q', data, BOOT_HHDM_OFF)[0]
mmapc = struct.unpack_from('<I', data, 32 + 256*24)[0]
ok = True
print(f'  [bl3] mem magic  = 0x{magic:016x}   {"OK" if magic == BOOT_MAGIC else "FAIL"}')
print(f'  [bl3] mem hhdm   = 0x{hhdm:016x}   {"OK" if hhdm == 0xffff800000000000 else "FAIL"}')
print(f'  [bl3] mem mmap_c = {mmapc}                   {"OK" if mmapc > 0 else "FAIL (empty E820)"}')
if magic != BOOT_MAGIC or hhdm != 0xffff800000000000 or mmapc == 0:
    sys.exit(1)
PY
    if [ $? -ne 0 ]; then
        fail=1
    fi
else
    echo '  [bl3] memdump skipped (no socat or QEMU monitor unavailable)'
fi

if [ "$fail" -eq 0 ]; then
    echo "[bl3] PASS"
    exit 0
else
    echo "[bl3] FAIL -- serial log:"
    sed 's/^/    /' "$LOG"
    exit 1
fi
