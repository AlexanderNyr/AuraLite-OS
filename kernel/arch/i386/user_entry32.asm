; =============================================================================
; kernel/arch/i386/user_entry32.asm -- iret into Ring 3 (I386_PLAN I4).
; Sibling of kernel/proc/user_entry.asm one width down.
;
; void user32_enter(uint32_t entry, uint32_t user_esp);   // cdecl
;
; Builds the 5-dword inter-privilege iret frame:
;     [ss=0x23] [esp] [eflags IF=1] [cs=0x1B] [eip]
; and drops to Ring 3.  Data segments are loaded with the user selector
; BEFORE the iret (they would fault inside Ring 3 otherwise).  Never
; returns; the way back is int 0x80 -> SYS32_EXIT.
; =============================================================================

bits 32

USER_CS equ 0x1B                ; selector 0x18 | RPL 3
USER_DS equ 0x23                ; selector 0x20 | RPL 3

section .text
global user32_enter
user32_enter:
    mov  ecx, [esp + 4]         ; entry
    mov  edx, [esp + 8]         ; user esp

    mov  ax, USER_DS
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    push dword USER_DS          ; ss
    push edx                    ; esp
    push dword 0x202            ; eflags: IF | reserved bit 1
    push dword USER_CS          ; cs
    push ecx                    ; eip
    iretd
