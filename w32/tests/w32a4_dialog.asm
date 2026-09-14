; w32a4_dialog.asm — the unhandled-exception dialog.  W32A-4.
;
; Creates a visible window (the unwinder's GUI-or-console rule: the modal
; box shows only when this process owns a live window), prints
; W32A4-DIALOG-ARMED, then divides by zero unguarded.  The GUI gate
; screenshots the box, presses Enter, and asserts the exit code.
; A console run of this binary shows no box (no compositor session owns
; the window... precisely: the box shows, off-screen, and waits -- which
; is why the gate always runs it under VNC).

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern RegisterClassExA
extern CreateWindowExA
extern DefWindowProcA

%define STD_OUTPUT_HANDLE -11
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define WS_VISIBLE  0x10000000

section .rdata
cls_name:  db "W32A4DIALOG", 0
win_title: db "W32A4 crash test", 0
msg_armed: db "W32A4-DIALOG-ARMED", 10
msg_armed_l equ $ - msg_armed

section .bss
written:  resq 1
stdout_h: resq 1
hwnd:     resq 1
wc:       resb 80

section .text

puts_raw:
    push rbp
    mov  rbp, rsp
    sub  rsp, 40h
    mov  rcx, [stdout_h]
    mov  rdx, rsi
    mov  r8d, edi
    lea  r9, [written]
    mov  qword [rsp+20h], 0
    call WriteFile
    add  rsp, 40h
    pop  rbp
    ret
puts_raw_end:

; Minimal wndproc: everything to DefWindowProcA.
wndproc:
    push rbp
    mov  rbp, rsp
    sub  rsp, 20h
    call DefWindowProcA
    add  rsp, 20h
    pop  rbp
    ret
wndproc_end:

global start
start:
    push rbp
    mov  rbp, rsp
    sub  rsp, 70h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax

    mov  dword [wc + 0], 80
    mov  dword [wc + 4], 0
    lea  rax, [wndproc]
    mov  [wc + 8], rax
    mov  qword [wc + 48], 0x00F0F0F0
    lea  rax, [cls_name]
    mov  [wc + 64], rax
    lea  rcx, [wc]
    call RegisterClassExA
    test ax, ax
    jz   .fail

    xor  ecx, ecx
    lea  rdx, [cls_name]
    lea  r8,  [win_title]
    mov  r9d, WS_OVERLAPPEDWINDOW | WS_VISIBLE
    mov  dword [rsp+20h], 40
    mov  dword [rsp+28h], 40
    mov  dword [rsp+30h], 320
    mov  dword [rsp+38h], 200
    mov  qword [rsp+40h], 0
    mov  qword [rsp+48h], 0
    mov  qword [rsp+50h], 0
    mov  qword [rsp+58h], 0
    call CreateWindowExA
    test rax, rax
    jz   .fail
    mov  [hwnd], rax

    lea  rsi, [msg_armed]
    mov  edi, msg_armed_l
    call puts_raw

    ; Unguarded divide by zero with a live window: the modal box.
    xor  ecx, ecx
    mov  eax, 1
    xor  edx, edx
    div  ecx
    ; NOTREACHED (the box ends in ExitProcess)
    mov  ecx, 44
    call ExitProcess
    hlt
.fail:
    mov  ecx, 45
    call ExitProcess
    hlt
start_end:

section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd wndproc, wndproc_end, wndproc_xdata
    dd start, start_end, start_xdata

section .xdata
puts_raw_xdata:
    db 0x01, 8, 3, 0x05
    db 8, 0x72
    db 4, 0x03
    db 1, 0x50
    db 0, 0

wndproc_xdata:                  ; push rbp; mov rbp,rsp; sub rsp,0x20
    db 0x01, 8, 3, 0x05
    db 8, 0x32                  ; ALLOC_SMALL(3)
    db 4, 0x03                  ; SET_FPREG
    db 1, 0x50                  ; PUSH rbp
    db 0, 0

start_xdata:                    ; push rbp; mov rbp,rsp; sub rsp,0x70
    db 0x01, 8, 3, 0x05
    db 8, 0xd2                  ; ALLOC_SMALL(13): 13*8+8 = 0x70
    db 4, 0x03
    db 1, 0x50
    db 0, 0
