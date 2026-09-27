; W32A-11: independent v5/v6 PE manifest comparison, real GDI pixels,
; explicit UxTheme part refusals, and zero-duration buffered animation.
; NASM -D V5=1 selects the v5 guest (the import table stays identical).
; SPDX-License-Identifier: Apache-2.0
bits 64
default rel
extern RegisterClassExW
extern CreateWindowExW
extern DefWindowProcW
extern DestroyWindow
extern CreateCompatibleDC
extern DeleteDC
extern CreateCompatibleBitmap
extern SelectObject
extern DeleteObject
extern CreateSolidBrush
extern FillRect
extern GetPixel
extern OpenThemeData
extern CloseThemeData
extern DrawThemeBackground
extern GetThemeBackgroundContentRect
extern GetThemePartSize
extern GetThemeTransitionDuration
extern EnableThemeDialogTexture
extern BufferedPaintInit
extern BufferedPaintUnInit
extern BeginBufferedAnimation
extern EndBufferedAnimation
extern BufferedPaintStopAllAnimations
extern GetLastError
extern GetStdHandle
extern WriteFile
extern ExitProcess
%define WM_NCCREATE 0x81
%define WS_POPUP_VISIBLE 0x90000000
%define S_OK 0
%define E_NOTIMPL 0x80004001
%define E_HANDLE 0x80070006
section .rdata
class_w: dw 'A','1','1','T','h','e','m','e',0
title_w: dw 'T','h','e','m','e',' ','m','a','t','r','i','x',0
button_w: dw 'B','u','t','t','o','n',0
marker_v5: db 'A11-THEME-V5-OK',13,10
marker_v5_len equ $-marker_v5
marker_v6: db 'A11-THEME-V6-OK',13,10
marker_v6_len equ $-marker_v6
marker_anim: db 'A11-ANIMATION-OK',13,10
marker_anim_len equ $-marker_anim
marker_fail: db 'A11-THEME-MATRIX-FAIL',13,10
marker_fail_len equ $-marker_fail
section .data
rect: dd 2,2,12,12
small: dd 1,1,4,4
area: dd 0,0,8,8
anim: dd 16,0,0,0         ; Win64 BP_ANIMATIONPARAMS: no scheduler/duration
section .bss
wc: resb 80
hwnd: resq 1
stdout: resq 1
written: resd 1
dc: resq 1
bitmap: resq 1
old_obj: resq 1
brush: resq 1
baseline: resd 1
normal_color: resd 1
theme: resq 1
content: resd 4
size_out: resd 2
duration: resd 1
from_dc: resq 1
to_dc: resq 1
buffer: resq 1
section .text
global winstart
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
%macro PRINT 2
    mov qword [rsp+32],0
    mov rcx,[stdout]
    lea rdx,[%1]
    mov r8d,%2
    lea r9,[written]
    call WriteFile
%endmacro
%macro BEGIN_FRAME 0
    mov qword [rsp+32],0
    lea rax,[anim]
    mov [rsp+40],rax
    lea rax,[from_dc]
    mov [rsp+48],rax
    lea rax,[to_dc]
    mov [rsp+56],rax
    mov rcx,[hwnd]
    mov rdx,[dc]
    lea r8,[area]
    xor r9d,r9d      ; BPBF_COMPATIBLEBITMAP
    call BeginBufferedAnimation
%endmacro
winstart:
    push rbx
    sub rsp,144       ; shadow + 8 more args; still 16-byte aligned
    mov ecx,-11
    call GetStdHandle
    mov [stdout],rax
    mov dword [wc],80
    lea rax,[wndproc]
    mov [wc+8],rax
    lea rax,[class_w]
    mov [wc+64],rax
    lea rcx,[wc]
    call RegisterClassExW
    test ax,ax
    jz fail
    mov dword [rsp+32],90
    mov dword [rsp+40],140
    mov dword [rsp+48],120
    mov dword [rsp+56],90
    mov qword [rsp+64],0
    mov qword [rsp+72],0
    mov qword [rsp+80],0
    mov qword [rsp+88],0
    xor ecx,ecx
    lea rdx,[class_w]
    lea r8,[title_w]
    mov r9d,WS_POPUP_VISIBLE
    call CreateWindowExW
    test rax,rax
    jz fail
    mov [hwnd],rax
    xor ecx,ecx
    call CreateCompatibleDC
    test rax,rax
    jz fail
    mov [dc],rax
    mov rcx,rax
    mov edx,16
    mov r8d,16
    call CreateCompatibleBitmap
    test rax,rax
    jz fail
    mov [bitmap],rax
    mov rcx,[dc]
    mov rdx,rax
    call SelectObject
    test rax,rax
    jz fail
    mov [old_obj],rax
    mov ecx,0x00123456
    call CreateSolidBrush
    test rax,rax
    jz fail
    mov [brush],rax
    mov rcx,[dc]
    lea rdx,[rect]
    mov r8,rax
    call FillRect
    cmp eax,1
    jne fail
    mov rcx,[dc]
    mov edx,5
    mov r8d,5
    call GetPixel
    mov [baseline],eax
    cmp eax,0xffffffff
    je fail
    mov rcx,[brush]
    call DeleteObject
    test eax,eax
    jz fail
    mov rcx,[hwnd]
    lea rdx,[button_w]
    call OpenThemeData
%ifdef V5
    test rax,rax
    jnz fail
    call GetLastError
    cmp eax,50                  ; v5 refuses theming by name
    jne fail
    mov rcx,[dc]
    mov edx,5
    mov r8d,5
    call GetPixel
    cmp eax,[baseline]          ; unthemed fallback remains visible
    jne fail
    PRINT marker_v5,marker_v5_len
%else
    test rax,rax
    jz fail
    mov [theme],rax
    mov rcx,rax
    mov rdx,[dc]
    mov r8d,1
    mov r9d,1
    lea rax,[rect]
    mov [rsp+32],rax
    mov qword [rsp+40],0
    call DrawThemeBackground
    test eax,eax
    jnz fail
    mov rcx,[dc]
    mov edx,5
    mov r8d,5
    call GetPixel
    mov [normal_color],eax
    cmp eax,[baseline]          ; v6 theme actually overwrote fallback
    je fail
    mov rcx,[theme]
    mov rdx,[dc]
    mov r8d,1
    mov r9d,2
    lea rax,[rect]
    mov [rsp+32],rax
    mov qword [rsp+40],0
    call DrawThemeBackground
    test eax,eax
    jnz fail
    mov rcx,[dc]
    mov edx,5
    mov r8d,5
    call GetPixel
    cmp eax,[normal_color]      ; hot and normal use different pixels
    je fail
    mov [baseline],eax
    mov rcx,[theme]
    mov rdx,[dc]
    mov r8d,99
    mov r9d,1
    lea rax,[rect]
    mov [rsp+32],rax
    mov qword [rsp+40],0
    call DrawThemeBackground
    cmp eax,E_NOTIMPL
    jne fail
    mov rcx,[dc]
    mov edx,5
    mov r8d,5
    call GetPixel
    cmp eax,[baseline]          ; unknown part did not paint
    jne fail
    mov rcx,[theme]
    mov rdx,[dc]
    mov r8d,1
    mov r9d,2
    lea rax,[rect]
    mov [rsp+32],rax
    lea rax,[content]
    mov [rsp+40],rax
    call GetThemeBackgroundContentRect
    test eax,eax
    jnz fail
    cmp dword [content],4
    jne fail
    mov rcx,[theme]
    mov rdx,[dc]
    mov r8d,1
    mov r9d,2
    lea rax,[rect]
    mov [rsp+32],rax
    mov qword [rsp+40],2        ; TS_DRAW is measured, not a magic size
    lea rax,[size_out]
    mov [rsp+48],rax
    call GetThemePartSize
    test eax,eax
    jnz fail
    cmp dword [size_out],10
    jne fail
    mov rcx,[theme]
    mov edx,1
    mov r8d,1
    mov r9d,2
    mov qword [rsp+32],6000    ; TMT_TRANSITIONDURATIONS
    lea rax,[duration]
    mov [rsp+40],rax
    mov dword [duration],123
    call GetThemeTransitionDuration
    test eax,eax
    jnz fail
    cmp dword [duration],0     ; honest flat theme: no timed transition
    jne fail
    mov rcx,[hwnd]
    mov edx,6                  ; ETDT_ENABLE | ETDT_USETABTEXTURE
    call EnableThemeDialogTexture
    test eax,eax
    jnz fail
    PRINT marker_v6,marker_v6_len

    call BufferedPaintInit
    test eax,eax
    jnz fail
    BEGIN_FRAME
    test rax,rax
    jz fail
    mov [buffer],rax
    mov rax,[from_dc]
    test rax,rax
    jz fail
    cmp rax,[to_dc]
    je fail
    mov ecx,0x00001234
    call CreateSolidBrush
    test rax,rax
    jz fail
    mov [brush],rax
    mov rcx,[to_dc]
    lea rdx,[small]
    mov r8,[brush]
    call FillRect
    cmp eax,1
    jne fail
    mov rcx,[brush]
    call DeleteObject
    test eax,eax
    jz fail
    mov rcx,[buffer]
    mov edx,1
    call EndBufferedAnimation
    test eax,eax
    jnz fail
    mov rcx,[dc]
    mov edx,2
    mov r8d,2
    call GetPixel
    cmp eax,0x00001234
    jne fail
    mov rcx,[buffer]
    mov edx,1
    call EndBufferedAnimation
    cmp eax,E_HANDLE           ; one-shot buffer token
    jne fail
    mov dword [anim+12],100
    BEGIN_FRAME
    test rax,rax               ; no frame scheduler => must refuse
    jnz fail
    call GetLastError
    cmp eax,50
    jne fail
    mov dword [anim+12],0
    BEGIN_FRAME
    test rax,rax
    jz fail
    mov [buffer],rax
    mov rcx,[hwnd]
    call BufferedPaintStopAllAnimations
    test eax,eax
    jnz fail
    mov rcx,[buffer]
    mov edx,1
    call EndBufferedAnimation
    cmp eax,E_HANDLE           ; stop invalidated the active frame
    jne fail
    call BufferedPaintUnInit
    test eax,eax
    jnz fail
    mov rcx,[theme]
    call CloseThemeData
    test eax,eax
    jnz fail
    PRINT marker_anim,marker_anim_len
%endif
    mov rcx,[dc]
    mov rdx,[old_obj]
    call SelectObject
    mov rcx,[bitmap]
    call DeleteObject
    mov rcx,[dc]
    call DeleteDC
    mov rcx,[hwnd]
    call DestroyWindow
    mov ecx,78
    call ExitProcess
fail:
    PRINT marker_fail,marker_fail_len
    mov ecx,79
    call ExitProcess
