; w32/tests/datadll.asm — W32A-1 fixture: exports a DWORD and a function.
bits 64
default rel

section .text

global DllMain
DllMain:
    mov eax, 1
    ret

global data_fn
data_fn:
    mov eax, 7
    ret

section .data
global answer
answer: dd 42
