; =============================================================================
; boot/bios/stage2/elf.inc -- ELF64 loader (real mode + unreal FS).
;
; Consumes the whole ELF image previously staged at some flat linear
; address (fat_load put it there), walks the program headers, and
; copies every PT_LOAD segment to its physical destination address.
;
; The AuraLite kernel is linked at virtual 0xFFFFFFFF80100000 with
; kernel.ld.  The linker does NOT emit AT() clauses, so p_paddr equals
; p_vaddr = 0xFFFFFFFF80xxxxxx for every PT_LOAD.  The physical load
; base (0x00100000) is already encoded inside p_paddr, so translation
; reduces to a single subtraction:
;
;     phys = p_paddr - KERNEL_VMA          (KERNEL_VMA = 0xFFFFFFFF80000000)
;
; In the low 32 bits this is `low32(p_paddr) - 0x80000000` because the
; upper 32 bits of KERNEL_VMA (0xFFFFFFFF) cancel with the sign-extended
; upper half of p_paddr.  Concretely for the first PT_LOAD:
;     low32(0xFFFFFFFF80100000) = 0x80100000
;     0x80100000 - 0x80000000    = 0x00100000    <- correct load phys.
;
; Requires unreal mode (FS lifted to 4 GiB flat).
;
; Public entry
; ------------
;   elf_load(EAX = image flat linear address, EDX = image size)
;       On success: CF=0, [elf_entry_addr] = 8-byte kernel entry point.
;       On failure: CF=1.
; =============================================================================

; Persistent state.
elf_entry_addr:      dq 0                  ; kernel _start virtual address
_elf_image_base:     dd 0
_elf_phdr_off:       dd 0                  ; low 32 bits of e_phoff
_elf_phnum:          dd 0
_elf_phentsize:      dd 0

; ELF constants.
PT_LOAD              equ 1
KERNEL_VMA_LOW32     equ 0x80000000        ; low 32 bits of 0xFFFFFFFF80000000

; ---------------------------------------------------------------------------
; elf_load(EAX = image, EDX = size)
; ---------------------------------------------------------------------------
elf_load:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp

    mov  [_elf_image_base], eax
    mov  esi, eax                         ; ESI = flat ptr to Ehdr

    ; Magic: bytes 0..3 == { 0x7F, 'E', 'L', 'F' }.
    mov  eax, [fs:esi]
    cmp  eax, 0x464C457F
    jne  .fail

    ; ELFCLASS64 (byte at offset 4 == 2).
    mov  al, [fs:esi + 4]
    cmp  al, 2
    jne  .fail

    ; Save e_entry (8 bytes at offset 24).
    mov  eax, [fs:esi + 24]
    mov  [elf_entry_addr + 0], eax
    mov  eax, [fs:esi + 28]
    mov  [elf_entry_addr + 4], eax

    ; e_phoff (8 bytes at offset 32).  We only support ELF images
    ; where the header table lives in the first 4 GiB of the file,
    ; so ignore the upper 32 bits.
    mov  eax, [fs:esi + 32]
    mov  [_elf_phdr_off], eax

    ; e_phentsize (u16 @54), e_phnum (u16 @56).
    movzx eax, word [fs:esi + 54]
    mov  [_elf_phentsize], eax
    movzx eax, word [fs:esi + 56]
    mov  [_elf_phnum], eax

    ; Iterate program headers.
    xor  ecx, ecx                         ; ECX = index
.ph_loop:
    cmp  ecx, [_elf_phnum]
    jae  .done

    ; Compute flat address of this phdr: image_base + phoff + i*phentsize.
    mov  eax, ecx
    mul  dword [_elf_phentsize]           ; EDX:EAX = i * phentsize
    add  eax, [_elf_phdr_off]
    add  eax, [_elf_image_base]
    mov  edi, eax                         ; EDI = phdr flat addr

    ; Skip non-PT_LOAD.
    mov  eax, [fs:edi + 0]                ; p_type
    cmp  eax, PT_LOAD
    jne  .next

    push ecx                              ; _elf_copy_segment trashes ECX
    call _elf_copy_segment                ; consumes EDI = phdr addr
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
; Internal: copy one PT_LOAD segment.
; EDI = flat address of the Phdr.
; Trashes EAX, EBX, ECX, EDX, ESI, EDI.
; ---------------------------------------------------------------------------
_elf_copy_segment:
    push ebp

    ; src = image_base + p_offset (low 32 bits of p_offset only).
    mov  esi, [fs:edi + 8]                ; p_offset (low 32)
    add  esi, [_elf_image_base]           ; ESI = source flat

    ; dst = low32(p_paddr) - KERNEL_VMA_LOW32
    ;      = 0x80100000 - 0x80000000 = 0x00100000 for the first PT_LOAD.
    mov  eax, [fs:edi + 24]               ; p_paddr (low 32)
    sub  eax, KERNEL_VMA_LOW32
    mov  ebp, eax                         ; EBP = dst flat

    ; filesz = low 32 bits of p_filesz.
    mov  ebx, [fs:edi + 32]

    ; memsz = low 32 bits of p_memsz.
    mov  edx, [fs:edi + 40]

    ; Copy [ESI..ESI+EBX) to [EBP..EBP+EBX) in dword steps, then a
    ; byte tail.  Cuts the load time for a 300 KiB .text from ~2 s
    ; to a few ms under TCG QEMU.
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
    ; Zero-fill (memsz - filesz) bytes.  EBP now points at dst + filesz.
    ; The BSS on some segments is > 1 MiB, so use dword stores.
    mov  ecx, edx
    mov  eax, [fs:edi + 32]               ; reload filesz
    sub  ecx, eax                         ; ECX = bss bytes
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
