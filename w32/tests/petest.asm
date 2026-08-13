; petest.asm — a real PE32+ .exe for the W32-3 kernel-loader gate.
;
; WIN32_PLAN.md phase W32-3 needs a genuine PE image to load, and it must be
; built in-tree so the gate depends on no downloaded binary and no toolchain
; beyond what REQUIRED_TOOLS already lists: `nasm -f win64` produces a COFF
; object and `lld-link` produces the PE, both already used to build BOOTX64.EFI.
;
; The program is freestanding: it imports nothing, links nothing, and talks to
; the kernel with the AuraLite syscall ABI directly.  That is deliberate for
; this phase.  W32-3 proves the *loader* works -- sections mapped, permissions
; applied, control transferred -- and nothing else.  Imports are W32-4's
; problem, and a test that needed KERNEL32 could not run until then.
;
; It writes a marker to stdout and exits with a distinctive status so the
; integration test can tell "the loader worked" from "something else printed".

bits 64
default rel

%define SYS_WRITE 1
%define SYS_EXIT  60

section .rdata
marker:     db "W32-PE-LOADER-OK", 10
marker_len  equ $ - marker

section .data
; A pointer that only has the right value if base relocations were applied
; (or if the image loaded at its preferred base).  Reading through it proves
; the relocation path did not corrupt the image.
selfptr:    dq marker

section .text
global start
start:
    ; write(1, marker, marker_len) -- reached through selfptr, not directly,
    ; so a broken relocation shows up as garbage or a fault rather than
    ; passing silently.
    mov     rax, SYS_WRITE
    mov     rdi, 1
    mov     rsi, [selfptr]
    mov     rdx, marker_len
    syscall

    ; exit(77)
    mov     rax, SYS_EXIT
    mov     rdi, 77
    syscall

.hang:
    jmp     .hang
