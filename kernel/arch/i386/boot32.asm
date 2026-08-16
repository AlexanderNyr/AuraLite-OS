; =============================================================================
; kernel/arch/i386/boot32.asm -- i386 kernel entry point (I386_PLAN I2,
; higher-half since I3).
;
; Stage 2's enter_prot32 jumps to _start in 32-bit protected mode with
; paging OFF and ESI = boot_info_t phys.  Since I3 the kernel is linked
; at 0xC0100000 (KERNEL_VMA + 1 MiB) but loaded at 0x00100000, so entry
; happens in the .boot section (VMA = LMA, see kernel32.ld) which:
;
;   1. Builds the boot page directory with PSE 4 MiB pages:
;        PDE[0..223]    identity  [0, 896 MiB)   -- survives the PG flip
;        PDE[768..991]  direct map [0, 896 MiB) at 0xC0000000
;      Zero page tables needed -- this is why check_i686 requires PSE.
;   2. CR3 := page directory, CR4.PSE := 1, CR0.PG|WP := 1.
;   3. Jumps to the higher half (indirect -- the linker resolved the
;      target at its VMA).
;
; The higher-half half then zeroes .bss, sets the real stack and calls
; kmain32(ESI) exactly as before.  The identity window is dropped by
; paging32_drop_identity() once kmain32 is running -- it exists only so
; the instruction after `mov cr0` fetches successfully.
; =============================================================================

bits 32

KERNEL_VMA     equ 0xC0000000
DIRECT_MAP_MB  equ 896
PDE_COUNT      equ DIRECT_MAP_MB / 4          ; 224 4-MiB PDEs

; PDE bits: P | RW | PS  (kernel-only, global left off until PGE audit)
PDE_FLAGS      equ (1 << 0) | (1 << 1) | (1 << 7)

; -----------------------------------------------------------------------------
; .boot -- physical-mode entry (VMA = LMA = 0x00100000 region)
; -----------------------------------------------------------------------------
section .boot
align 4096
global boot_page_directory
boot_page_directory:
    times 4096 db 0

global _start
_start:
    cli
    ; ESI = boot_info phys; nothing below may clobber it.

    ; ---- 1. Fill the page directory (runs at physical addresses). ----
    ; Identity PDEs 0..223 and direct-map PDEs 768..991 share frames:
    ; entry i maps phys i*4MiB.
    mov  edi, boot_page_directory
    xor  ecx, ecx                       ; ecx = PDE index 0..223
.fill_pde:
    mov  eax, ecx
    shl  eax, 22                        ; phys = index * 4 MiB
    or   eax, PDE_FLAGS
    mov  [edi + ecx*4], eax             ; identity
    mov  [edi + (ecx + 768)*4], eax     ; +0xC0000000
    inc  ecx
    cmp  ecx, PDE_COUNT
    jb   .fill_pde

    ; ---- 2. Turn paging on. ----
    mov  eax, boot_page_directory
    mov  cr3, eax

    mov  eax, cr4
    or   eax, (1 << 4)                  ; CR4.PSE
    mov  cr4, eax

    mov  eax, cr0
    or   eax, (1 << 31) | (1 << 16)     ; CR0.PG | CR0.WP
    mov  cr0, eax

    ; ---- 3. To the higher half.  Indirect jump: the target symbol
    ; lives in .text, so the linker resolved it at its VMA. ----
    mov  eax, higher_half
    jmp  eax

; -----------------------------------------------------------------------------
; .text -- the higher-half kernel proper
; -----------------------------------------------------------------------------
section .text
extern kmain32
extern __bss_start
extern __bss_end

STACK_SIZE equ 64*1024

higher_half:
    ; Zero .bss (VMA addresses; the direct map backs them now).
    ; ESI survives: rep stosd touches EDI/ECX/EAX only.
    mov  edi, __bss_start
    mov  ecx, __bss_end
    sub  ecx, edi
    shr  ecx, 2
    xor  eax, eax
    rep  stosd

    mov  esp, stack_top

    push esi                            ; cdecl: kmain32(boot_info_phys)
    call kmain32

.hang:
    hlt
    jmp  .hang

section .bss
align 16
stack_bottom:
    resb STACK_SIZE
stack_top:
