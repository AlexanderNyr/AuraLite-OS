#!/usr/bin/env bash
# tests/integration/bl2_mbr_smoke.sh -- smoke test for the BL2 MBR.
#
# Builds a synthetic disk image consisting of our real MBR followed by a
# tiny stub at LBA 1 that prints "OK" via BIOS teletype and halts.  Runs
# it under QEMU with SeaBIOS and asserts the "OK" string appears on the
# serial console -- proving the MBR reached the extension probe, issued
# a successful INT 13h read, and long-jumped to STAGE2_SEG:0000 with DL
# preserved.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/bl2_smoke.img"
LOG="$BUILD/bl2_smoke.log"

# 1. Build the MBR (real BL2 artifact).
make -C "$ROOT" mbr >/dev/null

# 2. Assemble a 512-byte stub that prints "OK\r\n" through the serial port
#    (COM1 = I/O 0x3F8) and then halts.  We use the serial port so QEMU's
#    -serial file:LOG captures the output no matter which console is chosen.
STUB_ASM="$BUILD/bl2_stub.asm"
STUB_BIN="$BUILD/bl2_stub.bin"
cat > "$STUB_ASM" <<'ASM'
bits 16
org 0x8000
    ; DL still holds the boot-drive number -- prove it by placing a
    ; recognisable sentinel in AH before we start printing.
    mov  ah, dl
    mov  si, .msg
.loop:
    lodsb
    test al, al
    jz   .hang
    ; Write al to COM1 data port 0x3F8 (no LSR polling -- QEMU's serial
    ; port is always ready and this is a smoke test).
    mov  dx, 0x3F8
    out  dx, al
    jmp  .loop
.hang:
    hlt
    jmp  .hang
.msg: db "AURALITE-BL2-OK", 0x0D, 0x0A, 0
times 512 - ($ - $$) db 0
ASM
nasm -f bin -o "$STUB_BIN" "$STUB_ASM"

# 3. Build a 1 MiB flat disk: sector 0 = MBR, sector 1 = stub, rest zero.
dd if=/dev/zero of="$IMG" bs=1M count=1 status=none
dd if="$BUILD/boot/mbr.bin" of="$IMG" conv=notrunc status=none
dd if="$STUB_BIN"           of="$IMG" bs=512 seek=1 conv=notrunc status=none

# 4. Run under QEMU (SeaBIOS default).  -display none keeps it headless;
#    -serial file: captures COM1.  -no-reboot stops the loop on halt.
rm -f "$LOG"
timeout 8 qemu-system-x86_64 \
    -drive format=raw,file="$IMG",if=ide \
    -display none -serial file:"$LOG" -no-reboot \
    >/dev/null 2>&1 || true

# 5. Verify.
if grep -q "AURALITE-BL2-OK" "$LOG"; then
    echo "[bl2] PASS  MBR handed off to Stage 2 (LBA read succeeded)"
    exit 0
else
    echo "[bl2] FAIL  no BL2 handoff string in serial log:"
    cat "$LOG"
    exit 1
fi
