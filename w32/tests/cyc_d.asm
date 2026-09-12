; w32/tests/cyc_d.asm — W32A-1 fixture: cyc_d.dll imports cyc_c_fn from
; cyc_c.dll, closing the cycle.  See cyc_c.asm.
bits 64
default rel

extern cyc_c_fn

section .text

global DllMain
DllMain:
    mov eax, 1
    ret

global cyc_d_fn
cyc_d_fn:
    push rbx
    sub rsp, 20h
    call cyc_c_fn
    inc eax
    add rsp, 20h
    pop rbx
    ret
