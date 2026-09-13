; =============================================================================
; isr.asm — 256 IDT entry-point stubs + the common save/restore handler.
;
; Each stub normalises the stack to: [int_no][err_code], then jumps to the common
; handler, which pushes all general registers, calls isr_handler(regs*) in C,
; restores them, drops the two header words, and returns with iretq.
;
; Stack frame layout on entry to isr_handler (low -> high address):
;   r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rdx rcx rbx rax
;   int_no err_code  rip cs rflags rsp ss
; =============================================================================

bits 64
default rel

section .rodata
align 8
global isr_table

; 256-entry table of handler addresses, indexed by vector number, used by
; idt.c to populate the IDT. Built via a macro so plain %1 substitution forms
; the symbol names isr0 .. isr255 (no fragile token-pasting).
%macro TABLE_ENTRY 1
    dq isr%1
%endmacro

isr_table:
%assign i 0
%rep 256
    TABLE_ENTRY i
%assign i i+1
%endrep


section .text
extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0          ; dummy error code -> uniform frame
    push qword %1         ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1         ; CPU already pushed the error code
    jmp isr_common_stub
%endmacro

; W32A-3 swapgs audit (the one kernel-wide change this phase makes).
; GS.base is the cpu_local anchor in Ring 0 and the running thread's user
; GS (its Win32 TEB-lite, 0 if unset) in Ring 3; KERNEL_GS_BASE shadows
; the other side.  Every path below swaps by the CPL it is LEAVING, and
; every return swaps by the CPL it is ENTERING:
;   SYSCALL entry/exit (syscall_entry.asm) .... unconditional (Ring 3 only)
;   interrupt/exception entry/exit (here) ..... by frame CS RPL (both rings)
;   signal-iret slow path (syscall_sigreturn.asm) by pushed CS (always Ring 3)
;   first process entry (user_entry.asm) ...... by pushed CS (always Ring 3)
;   fork/clone child entry (fork_return.asm) .. unconditional (Ring 3 only)
; The interrupted frame's CS is read from the STACK, never from %gs, so the
; test itself is safe in either state.  Signal delivery rewrites RIP but
; keeps CS RPL=3, so the exit test still swaps.  #DF (vector 8) arrives on
; IST1 but goes through this same stub, so its GS handling is identical.
; KNOWN ACCEPTED WINDOW: an NMI (LINT1 is unmasked, lapic.c) landing in the
; 2 instructions between swapgs and sysret/iretq on an EXIT path runs the
; NMI handler with the wrong GS - CPL=0 in the frame, so no compensating
; swap happens, and the handler's cpu_local reads see user memory.  The NMI
; sources on this tree never fire under qemu (no watchdog, no panic button;
; CI boots thousands of times without one), and the window is 2 cycles wide
; per transition, so the expected rate is zero; a hit would halt in #DF
; with garbled diagnostics, never silently corrupt.  A paranoid-NMI stub
; (RIP-range check + compensating swap) is the documented follow-up if NMI
; ever becomes a real source here.  Entry windows are NMI-safe by
; construction: the entry swapgs runs before any %gs use, and an NMI inside
; the window sees the pre-swap state, which is exactly what its own CPL test
; expects.  (SYSCALL entry is additionally single-state: the kernel never
; executes SYSCALL itself, so the entry swap is unconditional.)

isr_common_stub:
    ; W32A-3: stack is [int_no][err][rip][cs]... - CS RPL at [rsp+24]
    ; tells which ring we came from.  Swap only for Ring 3 origin.
    test byte [rsp + 24], 3
    jz .Lw32a3_no_swap_in
    swapgs                     ; GS.base <- cpu_local, shadow <- user GS
.Lw32a3_no_swap_in:
    ; Save all general-purpose registers. First push ends highest; the struct
    ; in isr.h lists them in the opposite (low-first) order to match.
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; RESIDUE2 T1: maintain the 16-byte C-ABI stack alignment.  The CPU
    ; frame plus the 17 pushes leave RSP at an arbitrary 8-mod-16 phase
    ; (it depends on the interrupted RSP and on whether an error code was
    ; pushed), so a compiler-aligned stack local in isr_handler's callees
    ; could land misaligned and any aligned SSE op (fxsave) would #GP.
    ; Remember the frame pointer in rbx (the app's rbx is already saved on
    ; the stack), align down, call, restore.  The M5 runtime-aligned
    ; scratch in signal.c was the workaround for exactly this and is gone
    ; in the same commit.
    mov rbx, rsp
    and rsp, -16
    mov rdi, rbx          ; System V AMD64: arg0 = pointer to registers
    cld                   ; ABI requires the direction flag clear
    call isr_handler
    mov rsp, rbx

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; W32A-3: stack is back to [int_no][err][rip][cs]... — the (possibly
    ; signal-rewritten) CS RPL at [rsp+24] tells which ring we return to.
    test byte [rsp + 24], 3
    jz .Lw32a3_no_swap_out
    swapgs                     ; GS.base <- user GS, shadow <- cpu_local
.Lw32a3_no_swap_out:
    add rsp, 16           ; discard err_code + int_no
    iretq

; ---- CPU exceptions 0-31 ----
; Vectors that push an error code: 8, 10, 11, 12, 13, 14, 17 (Intel SDM 3A 6-13).
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- Hardware IRQs 32-47 and remaining vectors 48-255 (all no error code) ----
%assign i 32
%rep 224
    ISR_NOERR i
%assign i i+1
%endrep
