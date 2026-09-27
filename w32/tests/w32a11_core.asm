; W32A-11 incremental GUEST gate. Effects, not unconditional PASS markers.
; NOT the full CLSID probe/compositor file drop/animation phase gate.
; SPDX-License-Identifier: Apache-2.0
bits 64
default rel
extern CoInitialize
extern CoUninitialize
extern OleInitialize
extern OleUninitialize
extern CoTaskMemAlloc
extern CoTaskMemFree
extern SysAllocString
extern SysAllocStringLen
extern SysFreeString
extern SysStringLen
extern SysStringByteLen
extern SysAllocStringByteLen
extern VariantClear
extern VariantCopy
extern ImmGetContext
extern ImmReleaseContext
extern ImmGetCompositionStringW
extern ImmSetCompositionWindow
extern ImmSetCompositionFontA
extern ImmSetCompositionFontW
extern ImmSetCandidateWindow
extern ImmSetCompositionStringW
extern ImmEscapeW
extern ImmNotifyIME
extern RegisterClipboardFormatA
extern RegisterClipboardFormatW
extern OpenThemeData
extern CloseThemeData
extern DrawThemeBackground
extern GetThemeBackgroundContentRect
extern CreateCompatibleDC
extern CreateCompatibleBitmap
extern SelectObject
extern GetPixel
extern DeleteObject
extern DeleteDC
extern GetStdHandle
extern WriteFile
extern ExitProcess
section .data
wide_bstr: dw 'A',0,'B',0
wide_plain: dw 'X','Y',0
odd_bytes: db 'x',0,'z'
name_a: db 'FileNameW',0
other_a: db 'FileContents',0
name_w: dw 'F','I','L','E','N','A','M','E','W',0
button_w: dw 'B','u','t','t','o','n',0
rect: dd 2,2,12,12
composition_buf: dd 0x12345678
src_var: dw 3,0,0,0
         dd -42,0
dst_var: times 16 db 0
m_com: db 'A11-COM-OK',13,10
m_com_len equ $-m_com
m_var: db 'A11-VARIANT-OK',13,10
m_var_len equ $-m_var
m_ord: db 'A11-ORDINALS-OK',13,10
m_ord_len equ $-m_ord
m_imm: db 'A11-IMM-OK',13,10
m_imm_len equ $-m_imm
m_clip: db 'A11-CLIPFMT-OK',13,10
m_clip_len equ $-m_clip
m_theme: db 'A11-THEME-OK',13,10
m_theme_len equ $-m_theme
m_final: db 'W32A11-CORE-SUBSET-OK',13,10
m_final_len equ $-m_final
m_fail: db 'W32A11-CORE-SUBSET-FAIL',13,10
m_fail_len equ $-m_fail
section .bss
written: resd 1
stdout: resq 1
bstr: resq 1
format_id: resd 1
theme: resq 1
dc: resq 1
bitmap: resq 1
normal_color: resd 1
content: resd 4
section .text
global winstart
%macro PRINT 2
    ; The Win64 fifth WriteFile argument (LPOVERLAPPED) must be zero.
    ; Theme/IMM calls reuse this stack slot for other pointers.
    mov qword [rsp+0x20], 0
    mov rcx, [stdout]
    lea rdx, [%1]
    mov r8d, %2
    lea r9, [written]
    call WriteFile
%endmacro
winstart:
    push rbx
    sub rsp, 0x60             ; 32B shadow + args 5/6; 16B aligned
    mov ecx, -11
    call GetStdHandle
    mov [stdout], rax

    ; Per-thread COM nesting; OleInitialize owns one Co depth.
    xor ecx, ecx
    call CoInitialize
    test eax, eax
    jnz fail
    xor ecx, ecx
    call CoInitialize
    cmp eax, 1
    jne fail
    xor ecx, ecx
    call OleInitialize
    cmp eax, 1
    jne fail
    mov ecx, 16
    call CoTaskMemAlloc
    test rax, rax
    jz fail
    mov byte [rax+15], 0x65
    cmp byte [rax+15], 0x65
    jne fail
    mov rcx, rax
    call CoTaskMemFree
    call OleUninitialize
    call CoUninitialize
    call CoUninitialize
    xor ecx, ecx
    call CoInitialize
    test eax, eax
    jnz fail
    call CoUninitialize
    PRINT m_com, m_com_len

    ; Ordinal #4/#7/#149: embedded NUL is data; scalar #10/#9 are real.
    lea rcx, [wide_bstr]
    mov edx, 3
    call SysAllocStringLen
    mov [bstr], rax
    test rax, rax
    jz fail
    mov rcx, rax
    call SysStringLen
    cmp eax, 3
    jne fail
    mov rcx, [bstr]
    call SysStringByteLen
    cmp eax, 6
    jne fail
    mov rax, [bstr]
    cmp word [rax+2], 0
    jne fail
    cmp word [rax+4], 'B'
    jne fail
    lea rcx, [dst_var]
    lea rdx, [src_var]
    call VariantCopy
    test eax, eax
    jnz fail
    cmp word [dst_var], 3
    jne fail
    cmp dword [dst_var+8], -42
    jne fail
    lea rcx, [dst_var]
    call VariantClear
    test eax, eax
    jnz fail
    cmp word [dst_var], 0
    jne fail
    mov word [dst_var], 8
    mov rax, [bstr]
    mov [dst_var+8], rax
    lea rcx, [dst_var]
    call VariantClear
    test eax, eax
    jnz fail
    cmp qword [dst_var+8], 0
    jne fail

    ; #2/#6 normal UTF-16 allocation; #150 odd-byte BSTR; #149 and
    ; #10 preserve all THREE bytes, not floor(byte_len / sizeof WCHAR).
    lea rcx, [wide_plain]
    call SysAllocString
    mov [bstr], rax
    test rax, rax
    jz fail
    mov rcx, rax
    call SysStringLen
    cmp eax, 2
    jne fail
    mov rcx, [bstr]
    call SysFreeString
    lea rcx, [odd_bytes]
    mov edx, 3
    call SysAllocStringByteLen
    mov [bstr], rax
    test rax, rax
    jz fail
    cmp byte [rax+2], 'z'
    jne fail
    cmp byte [rax+3], 0
    jne fail
    mov rcx, rax
    call SysStringByteLen
    cmp eax, 3
    jne fail
    mov word [src_var], 8
    mov rax, [bstr]
    mov [src_var+8], rax
    lea rcx, [dst_var]
    lea rdx, [src_var]
    call VariantCopy
    test eax, eax
    jnz fail
    mov rcx, [dst_var+8]
    test rcx, rcx
    jz fail
    cmp rcx, [bstr]
    je fail
    call SysStringByteLen
    cmp eax, 3
    jne fail
    mov rax, [dst_var+8]
    cmp byte [rax+2], 'z'
    jne fail
    lea rcx, [src_var]
    call VariantClear
    test eax, eax
    jnz fail
    lea rcx, [dst_var]
    call VariantClear
    test eax, eax
    jnz fail
    PRINT m_var, m_var_len
    PRINT m_ord, m_ord_len

    ; Ten IME exports: no phantom context/composition/buffer mutation.
    xor ecx, ecx
    call ImmGetContext
    test rax, rax
    jnz fail
    xor ecx, ecx
    xor edx, edx
    lea r8, [composition_buf]
    mov r9d, 4
    call ImmGetCompositionStringW
    test eax, eax
    jnz fail
    cmp dword [composition_buf], 0x12345678
    jne fail
    xor ecx, ecx
    xor edx, edx
    call ImmReleaseContext
    test eax, eax
    jnz fail
    xor ecx, ecx
    xor edx, edx
    call ImmSetCompositionWindow
    test eax, eax
    jnz fail
    xor ecx, ecx
    xor edx, edx
    call ImmSetCompositionFontA
    test eax, eax
    jnz fail
    xor ecx, ecx
    xor edx, edx
    call ImmSetCompositionFontW
    test eax, eax
    jnz fail
    xor ecx, ecx
    xor edx, edx
    call ImmSetCandidateWindow
    test eax, eax
    jnz fail
    mov qword [rsp+0x20], 0
    mov qword [rsp+0x28], 0
    xor ecx, ecx
    xor edx, edx
    xor r8d, r8d
    xor r9d, r9d
    call ImmSetCompositionStringW
    test eax, eax
    jnz fail
    xor ecx, ecx
    xor edx, edx
    xor r8d, r8d
    xor r9d, r9d
    call ImmEscapeW
    test rax, rax
    jnz fail
    xor ecx, ecx
    xor edx, edx
    xor r8d, r8d
    xor r9d, r9d
    call ImmNotifyIME
    test eax, eax
    jnz fail
    PRINT m_imm, m_imm_len

    ; A/W names resolve to one format, different names cannot alias.
    lea rcx, [name_a]
    call RegisterClipboardFormatA
    cmp eax, 0xC000
    jb fail
    mov [format_id], eax
    lea rcx, [other_a]
    call RegisterClipboardFormatA
    test eax, eax
    jz fail
    cmp eax, [format_id]
    je fail
    lea rcx, [name_w]
    call RegisterClipboardFormatW
    cmp eax, [format_id]
    jne fail
    PRINT m_clip, m_clip_len

    ; v6 manifest → live compositor palette; paint two BUTTON states into
    ; the same REAL GDI bitmap and assert the pixels differ.
    xor ecx, ecx
    lea rdx, [button_w]
    call OpenThemeData
    mov [theme], rax
    test rax, rax
    jz fail
    xor ecx, ecx
    call CreateCompatibleDC
    mov [dc], rax
    test rax, rax
    jz fail
    mov rcx, rax
    mov edx, 16
    mov r8d, 16
    call CreateCompatibleBitmap
    mov [bitmap], rax
    test rax, rax
    jz fail
    mov rcx, [dc]
    mov rdx, [bitmap]
    call SelectObject
    mov rcx, [theme]
    mov rdx, [dc]
    mov r8d, 1
    mov r9d, 1
    lea rax, [rect]
    mov [rsp+0x20], rax
    mov qword [rsp+0x28], 0
    call DrawThemeBackground
    test eax, eax
    jnz fail
    mov rcx, [dc]
    mov edx, 5
    mov r8d, 5
    call GetPixel
    mov [normal_color], eax
    cmp eax, 0xffffffff
    je fail
    mov rcx, [theme]
    mov rdx, [dc]
    mov r8d, 1
    mov r9d, 2
    lea rax, [rect]
    mov [rsp+0x20], rax
    mov qword [rsp+0x28], 0
    call DrawThemeBackground
    test eax, eax
    jnz fail
    mov rcx, [dc]
    mov edx, 5
    mov r8d, 5
    call GetPixel
    cmp eax, [normal_color]
    je fail
    mov rcx, [theme]
    mov rdx, [dc]
    mov r8d, 999
    mov r9d, 1
    lea rax, [rect]
    mov [rsp+0x20], rax
    mov qword [rsp+0x28], 0
    call DrawThemeBackground
    cmp eax, 0x80004001
    jne fail
    mov rcx, [theme]
    mov rdx, [dc]
    mov r8d, 1
    mov r9d, 2
    lea rax, [rect]
    mov [rsp+0x20], rax
    lea rax, [content]
    mov [rsp+0x28], rax
    call GetThemeBackgroundContentRect
    test eax, eax
    jnz fail
    cmp dword [content], 4
    jne fail
    mov rcx, [theme]
    call CloseThemeData
    test eax, eax
    jnz fail
    mov rcx, [bitmap]
    call DeleteObject
    mov rcx, [dc]
    call DeleteDC
    PRINT m_theme, m_theme_len
    PRINT m_final, m_final_len
    mov ecx, 78
    call ExitProcess
fail:
    PRINT m_fail, m_fail_len
    mov ecx, 79
    call ExitProcess
