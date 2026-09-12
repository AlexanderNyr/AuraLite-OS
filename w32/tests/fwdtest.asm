; w32/tests/fwdtest.asm — W32A-1 fixture: the forwarding DLL.
; A forwarder has no code behind it -- the .def alone creates the export
; entry -- so this object carries only DllMain.
bits 64
default rel

section .text

global DllMain
DllMain:
    mov eax, 1
    ret
