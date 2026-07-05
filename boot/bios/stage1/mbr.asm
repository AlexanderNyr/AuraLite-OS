; =============================================================================
; boot/bios/stage1/mbr.asm -- AuraLite OS Master Boot Record (Stage 1).
;
; Phase BL2 of the custom-bootloader plan.
;
; Loaded by the legacy BIOS at physical address 0x0000:0x7C00 in 16-bit
; real mode, with:
;   * DL  = BIOS boot-drive number (must be forwarded to Stage 2 so it
;           can keep reading sectors from the same device).
;   * CS:IP = 0000:7C00.
;   * SS/SP undefined -- we set our own before the first push/call.
;
; What we do
; ----------
;   1. Set DS = ES = SS = 0, SP = 0x7C00 (stack grows down from just
;      below our own code).
;   2. Try INT 13h AH=41h (Check Extensions Present) with BX=0x55AA:
;      * On success (BX flipped to 0xAA55, carry clear, AH=1/2/3) we
;        use LBA read (INT 13h AH=42h + Disk Address Packet).
;      * On failure we fall back to CHS INT 13h AH=02h; this is enough
;        for the first 63 sectors on any drive.
;   3. Load STAGE2_SECTS (126) sectors starting at STAGE2_LBA (=1) into
;      STAGE2_SEG:0000 = physical 0x8000.  126*512 = 63 KiB, the max
;      that fits between our load address and the BIOS-reserved area
;      at 0x9FC00 with plenty of slack.
;   4. Long-jump to STAGE2_SEG:0x0000 with DL preserved so Stage 2
;      knows the boot drive.
;
; Layout of the 512-byte sector
; -----------------------------
;   0x000 .. 0x1BD    code + data (446 bytes)
;   0x1BE .. 0x1FD    four 16-byte MBR partition entries (filled by
;                     mkdisk / xorriso later, zeroed at assembly time)
;   0x1FE .. 0x1FF    0x55 0xAA boot signature
;
; References
; ----------
;   Ralph Brown's Interrupt List, INT 13h / AH=02h, 41h, 42h.
;   Intel 8086 Family User's Manual (real-mode segmentation).
; =============================================================================

bits 16
org  0x7C00

; ---------------------------- Constants -------------------------------------

STAGE2_SEG      equ 0x0800          ; load Stage 2 at 0x0800:0000 = phys 0x8000
STAGE2_LBA      equ 1               ; first LBA of Stage 2 on the boot disk
STAGE2_SECTS    equ 126             ; sectors to load (63 KiB)

; ---------------------------- Entry point -----------------------------------

start:
    cli                             ; no interrupts during segment setup
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00                 ; stack grows DOWN from our load address
    sti

    ; Preserve the BIOS-supplied boot-drive number for later.
    mov  [boot_drive], dl

    ; -------- Probe for INT 13h extensions (LBA support) ------------------
    ;   AH = 41h, BX = 55AAh, DL = drive.
    ;   On success: CF clear, BX = AA55h, CX bit 0 set (packet access).
    mov  ah, 0x41
    mov  bx, 0x55AA
    int  0x13
    jc   .no_ext                    ; carry set => extensions absent
    cmp  bx, 0xAA55
    jne  .no_ext

    ; -------- LBA read via Disk Address Packet (INT 13h AH=42h) -----------
    mov  si, dap
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    jc   .disk_error
    jmp  .launch

.no_ext:
    ; -------- CHS fallback (INT 13h AH=02h) --------------------------------
    ; Reads STAGE2_SECTS sectors starting at CHS (cyl=0, head=0, sector=2)
    ; into ES:BX = 0000:8000.  Enough for boot media that lacks LBA support
    ; (obsolete floppies, some VMs); real hardware from ~2000 onward has
    ; LBA and takes the fast path above.
    mov  ah, 0x02                   ; function 02h: read sectors
    mov  al, STAGE2_SECTS           ; sectors to read
    mov  ch, 0                      ; cylinder low  = 0
    mov  cl, 2                      ; sector = 2 (1-based; sector 1 is us)
    mov  dh, 0                      ; head = 0
    mov  dl, [boot_drive]
    xor  bx, bx
    mov  es, bx
    mov  bx, 0x8000                 ; ES:BX = 0000:8000
    int  0x13
    jc   .disk_error

.launch:
    ; Restore DL for Stage 2 and long-jump to STAGE2_SEG:0000.
    mov  dl, [boot_drive]
    jmp  STAGE2_SEG:0x0000

; ---------------------------- Error path ------------------------------------

.disk_error:
    mov  si, msg_err
    call bios_puts
.halt:
    hlt
    jmp  .halt

; ---------------------------- Helpers ---------------------------------------

; bios_puts -- print zero-terminated string at DS:SI via BIOS teletype.
bios_puts:
    lodsb
    test al, al
    jz   .done
    mov  ah, 0x0E
    mov  bx, 0x0007                 ; page 0, attr grey-on-black
    int  0x10
    jmp  bios_puts
.done:
    ret

; ---------------------------- Data ------------------------------------------

msg_err:    db "AuraLite: boot disk read error", 0x0D, 0x0A, 0

; Disk Address Packet for INT 13h AH=42h.
; Structure (16 bytes, ATA/ATAPI Spec):
;   +0  size (10h)
;   +1  reserved (0)
;   +2  sectors to transfer (word)
;   +4  destination offset : segment (word:word)
;   +8  LBA start (qword)
align 2
dap:
    db   0x10                       ; DAP size
    db   0                          ; reserved
    dw   STAGE2_SECTS               ; sector count
    dw   0x0000                     ; destination offset
    dw   STAGE2_SEG                 ; destination segment
    dq   STAGE2_LBA                 ; LBA start (little-endian qword)

boot_drive: db 0

; ---------------------------- Pad + partition table + signature -------------

; Pad up to offset 446 (0x1BE): everything before the four partition entries.
times 446 - ($ - $$) db 0

; Four 16-byte partition table entries -- zero at assembly time; the disk
; imaging tool (mkdisk / xorriso --protective-msdos-label) overwrites this
; block later so the disk appears partitioned to firmware.
times 64 db 0

; Boot signature.
dw 0xAA55
