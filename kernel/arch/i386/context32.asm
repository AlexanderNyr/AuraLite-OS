; =============================================================================
; kernel/arch/i386/context32.asm -- kernel-thread context switch
; (I386_PLAN I4).  Sibling of kernel/proc/context.asm one width down.
;
; void context_switch32(uint32_t *save_esp, uint32_t new_esp);
;
; cdecl: [esp+4] = &old->esp slot, [esp+8] = new thread's saved ESP.
;
; The saved context is deliberately minimal and matches what
; thread32_create fabricates on a fresh stack:
;     [ebp] [edi] [esi] [ebx] [eflags] [ret-eip]
; EAX/ECX/EDX are caller-saved under cdecl, so the C caller already
; spilled anything it needed -- same reasoning context.asm applies to
; the SysV AMD64 set (rbx/rbp/r12-r15 only).
; =============================================================================

bits 32

section .text
global context_switch32
context_switch32:
    mov  eax, [esp + 4]         ; save slot
    mov  edx, [esp + 8]         ; new esp

    pushfd
    push ebx
    push esi
    push edi
    push ebp

    mov  [eax], esp             ; old->esp = current stack top

    mov  esp, edx               ; switch stacks

    pop  ebp
    pop  edi
    pop  esi
    pop  ebx
    popfd
    ret                          ; into the new thread's saved eip
