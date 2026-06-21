; =============================================================================
; syscall_entry.asm — SYSCALL handler entry point.
;
; CRITICAL: SYSCALL does NOT switch stacks (unlike interrupts). The kernel
; handler runs on the USER's RSP, which would corrupt the user's stack data.
; So we manually switch to a kernel stack at entry, and restore the user RSP
; before SYSRET.
;
; SYSCALL convention: rax=sysno, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
; C ABI:             rdi=arg1, rsi=arg2, rdx=arg3, rcx=arg4, r8=arg5, r9=arg6
; =============================================================================

bits 64
default rel

section .data
align 8
syscall_saved_rcx:   dq 0      ; user return RIP (saved by CPU in RCX)
syscall_saved_r11:   dq 0      ; user RFLAGS (saved by CPU in R11)
syscall_saved_rsp:   dq 0      ; user RSP (we save it manually)
syscall_kstack:      dq 0      ; kernel stack top for syscall processing

section .text
extern syscall_dispatch
global syscall_init
global syscall_entry
global set_syscall_stack

%define MSR_STAR   0xC0000081
%define MSR_LSTAR  0xC0000082
%define MSR_FMASK  0xC0000084
%define MSR_EFER   0xC0000080

; Called by the kernel before jumping to user mode: sets the kernel stack
; that syscall_entry will switch to. C prototype: void set_syscall_stack(uint64_t).
set_syscall_stack:
    mov [rel syscall_kstack], rdi
    ret

syscall_init:
    ; LSTAR = full 64-bit address of syscall_entry (EDX:EAX for WRMSR).
    mov ecx, MSR_LSTAR
    lea rax, [rel syscall_entry]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    ; STAR[47:32]=0x08 (SYSCALL -> CS=0x08, SS=0x10)
    ; STAR[63:48]=0x08 (SYSRET  -> CS=0x18|3=0x1B, SS=0x10|3=0x13)
    mov ecx, MSR_STAR
    xor eax, eax
    mov edx, 0x00080008
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
    ; The CPU set: RCX=user RIP, R11=user RFLAGS. RSP is still the USER's stack.
    mov [rel syscall_saved_rcx], rcx
    mov [rel syscall_saved_r11], r11
    mov [rel syscall_saved_rsp], rsp     ; save user RSP

    ; Switch to the kernel stack so our C call doesn't clobber user data.
    mov rsp, [rel syscall_kstack]

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

    ; Restore the user RSP (RAX holds the syscall return value).
    mov rsp, [rel syscall_saved_rsp]
    mov rcx, [rel syscall_saved_rcx]     ; restore return RIP
    mov r11, [rel syscall_saved_r11]     ; restore RFLAGS
    o64 sysret                            ; 64-bit operand SYSRET (48 0F 07):
                                         ; CS = STAR[63:48]+0x10|RPL3 = 0x1B
