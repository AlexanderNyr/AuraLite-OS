; w32a4_purecall.asm — _purecall.  W32A-4.
;
; Calls the imported msvcrt _purecall directly: W32-PURECALL on stderr,
; exit 99.  (Companion to w32a4_term.asm; see the note there.)

bits 64
default rel

extern _purecall

section .text
global start
start:
    sub  rsp, 28h
    call _purecall               ; NORETURN
    hlt
start_end:

section .pdata
    dd start, start_end, start_xdata

section .xdata
start_xdata:
    db 0x01, 4, 1, 0x00
    db 4, 0x42                  ; ALLOC_SMALL(4): sub rsp,0x28
    db 0, 0
