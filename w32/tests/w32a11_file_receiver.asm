; W32A-11 independent PE: compositor GUI_EVT_DROP -> HWND WM_DROPFILES,
; genuine HDROP UTF-16 path -> open/read on-disk payload -> DragFinish.
; An import library is not a DLL, and no path pointer is shared across tasks.
; SPDX-License-Identifier: Apache-2.0
bits 64
default rel
extern RegisterClassExW
extern CreateWindowExW
extern DefWindowProcW
extern DispatchMessageW
extern GetMessageW
extern GetCursorPos
extern SetWindowPos
extern ShowWindow
extern GetStdHandle
extern WriteFile
extern CreateFileA
extern ReadFile
extern CloseHandle
extern DragQueryFileW
extern DragQueryPoint
extern DragFinish
extern ExitProcess
%define WM_NCCREATE 0x81
%define WM_DROPFILES 0x233
%define WS_POPUP_VISIBLE 0x90000000
%define WS_EX_ACCEPTFILES 0x10
section .rdata
cls_w: dw 'A','1','1','F','i','l','e',0
title_w: dw 'F','i','l','e',' ','r','e','c','e','i','v','e','r',0
path_a: db '/tests/w32a11_payload.txt',0
content: db 'W32A11-DROP-FILE-CONTENTS',10
content_len equ $-content
ready_msg: db 'A11-FILE-READY',13,10
ready_len equ $-ready_msg
ok_msg: db 'A11-FILE-OK',13,10
ok_len equ $-ok_msg
fail_msg: db 'A11-FILE-FAIL',13,10
fail_len equ $-fail_msg
section .bss
stdout: resq 1
written: resd 1
wc: resb 80
hwnd: resq 1
msg: resb 48
pos: resd 2
pt: resd 2
path_w: resw 260
path_from_drop: resb 260
payload: resb 64
received: resd 1
hdrop: resq 1
file: resq 1
section .text
global winstart
%macro PRINT 2
    mov qword [rsp+32],0
    mov rcx,[stdout]
    lea rdx,[%1]
    mov r8d,%2
    lea r9,[written]
    call WriteFile
%endmacro
wndproc:
    cmp edx,WM_NCCREATE
    je .nc
    cmp edx,WM_DROPFILES
    je .receive
    sub rsp,40
    call DefWindowProcW
    add rsp,40
    ret
.nc:
    mov eax,1
    ret
.receive:
    ; Entry rsp%16=8, push rbx -> aligned; reserve shadow + 32 bytes.
    push rbx
    sub rsp,64
    mov [hdrop],r8
    mov rcx,r8
    mov edx,0xffffffff
    xor r8d,r8d
    xor r9d,r9d
    call DragQueryFileW
    cmp eax,1
    jne fail
    mov rcx,[hdrop]
    xor edx,edx
    lea r8,[path_w]
    mov r9d,260
    call DragQueryFileW
    cmp eax,25 ; length of /tests/w32a11_payload.txt (verified below)
    jne fail
    mov rcx,[hdrop]
    lea rdx,[pt]
    call DragQueryPoint
    test eax,eax
    jz fail
    cmp dword [pt],200
    jae fail
    cmp dword [pt+4],100
    jae fail
    xor ecx,ecx
    lea r10,[path_w]
    lea r11,[path_a]
    lea rbx,[path_from_drop]
.path_loop:
    movzx edx,word [r10+rcx*2]
    cmp edx,127
    ja fail
    cmp dl,[r11+rcx]
    jne fail
    mov [rbx+rcx],dl
    test dl,dl
    jz .read_file
    inc ecx
    cmp ecx,259
    jae fail
    jmp .path_loop
.read_file:
    ; The opened path is converted from DragQueryFileW, not hardwired.
    lea rcx,[path_from_drop]
    mov edx,0x80000000
    mov r8d,1
    xor r9d,r9d
    mov qword [rsp+32],3 ; OPEN_EXISTING
    mov qword [rsp+40],0
    mov qword [rsp+48],0
    call CreateFileA
    cmp rax,-1
    je fail
    mov [file],rax
    mov rcx,rax
    lea rdx,[payload]
    mov r8d,64
    lea r9,[received]
    mov qword [rsp+32],0
    call ReadFile
    test eax,eax
    jz fail
    cmp dword [received],content_len
    jne fail
    xor ecx,ecx
    lea r10,[payload]
    lea r11,[content]
.compare:
    mov al,[r10+rcx]
    cmp al,[r11+rcx]
    jne fail
    inc ecx
    cmp ecx,content_len
    jb .compare
    mov rcx,[file]
    call CloseHandle
    test eax,eax
    jz fail
    mov rcx,[hdrop]
    call DragFinish
    PRINT ok_msg,ok_len
    mov ecx,78
    call ExitProcess
    ud2
fail:
    PRINT fail_msg,fail_len
    mov ecx,79
    call ExitProcess
    ud2
winstart:
    push rbx
    sub rsp,128
    mov ecx,-11
    call GetStdHandle
    mov [stdout],rax
    mov dword [wc],80
    lea rax,[wndproc]
    mov [wc+8],rax
    lea rax,[cls_w]
    mov [wc+64],rax
    lea rcx,[wc]
    call RegisterClassExW
    test ax,ax
    jz fail_main
    ; GetCursorPos requires that THIS process already owns a GUI window.
    ; Create it hidden, query the pointer, then move it before making visible.
    mov dword [rsp+32],0
    mov dword [rsp+40],0
    mov dword [rsp+48],200
    mov dword [rsp+56],100
    mov qword [rsp+64],0
    mov qword [rsp+72],0
    mov qword [rsp+80],0
    mov qword [rsp+88],0
    mov ecx,WS_EX_ACCEPTFILES
    lea rdx,[cls_w]
    lea r8,[title_w]
    mov r9d,0x80000000 ; WS_POPUP without WS_VISIBLE
    call CreateWindowExW
    test rax,rax
    jz fail_main
    mov [hwnd],rax
    lea rcx,[pos]
    call GetCursorPos
    test eax,eax
    jz fail_main
    mov rcx,[hwnd]
    xor edx,edx
    mov r8d,[pos]
    sub r8d,100
    mov r9d,[pos+4]
    sub r9d,50
    mov qword [rsp+32],200
    mov qword [rsp+40],100
    mov qword [rsp+48],4 ; SWP_NOZORDER
    call SetWindowPos
    test eax,eax
    jz fail_main
    mov rcx,[hwnd]
    mov edx,5 ; SW_SHOW
    call ShowWindow
    PRINT ready_msg,ready_len
.pump:
    lea rcx,[msg]
    xor edx,edx
    xor r8d,r8d
    xor r9d,r9d
    call GetMessageW
    test eax,eax
    jle fail_main
    lea rcx,[msg]
    call DispatchMessageW
    jmp .pump
fail_main:
    PRINT fail_msg,fail_len
    mov ecx,79
    call ExitProcess
