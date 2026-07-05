# Phase BL2 — BIOS Stage 1 (MBR) — Completed

Result commit: `79fbf2b` — `boot: bios/stage1: implement 512-byte MBR (Phase BL2)`
Toolchain:     NASM 2.16 (assembly), QEMU 10.0 + SeaBIOS (smoke test)

---

## Definition of Done — all criteria met

| # | Criterion | Result |
|---|-----------|:------:|
| 1 | `build/boot/mbr.bin` is exactly 512 bytes | **✓** |
| 2 | Bytes 510–511 are `0x55 0xAA` | **✓** |
| 3 | `ndisasm -b 16 build/boot/mbr.bin` shows correct segment setup and DAP | **✓** |
| 4 | `make test-unit` still green (MBR is flat binary, no C linkage) | **36/36** |
| 5 | Bonus: real QEMU/SeaBIOS smoke test | **PASS** |

---

## Files added

| Path | LOC | Purpose |
|------|----:|---------|
| `boot/bios/stage1/mbr.asm` | 130 | The 512-byte Master Boot Record |
| `tests/integration/bl2_mbr_smoke.sh` | 60 | Synthetic-disk QEMU smoke test |
| `Makefile` (edit) | +19 | New `make mbr` target + size / signature guard |

## Disk layout targeted by Stage 1

```
LBA 0            MBR (this file, 512 B)
LBA 1 .. 126     Stage 2  (63 KiB max, loaded to phys 0x8000)
LBA 128+         FAT32 partition (kernel.elf, initrd.tar) — filled later
```

## Runtime contract at hand-off

Stage 2 is entered at `0x0000:8000` with:
* `DL` = BIOS boot-drive number (preserved from BIOS entry).
* `DS = ES = SS = 0`, `SP = 0x7C00`.
* Interrupts enabled.

## Verification transcript

```
$ make mbr
nasm -f bin -o build/boot/mbr.bin boot/bios/stage1/mbr.asm
  [mbr] build/boot/mbr.bin                       512 bytes, sig=0x55AA

$ ndisasm -b16 -o0x7C00 build/boot/mbr.bin | head -14
00007C00  FA                cli
00007C01  31C0              xor ax,ax
00007C03  8ED8              mov ds,ax
00007C05  8EC0              mov es,ax
00007C07  8ED0              mov ss,ax
00007C09  BC007C            mov sp,0x7c00
00007C0C  FB                sti
00007C0D  88169A7C          mov [0x7c9a],dl
00007C11  B441              mov ah,0x41           ; probe LBA extensions
00007C13  BBAA55            mov bx,0x55aa
00007C16  CD13              int 0x13
00007C18  7215              jc  0x7c2f            ; fall through to CHS
00007C1A  81FB55AA          cmp bx,0xaa55
00007C1E  750F              jnz 0x7c2f

$ python3 - <<PY
import struct
data = open('build/boot/mbr.bin','rb').read()
dap = data[0x8a:0x8a+16]
size,resv,sects,off,seg = struct.unpack('<BBHHH', dap[:8])
lba = struct.unpack('<Q', dap[8:16])[0]
print(f'DAP: size={size:#x} sects={sects} dest={seg:#06x}:{off:#06x} LBA={lba}')
PY
DAP: size=0x10 sects=126 dest=0x0800:0x0000 LBA=1

$ bash tests/integration/bl2_mbr_smoke.sh
[bl2] PASS  MBR handed off to Stage 2 (LBA read succeeded)
```

---

## Deviations from the specification

Two minor adjustments improve robustness without changing intent:

1. **CHS fallback: explicit `mov ah,0x02` + `mov al,STAGE2_SECTS`.**
   The spec used `mov ax, 0x0200 | STAGE2_SECTS`, which works for our
   count (126) but silently overwrites `AH` when the count exceeds
   0xFF.  The split form is safer and matches the INT 13h AH=02h
   documentation more directly.

2. **`bios_puts` sets `BX=0x0007`.**  INT 10h AH=0Eh takes the page
   number in `BH` and the text attribute in `BL` (for graphics
   modes).  Zero-initialising BX prevents undefined attribute bytes
   from corrupting the on-screen error message.

Everything else (STAGE2_SEG, STAGE2_LBA, STAGE2_SECTS, DAP layout,
partition-table gap, boot signature) matches the specification verbatim.

---

## What is now unblocked

Phase BL3 (BIOS Stage 2 real-mode services: E820, VBE, A20, FAT32,
ELF64) can start.  Stage 2's entry point is fixed at `0x0000:8000`
and the boot drive is in `DL` — the contract the MBR just proved
under QEMU/SeaBIOS.
