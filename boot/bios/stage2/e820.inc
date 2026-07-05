; =============================================================================
; boot/bios/stage2/e820.inc -- BIOS INT 15h EAX=E820h memory map query.
;
; Fills boot_info_t.mmap[] and boot_info_t.mmap_count.
;
; Each E820 entry the BIOS returns is 20 bytes: base(8) length(8) type(4).
; boot_mmap_entry_t is 24 bytes: base(8) length(8) type(4) _pad(4).
; We copy the 20-byte BIOS record into the low 20 bytes of the boot_info
; slot and leave the last 4 bytes zero (they were pre-zeroed by the
; block-clear at the top of stage2_entry).
;
; E820 type numbers 1..5 already match BOOT_MEM_USABLE..BOOT_MEM_BAD
; verbatim, so no translation is required.
;
; Reference: Ralph Brown's Interrupt List, INT 15h AX=E820h;
;            ACPI 6.5 spec, s.15.3 "System Address Map Interfaces".
; =============================================================================

; boot_info_t offsets are generated from boot/shared/boot_info.h and included
; by stage2_start.asm before this file.
E820_ENTRY_SIZE      equ 24         ; boot_mmap_entry_t stride

; detect_memory -- fills boot_info.mmap[] up to BOOT_MAX_MMAP=256 entries
; via INT 15h EAX=E820h.
;
; Accesses the boot_info block via ES = BOOT_INFO_SEG so all field
; offsets stay within a 16-bit displacement.
;
; On entry: DS = 0.
; On exit : mmap_count stored; carry clear; ES/DI clobbered internally.
detect_memory:
    pusha
    push es

    mov  ax, BOOT_INFO_SEG
    mov  es, ax
    mov  di, BOOT_MMAP_OFF            ; ES:DI -> first slot
    xor  ebx, ebx                      ; continuation = 0
    xor  bp, bp                        ; counter

.loop:
    ; Zero the _pad half of the slot (BIOS writes only 20 bytes; we
    ; keep the trailing 4 bytes zero so the u32 type field remains
    ; clean and the pad stays zero).
    mov  dword [es:di + 20], 0

    mov  eax, 0x0000E820
    mov  edx, 0x534D4150              ; 'SMAP'
    mov  ecx, 20                      ; request 20-byte descriptor
    int  0x15
    jc   .done                        ; CF set: end of list
    cmp  eax, 0x534D4150
    jne  .done                        ; BIOS did not honour the call

    ; Skip zero-length entries (some BIOSes emit them).
    mov  eax, [es:di + 8]             ; length low
    or   eax, [es:di + 12]            ; length high
    jz   .skip

    add  di, E820_ENTRY_SIZE
    inc  bp
    cmp  bp, 256
    jae  .done                        ; refuse to overflow the array
.skip:
    test ebx, ebx
    jnz  .loop                        ; more entries available
.done:
    ; Store the count (u32 -- upper half stays zero from block-clear).
    mov  [es:BOOT_MMAP_CNT_OFF], bp
    clc
    pop  es
    popa
    ret
