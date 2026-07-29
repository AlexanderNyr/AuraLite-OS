; =============================================================================
; boot/smp/ap_trampoline.asm -- Application Processor startup trampoline.
;
; Assembled to a flat binary (`nasm -f bin`; no linker, no ELF headers),
; embedded into the kernel as ap_trampoline_blob[] (see
; tools/gen_ap_trampoline_inc.py + build/ap_trampoline.inc), and copied by
; smp_init() (kernel/arch/x86_64/smp.c) to the fixed low physical address
; SMP_TRAMPOLINE_CODE_PHYS = 0x8000 before every SIPI.
;
; An AP starts here after receiving its Startup IPI: real mode, 16-bit, with
; CS base = SIPI_vector << 12 = 0x8000 and IP = 0 -- which is why this file
; uses `org 0x8000` (linear address == file offset once DS is zeroed).
;
; What this trampoline must do, in order:
;   1. Kill interrupts, set up known segments + a scratch stack.
;   2. Load a tiny private GDT good enough to reach 64-bit mode (it has NO
;      TSS descriptor; ap_entry() in smp.c installs the real per-CPU GDT/TSS
;      immediately on arrival).
;   3. Switch to the KERNEL'S OWN PML4 (physical address handed off in the
;      shared data page) so the AP shares the BSP's exact address space from
;      its very first 64-bit instruction.
;   4. Enable PAE (CR4), LME+NXE (EFER), then paging (CR0) and far-jump into
;      the 64-bit code segment.
;   5. Pull the per-AP stack, cpu index and C entry point out of the shared
;      handoff page and `jmp` into C (SysV ABI: first arg in RDI).
;
; Shared handoff data page contract (written by smp_init() per AP, read
; here; MUST match struct smp_handoff in kernel/arch/x86_64/smp.c):
;   SMP_TRAMPOLINE_DATA_PHYS + 0   dword  kernel PML4 physical address
;                                        (low 32 bits; page tables live
;                                         below 4 GiB on every config we
;                                         support)
;   SMP_TRAMPOLINE_DATA_PHYS + 8   qword  this AP's kernel stack top
;   SMP_TRAMPOLINE_DATA_PHYS + 16  qword  this AP's kernel-assigned CPU
;                                        index (-> RDI)
;   SMP_TRAMPOLINE_DATA_PHYS + 24  qword  C entry point address (ap_entry)
;
; This address must equal the SMP_TRAMPOLINE_DATA_PHYS #define in
; kernel/arch/x86_64/smp.c -- both sides document the contract.
; =============================================================================

bits 16
org 0x8000

SMP_TRAMPOLINE_DATA_PHYS equ 0x7000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                  ; scratch stack in a safely dead region

    lgdt [ap_gdt_ptr]               ; tiny private GDT (see below)

    ; CR3 <- the kernel's own PML4 (handoff +0, dword). The boot-time
    ; identity map in those page tables covers this low 1 MiB region, so the
    ; instruction stream keeps flowing once paging turns on.
    mov eax, [SMP_TRAMPOLINE_DATA_PHYS + 0]
    mov cr3, eax

    ; CR4.PAE (required for IA-32e paging)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; EFER: LME (long mode enable) + NXE (match the BSP's page-table NX bits)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr

    ; CR0: PG (bit 31) and PE (bit 0) set together -- paging comes up with
    ; protection already on, straight into compatibility mode of LME.
    mov eax, cr0
    or eax, (1 << 31) | 1
    mov cr0, eax

    ; Far jump: reload CS from the GDT's L-bit code descriptor -> 64-bit.
    jmp 0x08:.long64

bits 64
.long64:
    ; Known data segments; FS/GS take the null selector.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; A `mov gs, ax` above (and every future gdt_flush()) leaves the hidden
    ; GS.base alone only if it was already 0 -- an AP coming out of INIT is
    ; not guaranteed that. Zero both GS base MSRs explicitly so the per-CPU
    ; pointer cpu_local_init() installs later starts from a clean slate.
    mov ecx, 0xC0000101             ; IA32_GS_BASE
    xor eax, eax
    xor edx, edx
    wrmsr
    mov ecx, 0xC0000102             ; IA32_KERNEL_GS_BASE
    wrmsr                           ; EAX/EDX still 0

    ; Hand off to C: stack, cpu index (RDI), entry point.
    mov rsp, [SMP_TRAMPOLINE_DATA_PHYS + 8]
    mov rdi, [SMP_TRAMPOLINE_DATA_PHYS + 16]
    mov rax, [SMP_TRAMPOLINE_DATA_PHYS + 24]
    jmp rax

; ---- private GDT: null / 64-bit code / 64-bit data ------------------------
align 8
ap_gdt:
    dq 0x0000000000000000           ; 0x00 null
    dq 0x00AF9A000000FFFF           ; 0x08 code64: present, ring0, exec/read,
                                    ;       L=1 (long mode), D=0
    dq 0x00CF92000000FFFF           ; 0x10 data64: present, ring0, writable
ap_gdt_ptr:
    dw ap_gdt_ptr - ap_gdt - 1      ; limit
    dd ap_gdt                       ; base (32-bit is enough: we're < 1 MiB)
