; =============================================================================
; boot/bios/stage2/elf32.inc -- ELF32 loader (real mode + unreal FS).
;
; Sibling of elf.inc (I386_PLAN I1).  Consumes an ELF32 image previously
; staged at a flat linear address by fat_load, walks the 32-bit program
; headers, and copies every PT_LOAD segment to its physical destination.
;
; Unlike the 64-bit kernel (linked higher-half, phys = p_paddr - VMA),
; the i386 kernel/stub is linked with VMA = LMA = 0x00100000 -- paging
; does not exist yet on this path, so p_paddr IS the destination and no
; translation is needed.  That asymmetry is deliberate: the 32-bit
; kernel enables paging itself in phase I3, exactly as the original
; kernel did before the loader took the job over.
;
; ELF32 field offsets (System V ABI, Elf32_Ehdr / Elf32_Phdr):
;   e_entry     @24 (4)      p_type   @ 0 (4)
;   e_phoff     @28 (4)      p_offset @ 4 (4)
;   e_phentsize @42 (2)      p_paddr  @12 (4)
;   e_phnum     @44 (2)      p_filesz @16 (4)
;                            p_memsz  @20 (4)
;
; Requires unreal mode (FS lifted to 4 GiB flat).
;
; Public entry
; ------------
;   elf32_load(EAX = image flat linear address)
;       On success: CF=0, [elf32_entry_addr] = 4-byte kernel entry point.
;       On failure: CF=1 (bad magic, or ELFCLASS != 1).
; =============================================================================

; Persistent state.
elf32_entry_addr:     dd 0                 ; kernel32 _start physical address
_elf32_image_base:    dd 0
_elf32_phdr_off:      dd 0
_elf32_phnum:         dd 0
_elf32_phentsize:     dd 0

ELFCLASS32            equ 1

; ---------------------------------------------------------------------------
; elf32_load(EAX = image flat address)
; ---------------------------------------------------------------------------
elf32_load:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp

    mov  [_elf32_image_base], eax
    mov  esi, eax                         ; ESI = flat ptr to Ehdr

    ; Magic: bytes 0..3 == { 0x7F, 'E', 'L', 'F' }.
    mov  eax, [fs:esi]
    cmp  eax, 0x464C457F
    jne  .fail

    ; ELFCLASS32 (byte at offset 4 == 1).  The 64-bit loader checks for
    ; 2 here; each loader refuses the other's class so a mis-copied
    ; kernel file fails loudly at parse time, not at jump time.
    mov  al, [fs:esi + 4]
    cmp  al, ELFCLASS32
    jne  .fail

    ; e_entry (4 bytes at offset 24).
    mov  eax, [fs:esi + 24]
    mov  [elf32_entry_addr], eax

    ; e_phoff (4 bytes at offset 28).
    mov  eax, [fs:esi + 28]
    mov  [_elf32_phdr_off], eax

    ; e_phentsize (u16 @42), e_phnum (u16 @44).
    movzx eax, word [fs:esi + 42]
    mov  [_elf32_phentsize], eax
    movzx eax, word [fs:esi + 44]
    mov  [_elf32_phnum], eax

    ; Iterate program headers.
    xor  ecx, ecx                         ; ECX = index
.ph_loop:
    cmp  ecx, [_elf32_phnum]
    jae  .done

    ; Flat address of this phdr: image_base + phoff + i*phentsize.
    mov  eax, ecx
    mul  dword [_elf32_phentsize]         ; EDX:EAX = i * phentsize
    add  eax, [_elf32_phdr_off]
    add  eax, [_elf32_image_base]
    mov  edi, eax                         ; EDI = phdr flat addr

    ; Skip non-PT_LOAD.  (PT_LOAD equ 1 comes from elf.inc, which is
    ; always included alongside this file.)
    mov  eax, [fs:edi + 0]                ; p_type
    cmp  eax, PT_LOAD
    jne  .next

    push ecx
    call _elf32_copy_segment              ; consumes EDI = phdr addr
    pop  ecx

.next:
    inc  ecx
    jmp  .ph_loop

.done:
    pop  ebp
    pop  edi
    pop  esi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    clc
    ret
.fail:
    pop  ebp
    pop  edi
    pop  esi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    stc
    ret

; ---------------------------------------------------------------------------
; Internal: copy one PT_LOAD segment.  EDI = flat address of the Phdr.
; Same dword-then-tail copy and BSS zero-fill as the 64-bit sibling.
; Trashes EAX, EBX, ECX, EDX, ESI, EDI.
; ---------------------------------------------------------------------------
_elf32_copy_segment:
    push ebp

    ; src = image_base + p_offset.
    mov  esi, [fs:edi + 4]                ; p_offset
    add  esi, [_elf32_image_base]         ; ESI = source flat

    ; dst = p_paddr, verbatim (VMA = LMA, see header comment).
    mov  ebp, [fs:edi + 12]               ; EBP = dst flat

    ; filesz / memsz.
    mov  ebx, [fs:edi + 16]               ; p_filesz
    mov  edx, [fs:edi + 20]               ; p_memsz

.copy_dwords:
    cmp  ebx, 4
    jb   .copy_tail
    mov  eax, [fs:esi]
    mov  [fs:ebp], eax
    add  esi, 4
    add  ebp, 4
    sub  ebx, 4
    jmp  .copy_dwords
.copy_tail:
    test ebx, ebx
    jz   .zbss
    mov  al, [fs:esi]
    mov  [fs:ebp], al
    inc  esi
    inc  ebp
    dec  ebx
    jmp  .copy_tail

.zbss:
    ; Zero-fill (memsz - filesz) bytes; EBP points at dst + filesz.
    mov  ecx, edx
    sub  ecx, [fs:edi + 16]               ; ECX = bss bytes
    xor  eax, eax
.zloop_dwords:
    cmp  ecx, 4
    jb   .zloop_tail
    mov  [fs:ebp], eax
    add  ebp, 4
    sub  ecx, 4
    jmp  .zloop_dwords
.zloop_tail:
    test ecx, ecx
    jz   .cs_done
    mov  byte [fs:ebp], 0
    inc  ebp
    dec  ecx
    jmp  .zloop_tail
.cs_done:
    pop  ebp
    ret
