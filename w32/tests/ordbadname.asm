; w32/tests/ordbadname.asm — W32A-1 fixture: a name NOBODY exports.
; NoSuchFunction is neither a real export nor a generated stub, so the
; binder must refuse BY NAME: "w32run: unresolved import
; COMCTL32.dll!NoSuchFunction".  start must never run.

bits 64
default rel

extern NoSuchFunction
extern ExitProcess

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h
    call NoSuchFunction
    mov ecx, 3
    call ExitProcess
