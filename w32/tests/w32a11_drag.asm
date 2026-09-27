; W32A-11: independent PE fixture exercising Win64 OLE vtable callbacks,
; refcounts, a real IDataObject::GetData(CF_HDROP) / HGLOBAL / STGMEDIUM,
; two compositor-backed windows, and a posted mouse transition.  No class
; activation, cross-process OLE marshalling, or real mouse is claimed.
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
extern GlobalAlloc
extern GlobalLock
extern GlobalSize
extern GlobalFree
extern CreateFileW
extern ReadFile
extern CloseHandle
extern ReleaseStgMedium
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
msg_success: db 'A11-DRAG-GETDATA-OK',13,10
success_len equ $-msg_success
msg_fail: db 'A11-DRAG-FAIL',13,10
fail_len equ $-msg_fail
; FORMATETC: cfFormat=CF_HDROP(15), ptd=NULL, DVASPECT_CONTENT(1),
; lindex=-1, tymed=TYMED_HGLOBAL(1); published Win64 layout (32 bytes).
align 8
format_etc: dw 15
            times 6 db 0
            dq 0
            dd 1, -1, 1, 0
; DROPFILES: pFiles=20, pt=(0,0), fNC=0, fWide=1.  The UTF-16
; MULTI_SZ holds exactly one Unicode filename and the double terminator.
drop_blob: dd 20, 0, 0, 0, 1
path_expected: dw '/', 't', 'e', 's', 't', 's', '/', 'w', '3', '2', 'a', '1', '1', '-'
               dw 0x00e9, '.', 't', 'x', 't', 0
path_units equ ($-path_expected)/2-1
           dw 0
drop_blob_size equ $-drop_blob
content: db 'W32A11-DROP-FILE-CONTENTS',10
content_len equ $-content
align 8
target_vt: dq query, tadd, trel, enter, over, leave, drop
source_vt: dq query, sadd, srel, cont, feedback
; IDataObject derives from IUnknown; unused methods fail by name, not zero
; pointers.  QueryGetData is vtable index 5, GetData index 3.
iid_data: dq 0x000000000000010e, 0x46000000000000c0
data_vt: dq data_query, data_add, data_release, data_get
         dq data_notimpl, data_query_get, data_notimpl, data_notimpl
         dq data_notimpl, data_notimpl, data_notimpl, data_notimpl
section .data
target_left: dq target_vt
             dd 1,0
target_right: dq target_vt
              dd 1,0
source: dq source_vt
        dd 1,0
data_obj: dq data_vt
          dd 1,0
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
data_queries: resd 1
data_gets: resd 1
data_qis: resd 1
iface: resq 1
medium: resq 3
medium_handle: resq 1
file: resq 1
payload: resb 64
received: resd 1
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
; IID_IUnknown = 00000000-0000-0000-C000-000000000046
; IID_IDataObject = 0000010E-0000-0000-C000-000000000046
data_query:
    test r8,r8
    jz .badptr
    mov qword [r8],0
    test rdx,rdx
    jz .badptr
    mov r10,0x46000000000000c0
    cmp qword [rdx+8],r10
    jne .no
    cmp qword [rdx],0
    je .yes
    cmp qword [rdx],0x10e
    jne .no
.yes:
    lea rax,[data_obj]
    cmp rcx,rax
    jne .no
    inc dword [rcx+8]
    mov [r8],rcx
    inc dword [data_qis]
    xor eax,eax
    ret
.no:
    mov eax,0x80004002 ; E_NOINTERFACE
    ret
.badptr:
    mov eax,0x80004003 ; E_POINTER
    ret
data_add:
    inc dword [rcx+8]
    mov eax,[rcx+8]
    ret
data_release:
    dec dword [rcx+8]
    mov eax,[rcx+8]
    ret
data_notimpl:
    mov eax,0x80004001 ; E_NOTIMPL
    ret
data_query_get:
    test rdx,rdx
    jz .no
    cmp word [rdx],15 ; CF_HDROP
    jne .no
    cmp qword [rdx+8],0
    jne .no
    cmp dword [rdx+16],1 ; DVASPECT_CONTENT
    jne .no
    cmp dword [rdx+20],-1
    jne .no
    test dword [rdx+24],1 ; TYMED_HGLOBAL
    jz .no
    inc dword [data_queries]
    xor eax,eax
    ret
.no:
    mov eax,0x80040064 ; DV_E_FORMATETC
    ret
data_get:
    ; HRESULT GetData(this, FORMATETC*, STGMEDIUM*).  Each successful
    ; request owns a newly allocated HGLOBAL, released by the recipient.
    test r8,r8
    jz .bad
    mov qword [r8],0
    mov qword [r8+8],0
    mov qword [r8+16],0
    ; QueryGetData uses exactly this FORMATETC (no global state).
    test rdx,rdx
    jz .bad
    cmp word [rdx],15
    jne .bad
    cmp qword [rdx+8],0
    jne .bad
    cmp dword [rdx+16],1
    jne .bad
    cmp dword [rdx+20],-1
    jne .bad
    test dword [rdx+24],1
    jz .bad
    push rbx
    sub rsp,48 ; shadow space, 16-byte aligned
    mov rbx,r8
    mov ecx,0x42 ; GMEM_MOVEABLE|GMEM_ZEROINIT
    mov edx,drop_blob_size
    call GlobalAlloc
    test rax,rax
    jz .nomem
    mov dword [rbx],1
    mov [rbx+8],rax
    mov qword [rbx+16],0
    mov rcx,rax
    call GlobalLock
    test rax,rax
    jz .free
    lea r10,[drop_blob]
    xor ecx,ecx
.copy:
    mov dl,[r10+rcx]
    mov [rax+rcx],dl
    inc ecx
    cmp ecx,drop_blob_size
    jb .copy
    inc dword [data_gets]
    add rsp,48
    pop rbx
    xor eax,eax
    ret
.free:
    mov rcx,[rbx+8]
    call GlobalFree
    mov qword [rbx+8],0
    mov dword [rbx],0
.nomem:
    add rsp,48
    pop rbx
    mov eax,0x8007000e ; E_OUTOFMEMORY
    ret
.bad:
    mov eax,0x80040064 ; DV_E_FORMATETC
    ret
enter:
    ; IDropTarget::DragEnter(this,data,keys,POINTL,dwEffect*)
    lea rax,[data_obj]
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
    ; IDropTarget::Drop(this,data,keys,POINTL,dwEffect*).  Verify an
    ; actual IID_IDataObject and obtain CF_HDROP via GetData's vtable.
    lea rax,[target_right]
    cmp rcx,rax
    jne .bad
    lea rax,[data_obj]
    cmp rdx,rax
    jne .bad
    test r8d,r8d
    jnz .bad
    mov [got_data],rdx
    mov [got_point],r9
    push rbx
    sub rsp,128 ; shadow space + stack args, 16-byte aligned
    mov rbx,[rsp+176] ; 5th arg from original Win64 frame
    test rbx,rbx
    jz .failure
    lea rcx,[data_obj]
    lea rdx,[rel iid_data]
    lea r8,[iface]
    mov rax,[rcx]
    call qword [rax] ; QueryInterface
    test eax,eax
    jnz .failure
    lea rax,[data_obj]
    cmp [iface],rax
    jne .failure
    mov rcx,[iface]
    mov rax,[rcx]
    call qword [rax+16] ; Release the QI reference
    lea rcx,[data_obj]
    lea rdx,[format_etc]
    mov rax,[rcx]
    call qword [rax+40] ; QueryGetData
    test eax,eax
    jnz .failure
    lea rcx,[data_obj]
    lea rdx,[format_etc]
    lea r8,[medium]
    mov rax,[rcx]
    call qword [rax+24] ; GetData
    test eax,eax
    jnz .failure
    cmp dword [medium],1 ; TYMED_HGLOBAL
    jne .release_bad
    cmp qword [medium+16],0 ; pUnkForRelease
    jne .release_bad
    mov rcx,[medium+8]
    test rcx,rcx
    jz .release_bad
    call GlobalSize
    cmp rax,drop_blob_size
    jb .release_bad
    mov rcx,[medium+8]
    call GlobalLock
    test rax,rax
    jz .release_bad
    cmp dword [rax],20 ; DROPFILES.pFiles
    jne .release_bad
    cmp dword [rax+12],0 ; DROPFILES.fNC
    jne .release_bad
    cmp dword [rax+16],1 ; DROPFILES.fWide
    jne .release_bad
    lea r10,[rax+20]
    lea r11,[path_expected]
    xor ecx,ecx
.path:
    mov dx,[r10+rcx*2]
    cmp dx,[r11+rcx*2]
    jne .release_bad
    inc ecx
    cmp ecx,path_units+1
    jb .path
    cmp word [r10+rcx*2],0 ; MULTI_SZ double NUL
    jne .release_bad
    ; Open using the pathname *inside the returned medium*, not our
    ; expected constant or an ASCII version of it.
    mov rcx,r10
    mov edx,0x80000000 ; GENERIC_READ
    mov r8d,1
    xor r9d,r9d
    mov qword [rsp+32],3 ; OPEN_EXISTING
    mov qword [rsp+40],0
    mov qword [rsp+48],0
    call CreateFileW
    cmp rax,-1
    je .release_bad
    mov [file],rax
    mov rcx,rax
    lea rdx,[payload]
    mov r8d,64
    lea r9,[received]
    mov qword [rsp+32],0
    call ReadFile
    test eax,eax
    jz .close_bad
    cmp dword [received],content_len
    jne .close_bad
    xor ecx,ecx
    lea r10,[payload]
    lea r11,[content]
.compare:
    mov dl,[r10+rcx]
    cmp dl,[r11+rcx]
    jne .close_bad
    inc ecx
    cmp ecx,content_len
    jb .compare
    mov rcx,[file]
    call CloseHandle
    test eax,eax
    jz .release_bad
    mov rcx,[medium+8]
    mov [medium_handle],rcx
    lea rcx,[medium]
    call ReleaseStgMedium
    cmp dword [medium],0
    jne .failure
    cmp qword [medium+8],0
    jne .failure
    mov rcx,[medium_handle]
    call GlobalSize
    test rax,rax ; allocation must have been freed
    jnz .failure
    inc dword [drops_right]
    mov dword [rbx],1
    add rsp,128
    pop rbx
    xor eax,eax
    ret
.close_bad:
    mov rcx,[file]
    call CloseHandle
.release_bad:
    lea rcx,[medium]
    call ReleaseStgMedium
.failure:
    add rsp,128
    pop rbx
.bad:
    mov eax,0x80004005 ; E_FAIL
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
    lea rcx,[data_obj]
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
    cmp dword [data_gets],1
    jne fail
    cmp dword [data_queries],1
    jne fail
    cmp dword [data_qis],1
    jne fail
    cmp dword [data_obj+8],1
    jne fail
    lea rax,[data_obj]
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
