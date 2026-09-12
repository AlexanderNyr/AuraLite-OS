; w32/tests/cyc_c.asm — W32A-1 fixture: cyc_c.dll imports cyc_d_fn from
; cyc_d.dll, which imports cyc_c_fn back.  The loader must refuse with the
; cycle named; no code here ever runs.
bits 64
default rel

extern cyc_d_fn

section .text

global DllMain
DllMain:
    mov eax, 1
    ret

global cyc_c_fn
cyc_c_fn:
    push rbx
    sub rsp, 20h
    call cyc_d_fn
    inc eax
    add rsp, 20h
    pop rbx
    ret
