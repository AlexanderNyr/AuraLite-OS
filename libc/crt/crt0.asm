; =============================================================================
; crt0.asm — C runtime entry point for AuraLite OS user programs.
;
; The kernel loads the ELF and jumps to _start with RSP pointing at the
; System V AMD64 initial process stack:
;
;     [rsp +  0]   argc
;     [rsp +  8]   argv[0]
;       ...        argv[argc-1]
;                  NULL                 (argv terminator)
;                  envp[0..]            environment pointers
;                  NULL                 (envp terminator)
;                  auxv ... AT_NULL
;
; We pull argc/argv/envp off the stack, hand them to __libc_start_main (which
; sets `environ`, runs main, then exit()s with its return value). For older
; kernels that jump in with a bare 16-aligned stack and argc==0 this still
; behaves correctly (main sees argc=0, argv/envp empty).
; =============================================================================

bits 64
default rel

section .text
global _start
extern __libc_start_main
extern main
extern __stack_chk_guard

_start:
    ; --- Per-process stack protector seed ------------------------------------
    ; Derive a per-instance cookie from the initial stack pointer and a fixed
    ; constant. Do this before we disturb RSP.
    lea rax, [rel __stack_chk_guard]
    xor rax, rsp
    mov rdx, 0x6B1F2D4CA53E9071
    xor rax, rdx
    mov [rel __stack_chk_guard], rax

    xor rbp, rbp                 ; clear frame pointer (ABI requirement)

    ; --- Decode the initial stack --------------------------------------------
    mov rdi, [rsp]               ; rdi = argc
    lea rsi, [rsp + 8]           ; rsi = argv  (&argv[0])
    ; envp = argv + (argc + 1)  (skip argv[] and its NULL terminator)
    lea rdx, [rsp + 8]           ; rdx = &argv[0]
    lea rdx, [rdx + rdi*8]       ; rdx = &argv[argc]  (the NULL slot)
    add rdx, 8                   ; rdx = &argv[argc+1] = envp

    ; Re-align RSP to 16 bytes for the C ABI before calling into C. The kernel
    ; lays the frame out so argc sits 16-aligned; the call below pushes a
    ; return address (RSP%16 becomes 8), which is exactly what the callee ABI
    ; expects on entry.
    and rsp, -16
    call __libc_start_main       ; (argc, argv, envp) -> does not return

    ; __libc_start_main never returns. If it somehow does, trap.
    ud2
