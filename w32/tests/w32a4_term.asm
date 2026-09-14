; w32a4_term.asm — the named C++ terminations.  W32A-4.
;
; Calls the imported msvcrt ?terminate@@YAXXZ directly (the D7 surface is
; real exports, not stubs): the process prints W32-CXX-TERMINATE and exits
; 99.  _purecall gets its own binary (a terminate call never returns, so
; one binary cannot cover both).

bits 64
default rel

extern ?terminate@@YAXXZ

section .text
global start
start:
    sub  rsp, 28h
    call ?terminate@@YAXXZ      ; NORETURN
    hlt
start_end:

section .pdata
    dd start, start_end, start_xdata

section .xdata
start_xdata:
    db 0x01, 4, 1, 0x00
    db 4, 0x42                  ; ALLOC_SMALL(4): sub rsp,0x28
    db 0, 0
