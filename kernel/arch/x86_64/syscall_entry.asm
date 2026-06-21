; =============================================================================
; syscall_entry.asm — SYSCALL handler entry point.
;
; SYSCALL convention: rax=sysno, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
; C ABI:             rdi=arg1, rsi=arg2, rdx=arg3, rcx=arg4, r8=arg5, r9=arg6
;
; The dispatch function signature is:
;   uint64_t syscall_dispatch(uint64_t num, uint64_t a1, ..., uint64_t a6);
; So we need: rdi=num(rax), rsi=a1(rdi), rdx=a2(rsi), rcx=a3(rdx), r8=a5, r9=a6
;
; On SYSCALL: RCX = return RIP, R11 = saved RFLAGS (preserve for SYSRET).
; =============================================================================

bits 64
default rel

section .data
align 8
syscall_saved_rcx: dq 0
syscall_saved_r11: dq 0

section .text
extern syscall_dispatch
global syscall_init
global syscall_entry

%define MSR_STAR   0xC0000081
%define MSR_LSTAR  0xC0000082
%define MSR_FMASK  0xC0000084
%define MSR_EFER   0xC0000080

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

    ; Enable SCE (syscall/sysret) in EFER.
    mov ecx, MSR_EFER
    rdmsr
    or eax, 1
    wrmsr
    ret

syscall_entry:
    ; Save RCX (return RIP) and R11 (saved RFLAGS) before the C call clobbers them.
    mov [rel syscall_saved_rcx], rcx
    mov [rel syscall_saved_r11], r11

    ; ---- Remap registers: insert rax(sysno) at front, shift the rest ----
    ; in:  rax=sysno  rdi=a1  rsi=a2  rdx=a3  r10=a4  r8=a5  r9=a6
    ; out: rdi=sysno  rsi=a1  rdx=a2  rcx=a3  r8=a5   r9=a6
    ; Strategy: push rdi,rsi,rdx (the values that must shift), then reload.
    push rdi          ; save a1
    push rsi          ; save a2
    push rdx          ; save a3

    mov rdi, rax      ; arg0 <- sysno
    mov rsi, [rsp+16] ; arg1 <- a1 (was rdi, at rsp+16)
    mov rdx, [rsp+8]  ; arg2 <- a2 (was rsi, at rsp+8)
    mov rcx, [rsp+0]  ; arg3 <- a3 (was rdx, at rsp+0)
    ; r8 and r9 are already a5 and a6 — no change needed.

    cld
    call syscall_dispatch

    add rsp, 24       ; pop the 3 saved values (3 * 8)

    mov rcx, [rel syscall_saved_rcx]   ; restore return RIP
    mov r11, [rel syscall_saved_r11]   ; restore RFLAGS
    sysret                              ; NASM mnemonic for SYSRET (0F 07)
