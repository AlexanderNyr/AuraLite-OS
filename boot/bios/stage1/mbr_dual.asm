; =============================================================================
; boot/bios/stage1/mbr_dual.asm -- AuraLite MBR variant for dual-boot disks.
;
; Same behaviour as boot/bios/stage1/mbr.asm (BL2) except Stage 2 is
; read from LBA 34..159 instead of LBA 1..126.  The change is
; required by BL7's dual-boot layout: the disk carries a GPT header
; at LBA 1 and a GPT partition array at LBA 2..33, so Stage 2 cannot
; live inside that range without corrupting either the boot chain or
; the UEFI-required GPT structures.
;
; Every other detail (INT 13h AH=42h LBA extensions probe, CHS fall-
; back, 0x55AA signature, partition-table gap) matches BL2 verbatim,
; so a bug in this file must exist in mbr.asm too and vice-versa.
; =============================================================================

bits 16
org  0x7C00

STAGE2_SEG      equ 0x0800          ; load Stage 2 at 0x0800:0000 = phys 0x8000
STAGE2_LBA      equ 34              ; first LBA past the GPT header + array
STAGE2_SECTS    equ 126             ; sectors to load (63 KiB, unchanged)

start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti

    mov  [boot_drive], dl

    mov  ah, 0x41
    mov  bx, 0x55AA
    int  0x13
    jc   .no_ext
    cmp  bx, 0xAA55
    jne  .no_ext

    mov  si, dap
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    jc   .disk_error
    jmp  .launch

.no_ext:
    ; CHS fallback is unlikely to hit under any modern firmware, but
    ; keep it as a safety net.  Cyl = 0, head = 1, sector = 3
    ; (approximates LBA 34 = 1*32 + 3 -1 for the -h 32 -s 32
    ; geometry we format the disk with).  If this ever needs to work
    ; on truly obscure hardware, prefer to fail loudly instead of
    ; silently reading the wrong data.
    mov  ah, 0x02
    mov  al, STAGE2_SECTS
    mov  ch, 0
    mov  cl, 3
    mov  dh, 1
    mov  dl, [boot_drive]
    xor  bx, bx
    mov  es, bx
    mov  bx, 0x8000
    int  0x13
    jc   .disk_error

.launch:
    mov  dl, [boot_drive]
    jmp  STAGE2_SEG:0x0000

.disk_error:
    mov  si, msg_err
    call bios_puts
.halt:
    hlt
    jmp  .halt

bios_puts:
    lodsb
    test al, al
    jz   .done
    mov  ah, 0x0E
    mov  bx, 0x0007
    int  0x10
    jmp  bios_puts
.done:
    ret

msg_err: db "AuraLite: boot disk read error (dual)", 0x0D, 0x0A, 0

align 2
dap:
    db   0x10                       ; DAP size
    db   0                          ; reserved
    dw   STAGE2_SECTS               ; sector count
    dw   0x0000                     ; destination offset
    dw   STAGE2_SEG                 ; destination segment
    dq   STAGE2_LBA                 ; LBA start

boot_drive: db 0

times 446 - ($ - $$) db 0
times 64 db 0                       ; partition-table area (filled by mkiso)
dw 0xAA55
