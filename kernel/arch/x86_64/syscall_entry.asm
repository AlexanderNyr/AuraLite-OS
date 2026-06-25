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
global syscall_saved_rcx
global syscall_saved_r11
global syscall_saved_rsp
syscall_saved_rcx:   dq 0      ; user return RIP (saved by CPU in RCX)
syscall_saved_r11:   dq 0      ; user RFLAGS (saved by CPU in R11)
syscall_saved_rsp:   dq 0      ; user RSP (saved manually, for fork())

section .text
extern syscall_dispatch
extern syscall_restore_user_frame
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
    ; CPU set: RCX=user RIP, R11=user RFLAGS.  RSP is still the user stack.
    ;
    ; We publish the user RCX/R11/RSP in three globals so do_fork() can read
    ; them.  A nested syscall from a thread the scheduler switches to during
    ; our call would otherwise overwrite those globals, so syscall_dispatch
    ; copies them into the current TCB's saved_user_* fields BEFORE doing
    ; any work that could yield; on our way back out we restore RCX/R11
    ; from the TCB rather than from the globals.
    mov [rel syscall_saved_rcx], rcx
    mov [rel syscall_saved_r11], r11
    mov [rel syscall_saved_rsp], rsp

    ; Stash all SYSCALL arg registers on the stack (in reverse order so the
    ; SysV slots line up neatly).  After these pushes:
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

    ; Restore the GLOBAL syscall_saved_rcx/r11 from this thread's TCB.  This
    ; matters when another thread issued its own syscall while we were
    ; blocked inside syscall_dispatch (e.g. wait4 yielding) and clobbered
    ; the globals.  syscall_restore_user_frame() takes no args, returns void,
    ; and uses the C ABI (so we must keep RSP 16-byte aligned for the call).
    ; We stash our syscall return value in R12 across the call (R12 is
    ; callee-saved).
    mov  r12, rax
    sub  rsp, 8                             ; align to 16 (user rsp ≡ 8 mod 16)
    call syscall_restore_user_frame
    add  rsp, 8
    mov  rax, r12

    mov rcx, [rel syscall_saved_rcx]
    mov r11, [rel syscall_saved_r11]
    o64 sysret
