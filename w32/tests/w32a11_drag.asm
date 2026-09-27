; W32A-11: independent PE fixture exercising Win64 OLE vtable callbacks,
; refcounts, two compositor-backed windows and a posted mouse transition.
; No class activation, outside OLE source, or real mouse is claimed.
; SPDX-License-Identifier: Apache-2.0
bits 64
default rel
extern OleInitialize
extern OleUninitialize
extern RegisterDragDrop
extern RevokeDragDrop
extern DoDragDrop
extern RegisterClassExW
extern CreateWindowExW
extern DefWindowProcW
extern PostMessageW
extern DestroyWindow
extern GetStdHandle
extern WriteFile
extern ExitProcess
%define WM_NCCREATE 0x81
%define WM_MOUSEMOVE 0x200
%define WM_LBUTTONUP 0x202
%define WS_POPUP_VISIBLE 0x90000000
%define S_OK 0
%define DRAGDROP_S_DROP 0x00040100
%define DRAGDROP_E_ALREADYREGISTERED 0x80040101
section .rdata
cls_w: dw 'A','1','1','D','r','a','g',0
title_left: dw 'L','e','f','t',0
title_right: dw 'R','i','g','h','t',0
msg_success: db 'A11-DRAG-OK',13,10
success_len equ $-msg_success
msg_fail: db 'A11-DRAG-FAIL',13,10
fail_len equ $-msg_fail
align 8
target_vt: dq query, tadd, trel, enter, over, leave, drop
source_vt: dq query, sadd, srel, cont, feedback
section .data
target_left: dq target_vt
             dd 1,0
target_right: dq target_vt
              dd 1,0
source: dq source_vt
        dd 1,0
data_tag: dq 0x4452414750415448
section .bss
stdout: resq 1
written: resd 1
wc: resb 80
h_left: resq 1
h_right: resq 1
chosen: resd 1
enters_left: resd 1
enters_right: resd 1
overs_left: resd 1
overs_right: resd 1
leaves_left: resd 1
drops_right: resd 1
got_data: resq 1
got_point: resq 1
queries: resd 1
feeds: resd 1
section .text
global winstart
; All vtable methods have ms_abi; no C shim runs between the PE and target.
query:
    mov eax,0x80004002 ; E_NOINTERFACE (the fixture never asks)
    ret
tadd:
    inc dword [rcx+8]
    mov eax,[rcx+8]
    ret
trel:
    dec dword [rcx+8]
    mov eax,[rcx+8]
    ret
enter:
    ; IDropTarget::DragEnter(this,data,keys,POINTL,dwEffect*)
    lea rax,[data_tag]
    cmp rdx,rax
    jne .bad
    cmp r8d,1
    jne .bad
    mov rax,[rsp+40]
    mov dword [rax],1
    lea rax,[target_left]
    cmp rcx,rax
    jne .right
    inc dword [enters_left]
    xor eax,eax
    ret
.right:
    lea rax,[target_right]
    cmp rcx,rax
    jne .bad
    inc dword [enters_right]
    xor eax,eax
    ret
.bad:
    mov eax,0x80004005
    ret
over:
    ; IDropTarget::DragOver(this,keys,POINTL,dwEffect*)
    cmp edx,1
    jne .bad
    mov rax,[rsp+40]
    mov dword [rax],1
    lea rax,[target_left]
    cmp rcx,rax
    jne .right
    inc dword [overs_left]
    xor eax,eax
    ret
.right:
    lea rax,[target_right]
    cmp rcx,rax
    jne .bad
    inc dword [overs_right]
    xor eax,eax
    ret
.bad:
    mov eax,0x80004005
    ret
leave:
    lea rax,[target_left]
    cmp rcx,rax
    jne .bad
    inc dword [leaves_left]
    xor eax,eax
    ret
.bad:
    mov eax,0x80004005
    ret
drop:
    lea rax,[target_right]
    cmp rcx,rax
    jne .bad
    lea rax,[data_tag]
    cmp rdx,rax
    jne .bad
    test r8d,r8d
    jnz .bad
    inc dword [drops_right]
    mov [got_data],rdx
    mov [got_point],r9
    mov rax,[rsp+40]
    mov dword [rax],1
    xor eax,eax
    ret
.bad:
    mov eax,0x80004005
    ret
sadd:
    inc dword [rcx+8]
    mov eax,[rcx+8]
    ret
srel:
    dec dword [rcx+8]
    mov eax,[rcx+8]
    ret
cont:
    inc dword [queries]
    test edx,edx
    jnz .cancel
    test r8d,1
    jz .drop
    xor eax,eax
    ret
.drop:
    mov eax,0x00040100
    ret
.cancel:
    mov eax,0x00040101
    ret
feedback:
    inc dword [feeds]
    mov eax,0x00040102
    ret
wndproc:
    cmp edx,WM_NCCREATE
    jne .other
    mov eax,1
    ret
.other:
    sub rsp,40
    call DefWindowProcW
    add rsp,40
    ret
%macro POST 3
    mov rcx,[h_left]
    mov edx,%1
    mov r8d,%2
    mov r9d,%3
    call PostMessageW
    test eax,eax
    jz fail
%endmacro
%macro PRINT 2
    mov qword [rsp+32],0
    mov rcx,[stdout]
    lea rdx,[%1]
    mov r8d,%2
    lea r9,[written]
    call WriteFile
%endmacro
winstart:
    push rbx
    sub rsp,128  ; shadow + eight stack args, aligned
    mov ecx,-11
    call GetStdHandle
    mov [stdout],rax
    xor ecx,ecx
    call OleInitialize
    test eax,eax
    jnz fail
    mov dword [wc],80
    lea rax,[wndproc]
    mov [wc+8],rax
    lea rax,[cls_w]
    mov [wc+64],rax
    lea rcx,[wc]
    call RegisterClassExW
    test ax,ax
    jz fail
    ; Whole-window rects: two nonoverlapping undecorated HWNDs.
    mov dword [rsp+32],60
    mov dword [rsp+40],80
    mov dword [rsp+48],160
    mov dword [rsp+56],120
    mov qword [rsp+64],0
    mov qword [rsp+72],0
    mov qword [rsp+80],0
    mov qword [rsp+88],0
    xor ecx,ecx
    lea rdx,[cls_w]
    lea r8,[title_left]
    mov r9d,WS_POPUP_VISIBLE
    call CreateWindowExW
    test rax,rax
    jz fail
    mov [h_left],rax
    mov dword [rsp+32],300
    mov dword [rsp+40],80
    mov dword [rsp+48],160
    mov dword [rsp+56],120
    mov qword [rsp+64],0
    mov qword [rsp+72],0
    mov qword [rsp+80],0
    mov qword [rsp+88],0
    xor ecx,ecx
    lea rdx,[cls_w]
    lea r8,[title_right]
    mov r9d,WS_POPUP_VISIBLE
    call CreateWindowExW
    test rax,rax
    jz fail
    mov [h_right],rax
    mov rcx,[h_left]
    lea rdx,[target_left]
    call RegisterDragDrop
    test eax,eax
    jnz fail
    mov rcx,[h_right]
    lea rdx,[target_right]
    call RegisterDragDrop
    test eax,eax
    jnz fail
    mov rcx,[h_left]
    lea rdx,[target_left]
    call RegisterDragDrop
    cmp eax,DRAGDROP_E_ALREADYREGISTERED
    jne fail
    ; lParam is source client coords. ClientToScreen and WindowFromPoint
    ; identify target_left then target_right despite all events on h_left.
    POST WM_MOUSEMOVE,1,0x00140014
    POST WM_MOUSEMOVE,1,0x00140015
    POST WM_MOUSEMOVE,1,0x00140104
    POST WM_MOUSEMOVE,1,0x00140105
    POST WM_LBUTTONUP,0,0x00140105
    lea rcx,[data_tag]
    lea rdx,[source]
    mov r8d,1
    lea r9,[chosen]
    call DoDragDrop
    cmp eax,DRAGDROP_S_DROP
    jne fail
    cmp dword [chosen],1
    jne fail
    cmp dword [enters_left],1
    jne fail
    cmp dword [enters_right],1
    jne fail
    cmp dword [overs_left],1
    jne fail
    cmp dword [overs_right],1
    jne fail
    cmp dword [leaves_left],1
    jne fail
    cmp dword [drops_right],1
    jne fail
    cmp dword [queries],5
    jne fail
    cmp dword [feeds],4
    jne fail
    lea rax,[data_tag]
    cmp [got_data],rax
    jne fail
    cmp dword [got_point],321 ; source client 261 + left origin 60
    jne fail
    cmp dword [got_point+4],100 ; source client 20 + top origin 80
    jne fail
    mov rcx,[h_left]
    call RevokeDragDrop
    test eax,eax
    jnz fail
    mov rcx,[h_right]
    call RevokeDragDrop
    test eax,eax
    jnz fail
    cmp dword [target_left+8],1
    jne fail
    cmp dword [target_right+8],1
    jne fail
    cmp dword [source+8],1
    jne fail
    PRINT msg_success,success_len
    mov rcx,[h_right]
    call DestroyWindow
    mov rcx,[h_left]
    call DestroyWindow
    call OleUninitialize
    mov ecx,78
    call ExitProcess
fail:
    PRINT msg_fail,fail_len
    mov ecx,79
    call ExitProcess
