; =============================================================================
; syscall_entry.asm — SYSCALL handler entry point.
;
; SYSCALL convention: rax=sysno, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
; C ABI:             rdi=arg1, rsi=arg2, rdx=arg3, rcx=arg4, r8=arg5, r9=arg6
;
; The dispatch runs on the USER's stack (SYSCALL does not switch stacks). This is
; safe because:
;   - The user stack is mapped writable + user-accessible.
;   - Timer/IRQ interrupts switch to the TSS.RSP0 kernel stack, which is
;     DIFFERENT from the user stack, so no conflict.
;   - iretq from the timer handler restores the user RSP.
; =============================================================================

bits 64
default rel

section .data
align 8
syscall_saved_rcx:   dq 0      ; user return RIP (saved by CPU in RCX)
syscall_saved_r11:   dq 0      ; user RFLAGS (saved by CPU in R11)

section .text
extern syscall_dispatch
global syscall_init
global syscall_entry
global set_syscall_stack

%define MSR_STAR   0xC0000081
%define MSR_LSTAR  0xC0000082
%define MSR_FMASK  0xC0000084
%define MSR_EFER   0xC0000080

; Placeholder — kept for API compatibility but does nothing now.
set_syscall_stack:
    ret

syscall_init:
    ; LSTAR = full 64-bit address of syscall_entry (EDX:EAX for WRMSR).
    mov ecx, MSR_LSTAR
    lea rax, [rel syscall_entry]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    ; STAR[47:32]=0x08 (SYSCALL -> CS=0x08, SS=0x10)
    ; STAR[63:48]=0x10 (SYSRET  -> CS=0x10+0x10=0x20|3=0x23, SS=0x10+0x08=0x18|3=0x1B)
    mov ecx, MSR_STAR
    xor eax, eax
    mov edx, 0x00100008
    wrmsr

    ; FMASK: clear IF (bit 9) on SYSCALL entry.
    mov ecx, MSR_FMASK
    mov eax, 0x200
    xor edx, edx
    wrmsr

    ; Enable SCE in EFER.
    mov ecx, MSR_EFER
    rdmsr
    or eax, 1
    wrmsr
    ret

syscall_entry:
    ; The CPU set: RCX=user RIP, R11=user RFLAGS.
    mov [rel syscall_saved_rcx], rcx
    mov [rel syscall_saved_r11], r11

    ; Remap SYSCALL args -> C ABI: insert sysno at front, shift the rest.
    ; in:  rax=sysno  rdi=a1  rsi=a2  rdx=a3  r10=a4  r8=a5  r9=a6
    ; out: rdi=sysno  rsi=a1  rdx=a2  rcx=a3
    push rdi          ; save a1
    push rsi          ; save a2
    push rdx          ; save a3

    mov rdi, rax      ; arg0 <- sysno
    mov rsi, [rsp+16] ; arg1 <- a1
    mov rdx, [rsp+8]  ; arg2 <- a2
    mov rcx, [rsp+0]  ; arg3 <- a3

    cld
    call syscall_dispatch

    add rsp, 24       ; pop the 3 saved values

    mov rcx, [rel syscall_saved_rcx]   ; restore return RIP
    mov r11, [rel syscall_saved_r11]   ; restore RFLAGS
    o64 sysret                          ; 64-bit SYSRET (48 0F 07):
                                        ; CS = STAR[63:48]+0x10|RPL3 = 0x1B
