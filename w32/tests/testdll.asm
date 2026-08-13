; w32/tests/testdll.asm — WIN32_PLAN.md phase W32-7 fixture.
;
; A real user-supplied DLL: exported functions, its own import of a
; KERNEL32 function, and a DllMain that observes attach and detach.
;
; It imports WriteFile itself so the gate proves that a LOADED DLL's own
; imports are bound -- not just the main image's.  A loader that mapped the
; DLL but skipped its import table would pass a test that only called a leaf
; function.
;
; Stack discipline, the recurring lesson from W32-5 and W32-6: Windows x64
; wants RSP % 16 == 0 at every CALL, entry leaves it at 8, and R12/RBX are
; callee-saved.  Every frame below is annotated.

bits 64
default rel

extern GetStdHandle
extern WriteFile

section .text

; DllMain(hinst, reason, reserved) -- must return non-zero to load.
; 2 pushes + 0x38 = 72 -> (8-72) % 16 == 0.  OK.
global DllMain
DllMain:
    push rbx
    push r12
    sub  rsp, 38h

    mov  ebx, edx                  ; reason

    cmp  ebx, 1                    ; DLL_PROCESS_ATTACH
    jne  .not_attach
    mov  byte [attached], 1
    lea  rcx, [msg_attach]
    call dll_puts
    jmp  .done

.not_attach:
    cmp  ebx, 0                    ; DLL_PROCESS_DETACH
    jne  .done
    lea  rcx, [msg_detach]
    call dll_puts

.done:
    mov  eax, 1                    ; TRUE: initialised
    add  rsp, 38h
    pop  r12
    pop  rbx
    ret

; dll_puts(rcx = NUL-terminated string).  Uses the DLL's OWN import of
; WriteFile, which only works if the loader bound this image's imports.
; 2 pushes + 0x38 = 72 -> OK.
dll_puts:
    push rbx
    push r12
    sub  rsp, 38h
    mov  rbx, rcx

    xor  eax, eax
.len:
    cmp  byte [rbx + rax], 0
    je   .got
    inc  rax
    jmp  .len
.got:
    mov  r12d, eax

    mov  ecx, -11
    call GetStdHandle

    mov  rcx, rax
    mov  rdx, rbx
    mov  r8d, r12d
    lea  r9,  [rsp + 30h]
    mov  qword [rsp + 20h], 0
    call WriteFile

    add  rsp, 38h
    pop  r12
    pop  rbx
    ret

; --- the exported functions ------------------------------------------------

; int dll_add(int a, int b) -- a leaf, to prove a returned pointer is callable.
global dll_add
dll_add:
    mov  eax, ecx
    add  eax, edx
    ret

; int dll_speak(void) -- calls back out through the DLL's own import, so the
; gate can tell that imports were bound rather than merely that code ran.
; 1 push + 0x30 = 56 -> (8-56) % 16 == 0.  OK.
global dll_speak
dll_speak:
    push rbx
    sub  rsp, 30h
    lea  rcx, [msg_speak]
    call dll_puts
    mov  eax, 1
    add  rsp, 30h
    pop  rbx
    ret

; int dll_was_attached(void) -- reports whether DllMain ran before any export
; was called, which is the ordering the loader must honour.
global dll_was_attached
dll_was_attached:
    movzx eax, byte [attached]
    ret

section .rdata align=16
msg_attach db "DLL-ATTACH", 10, 0
msg_detach db "DLL-DETACH", 10, 0
msg_speak  db "DLL-SPEAK", 10, 0

section .data align=8
attached db 0
