; =============================================================================
; syscall_entry.asm — SYSCALL handler entry point.
;
; SYSCALL convention: rax=sysno, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
; C ABI:             rdi=arg1, rsi=arg2, rdx=arg3, rcx=arg4, r8=arg5, r9=a6
;
; SECURITY MODEL:
;   - SYSCALL itself does not switch stacks, so we immediately capture the
;     userspace RSP and switch onto a per-thread kernel stack published by
;     set_syscall_stack().
;   - This avoids running kernel code on attacker-controlled userspace stack
;     memory and is a prerequisite for stronger SMAP-style hardening later.
;
; SMP MODEL:
;   - All entry/exit state lives in the per-CPU struct cpu_local slots,
;     reached through %gs (each CPU's GS.base points at ITS cpu_local, set by
;     cpu_local_init(); nothing ever changes GS.base after that, and userland
;     TLS uses FS, so %gs is valid here in both rings).  The offsets come
;     from tools/gen_asm_offsets.c via build/asm_offsets.inc, so they can
;     never drift out of sync with the C struct.
;   - The old implementation kept this state in .data globals -- correct
;     only while one CPU could enter the kernel at a time.  With real SMP
;     two CPUs can run syscalls concurrently, and those globals would tear.
; =============================================================================

bits 64
default rel

; CL_SYS_* offsets are auto-generated from struct cpu_local via
; tools/gen_asm_offsets.c so this file never drifts out of sync with the C
; struct layout.
%include "asm_offsets.inc"

section .text
extern syscall_dispatch
extern syscall_restore_user_frame
extern syscall_check_signals
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
    ; Capture the full userspace return frame into THIS cpu's cpu_local slots
    ; (interrupts are masked by FMASK, so nothing can preempt us mid-capture),
    ; then switch immediately to the published kernel stack so no kernel work
    ; runs on attacker-controlled user memory.
    mov [gs:CL_SYS_RIP], rcx
    mov [gs:CL_SYS_RFLAGS], r11
    mov [gs:CL_SYS_RSP], rsp
    ; Capture the live user callee-saved registers before any kernel code runs,
    ; so a signal delivered at syscall exit can faithfully restore them.
    mov [gs:CL_SYS_RBX], rbx
    mov [gs:CL_SYS_RBP], rbp
    mov [gs:CL_SYS_R12], r12
    mov [gs:CL_SYS_R13], r13
    mov [gs:CL_SYS_R14], r14
    mov [gs:CL_SYS_R15], r15
    mov rsp, [gs:CL_SYS_KRSP]

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
    ; The 7th C arg (a6) must be at [rsp] before CALL so the callee sees it
    ; at [rsp+8] after the return address is pushed.  The seven saved SYSCALL
    ; registers leave RSP 8 mod 16, so this single slot also restores the
    ; required 16-byte pre-call alignment.
    mov  rax, [rsp + 48]
    sub  rsp, 8
    mov  [rsp], rax

    cld
    call syscall_dispatch

    add  rsp, 8            ; drop a6 stack slot
    add  rsp, 7*8          ; drop the 7 pushed sources

    ; Refresh THIS cpu's per-CPU syscall slots from this thread's TCB (the
    ; thread may have been preempted mid-syscall and resumed HERE after
    ; another CPU ran its own syscalls).  IMPORTANT: preserve user
    ; callee-saved registers such as R12.
    ; RESIDUE2 T1 (stub-alignment box): after the 64-byte teardown above RSP
    ; is 16-byte aligned; the bare `push r12` flipped it to 8 mod 16, so BOTH
    ; C calls below entered their callees with a misaligned stack.  Every
    ; frame below then inherits the skew and build_handler_frame()'s
    ; `aligned(16)` signal frame lands 8-off -> `fxsave` #GPs on the first
    ; cross-process signal delivery.  Pad the window so the SysV pre-call
    ; alignment (RSP % 16 == 0) holds at both calls.
    push r12
    sub  rsp, 8
    mov  r12, rax
    call syscall_restore_user_frame

    ; Signal delivery slow path: if a signal is pending, syscall_check_signals
    ; builds a handler frame and returns to user via IRETQ (never returns here).
    ; It takes the syscall return value (saved in r12) as its argument.
    mov  rdi, r12
    call syscall_check_signals

    add  rsp, 8
    mov  rax, r12
    pop  r12

    mov rcx, [gs:CL_SYS_RIP]
    mov r11, [gs:CL_SYS_RFLAGS]
    mov rsp, [gs:CL_SYS_RSP]
    o64 sysret
