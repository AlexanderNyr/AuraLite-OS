; usercopy_fault.asm — fault-recoverable uaccess copy primitive.
;
; int uaccess_copy_asm(void *dst, const void *src, uint64_t len)
;
; The C wrapper enables SMAP user access before calling us and disables it after
; return.  While REP MOVSB is active, the per-CPU uaccess_recover_ip_percpu slot points at .fault.  The
; page-fault handler rewrites RIP to that address for kernel #PFs in this window,
; making the function return -1 instead of panicking the kernel.

bits 64
default rel

section .text

global uaccess_copy_asm
extern uaccess_recover_ip_percpu

uaccess_copy_asm:
    mov r8, [gs:8]                 ; current cpu_id
    cmp r8, 63
    jbe .cpu_ok
    xor r8, r8
.cpu_ok:
    lea r9, [rel uaccess_recover_ip_percpu]
    lea rax, [rel .fault]
    mov [r9 + r8*8], rax
    mov rcx, rdx
    rep movsb
    mov qword [r9 + r8*8], 0
    xor eax, eax
    ret

.fault:
    mov r8, [gs:8]
    cmp r8, 63
    jbe .fault_cpu_ok
    xor r8, r8
.fault_cpu_ok:
    lea r9, [rel uaccess_recover_ip_percpu]
    mov qword [r9 + r8*8], 0
    mov rax, -1
    ret
