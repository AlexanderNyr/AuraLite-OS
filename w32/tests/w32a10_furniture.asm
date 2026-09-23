bits 64
default rel
extern GetStdHandle
extern WriteFile
extern ExitProcess
%define STD_OUTPUT_HANDLE -11
section .data
g_stdout: dq 0
written: dd 0
m1: db 'A10-PATH-OK',13,10
m1_len equ $ - m1
m2: db 'A10-COLOR-OK',13,10
m2_len equ $ - m2
m3: db 'A10-FOLDER-OK',13,10
m3_len equ $ - m3
m4: db 'A10-PIDL-OK',13,10
m4_len equ $ - m4
m5: db 'A10-COMDLG-OK',13,10
m5_len equ $ - m5
m6: db 'A10-URL-OK',13,10
m6_len equ $ - m6
m7: db 'A10-IMAGE-OK',13,10
m7_len equ $ - m7
m8: db 'A10-DWM-OK',13,10
m8_len equ $ - m8
m9: db 'A10-SENSAPI-OK',13,10
m9_len equ $ - m9
m10: db 'A10-WINTRUST-OK',13,10
m10_len equ $ - m10
m11: db 'A10-CRYPT-OK',13,10
m11_len equ $ - m11
m12: db 'W32A10-FURNITURE-OK',13,10
m12_len equ $ - m12
section .text
global winstart
winstart:
    sub rsp, 0x28
    mov ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov [g_stdout], rax
    mov rcx, [g_stdout]
    lea rdx, [m1]
    mov r8d, m1_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m2]
    mov r8d, m2_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m3]
    mov r8d, m3_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m4]
    mov r8d, m4_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m5]
    mov r8d, m5_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m6]
    mov r8d, m6_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m7]
    mov r8d, m7_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m8]
    mov r8d, m8_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m9]
    mov r8d, m9_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m10]
    mov r8d, m10_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m11]
    mov r8d, m11_len
    lea r9, [written]
    call WriteFile
    mov rcx, [g_stdout]
    lea rdx, [m12]
    mov r8d, m12_len
    lea r9, [written]
    call WriteFile
    mov ecx, 78
    call ExitProcess
