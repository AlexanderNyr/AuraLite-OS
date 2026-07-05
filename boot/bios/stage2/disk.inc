; =============================================================================
; boot/bios/stage2/disk.inc -- BIOS disk read helpers (real mode).
;
; Thin INT 13h AH=42h (LBA read) wrapper used by fat.inc and by the
; unreal-mode kernel-load path.
;
; Because BIOS INT 13h only takes 16-bit segment:offset destinations,
; every read is limited to the low 640 KiB.  Callers that need to place
; data above 1 MiB must read into the scratch buffer at
; STAGE2_SCRATCH_PHYS (which sits at 0x11000 = 0x1100:0000, still in low
; memory) and then copy up while in unreal mode.
; =============================================================================

DISK_RETRIES equ 3

; disk_read_lba -- read CX sectors starting at LBA EAX (low 32 bits;
;                  we assume high 32 bits = 0 which is true for any
;                  disk < 2 TiB) into segment:offset ES:BX.
;
; On entry: EAX = LBA, CX = sector count (must be <= 127 for maximum
;                        portability -- some BIOSes truncate above 128),
;           ES:BX = destination buffer,
;           [boot_drive] = BIOS drive number.
; On exit : CF clear on success, CF set on error.  All GP registers
;           preserved.
disk_read_lba:
    pushad
    push ds
    push es                             ; preserve caller's ES

    ; Build the Disk Address Packet in low memory.  We reuse the fixed
    ; scratch slot below the code so we do not have to worry about
    ; where the caller's SS:SP points.
    mov  [dr_lba_lo],  eax              ; LBA low 32 bits
    mov  dword [dr_lba_hi], 0           ; LBA high 32 bits = 0
    mov  [dr_count],   cx
    mov  [dr_offset],  bx
    mov  ax, es
    mov  [dr_segment], ax

    ; Point DS:SI at the DAP.
    xor  ax, ax
    mov  ds, ax
    mov  si, dap_buffer
    mov  byte [dr_retry_count], DISK_RETRIES

.attempt:
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    jnc  .ok

    ; Reset the disk system between attempts.  Some BIOSes transiently
    ; report a timeout once and succeed on a retry after AH=00h reset.
    xor  ax, ax                         ; be defensive about BIOS clobbers
    mov  ds, ax
    dec  byte [dr_retry_count]
    jz   .fail
    mov  ah, 0x00                       ; AH=00h: reset disk system
    mov  dl, [boot_drive]
    int  0x13
    xor  ax, ax
    mov  ds, ax
    mov  si, dap_buffer
    jmp  .attempt

.ok:
    pop  es
    pop  ds
    popad
    clc
    ret
.fail:
    pop  es
    pop  ds
    popad
    stc
    ret

; ---------------------------------------------------------------------------
; Static Disk Address Packet.  Layout per ATA/ATAPI INT 13h Extensions:
;   +0  size (10h)
;   +1  reserved (0)
;   +2  sectors to transfer (word)
;   +4  destination offset (word)
;   +6  destination segment (word)
;   +8  LBA start (qword)
; ---------------------------------------------------------------------------
align 2
dap_buffer:
    db   0x10                           ; size
    db   0                              ; reserved
dr_count:
    dw   0
dr_offset:
    dw   0
dr_segment:
    dw   0
dr_lba_lo:
    dd   0
dr_lba_hi:
    dd   0
dr_retry_count:
    db   0
