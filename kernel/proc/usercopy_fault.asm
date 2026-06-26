; usercopy_fault.asm — fault-recoverable uaccess copy primitive.
;
; int uaccess_copy_asm(void *dst, const void *src, uint64_t len)
;
; The C wrapper enables SMAP user access before calling us and disables it after
; return.  While REP MOVSB is active, uaccess_recover_ip points at .fault.  The
; page-fault handler rewrites RIP to that address for kernel #PFs in this window,
; making the function return -1 instead of panicking the kernel.

bits 64
default rel

section .text

global uaccess_copy_asm
extern uaccess_recover_ip

uaccess_copy_asm:
    lea rax, [rel .fault]
    mov [rel uaccess_recover_ip], rax
    mov rcx, rdx
    rep movsb
    mov qword [rel uaccess_recover_ip], 0
    xor eax, eax
    ret

.fault:
    mov qword [rel uaccess_recover_ip], 0
    mov rax, -1
    ret
