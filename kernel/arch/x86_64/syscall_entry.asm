; =============================================================================
; syscall_entry.asm — SYSCALL handler entry point.
;
; SYSCALL convention: rax=sysno, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
; C ABI:             rdi=arg1, rsi=arg2, rdx=arg3, rcx=arg4, r8=arg5, r9=arg6
;
; SECURITY MODEL:
;   - SYSCALL itself does not switch stacks, so we immediately capture the
;     userspace RSP and switch onto a per-thread kernel stack published by
;     set_syscall_stack().
;   - This avoids running kernel code on attacker-controlled userspace stack
;     memory and is a prerequisite for stronger SMAP-style hardening later.
; =============================================================================

bits 64
default rel

section .data
align 8
global syscall_saved_rcx
global syscall_saved_r11
global syscall_saved_rsp
global syscall_kernel_rsp
syscall_saved_rcx:   dq 0      ; user return RIP (saved by CPU in RCX)
syscall_saved_r11:   dq 0      ; user RFLAGS (saved by CPU in R11)
syscall_saved_rsp:   dq 0      ; user RSP (saved manually, for fork())
syscall_kernel_rsp:  dq 0      ; per-thread kernel stack top (published on switch)

section .text
extern syscall_dispatch
extern syscall_restore_user_frame
extern syscall_check_signals
global syscall_init
global syscall_entry
global set_syscall_stack

%define MSR_STAR   0xC0000081
%define MSR_LSTAR  0xC0000082
%define MSR_FMASK  0xC0000084
%define MSR_EFER   0xC0000080

; Publish the kernel stack top to use on SYSCALL entry.
;   rdi = stack_top
set_syscall_stack:
    mov [rel syscall_kernel_rsp], rdi
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

    ; FMASK: clear sensitive flags on SYSCALL entry.
    ;   TF  0x00000100  single-step
    ;   IF  0x00000200  interrupt-enable
    ;   DF  0x00000400  string direction
    ;   NT  0x00004000  nested task
    ;   RF  0x00010000  resume flag
    ;   AC  0x00040000  alignment-check / SMAP override bit
    mov ecx, MSR_FMASK
    mov eax, 0x00054700
    xor edx, edx
    wrmsr

    ; Enable SCE in EFER.
    mov ecx, MSR_EFER
    rdmsr
    or eax, 1
    wrmsr
    ret

syscall_entry:
    ; CPU set: RCX=user RIP, R11=user RFLAGS.  RSP is still the user stack.
    ; Capture the full userspace return frame, then switch immediately to the
    ; published kernel stack so no kernel work runs on attacker-controlled user
    ; memory.
    mov [rel syscall_saved_rcx], rcx
    mov [rel syscall_saved_r11], r11
    mov [rel syscall_saved_rsp], rsp
    mov rsp, [rel syscall_kernel_rsp]

    ; Stash all SYSCALL arg registers on the KERNEL stack (in reverse order so
    ; the SysV slots line up neatly). After these pushes:
    ;   [rsp+0]  = num (rax)
    ;   [rsp+8]  = a1  (rdi)
    ;   [rsp+16] = a2  (rsi)
    ;   [rsp+24] = a3  (rdx)
    ;   [rsp+32] = a4  (r10)
    ;   [rsp+40] = a5  (r8)
    ;   [rsp+48] = a6  (r9)
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax

    ; Reload into C ABI registers.  rsp+0 = num.
    mov  rdi, [rsp + 0]    ; num
    mov  rsi, [rsp + 8]    ; a1
    mov  rdx, [rsp + 16]   ; a2
    mov  rcx, [rsp + 24]   ; a3
    mov  r8 , [rsp + 32]   ; a4
    mov  r9 , [rsp + 40]   ; a5
    ; The 7th arg (a6) must live on the stack at [rsp] when call executes.
    mov  rax, [rsp + 48]
    push rax               ; [rsp]  = a6  (7th C arg)
    sub  rsp, 8            ; keep 16-byte alignment for `call`

    cld
    call syscall_dispatch

    add  rsp, 16           ; drop alignment pad + a6
    add  rsp, 7*8          ; drop the 7 pushed sources

    ; Restore the GLOBAL syscall_saved_rcx/r11/rsp from this thread's TCB.
    ; IMPORTANT: preserve user callee-saved registers such as R12.
    push r12
    mov  r12, rax
    call syscall_restore_user_frame

    ; Signal delivery slow path: if a signal is pending, syscall_check_signals
    ; builds a handler frame and returns to user via IRETQ (never returns here).
    ; It takes the syscall return value (saved in r12) as its argument.
    mov  rdi, r12
    call syscall_check_signals

    mov  rax, r12
    pop  r12

    mov rcx, [rel syscall_saved_rcx]
    mov r11, [rel syscall_saved_r11]
    mov rsp, [rel syscall_saved_rsp]
    o64 sysret
