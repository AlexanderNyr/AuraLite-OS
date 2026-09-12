; w32/tests/ordbadnum.asm — W32A-1 fixture: an ordinal NOBODY maps.
; 9999 has no row in w32/ordinal_map.tsv, so the binder cannot name it and
; must refuse BY NUMBER: "w32run: unresolved import COMCTL32.dll!#9999".
; start must never run; reaching it prints ORD-FAIL and exits 3.

bits 64
default rel

extern NoSuchOrd
extern ExitProcess

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h
    call NoSuchOrd
    mov ecx, 3
    call ExitProcess
