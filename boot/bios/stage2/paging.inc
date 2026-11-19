; =============================================================================
; boot/bios/stage2/paging.inc -- build 4-level x86_64 page tables.
;
; Requires unreal mode (FS = flat).  Assembles page tables at the
; physical addresses below and installs CR3, but does NOT enable
; paging.  Long-mode entry (longmode.inc) is responsible for the
; PAE/LME/PG bring-up sequence.
;
; Three virtual regions are mapped:
;
;   1. Identity map: virt 0x0000000000000000 .. 0x100000000 (4 GiB)
;      -> phys 0x0..0x100000000.  Lets Stage 2 keep executing after
;      paging turns on and lets early kernel code touch low physical RAM.
;
;   2. HHDM (Higher Half Direct Map):
;      virt 0xffff800000000000 .. +4 GiB  ->  phys 0..4 GiB.
;      The kernel adds hhdm_offset to every physical address to
;      reach memory once paging is active.
;
;   3. Kernel higher half:
;      virt 0xFFFFFFFF80000000 .. +4 MiB  ->  phys 0..4 MiB.
;      Two 2-MiB large pages cover the kernel image which loads at
;      phys 0x100000 and is < 4 MiB total.
;
; Physical layout of the tables (all 4 KiB each, all pre-zeroed):
;
;   0x00010000  -- boot_info_t (already there)
;   0x00011000  -- Stage 2 scratch (BPB / FAT / dir sector)
;   PT_BASE     PML4
;   PT_BASE+1000 PDPT0   (identity map)
;   0x01002000  PDPT256  (HHDM base 0xffff800000000000)
;   0x01003000  PDPT511  (kernel higher half 0xFFFFFFFF80000000)
;   0x01004000  PD0      (2 MiB large pages for physical 0..1 GiB)
;   0x01005000  PD1      (2 MiB large pages for physical 1..2 GiB)
;   0x01006000  PD2      (2 MiB large pages for physical 2..3 GiB)
;   0x01007000  PD3      (2 MiB large pages for physical 3..4 GiB)
;   0x01008000  PD511    (2 MiB large pages for the kernel image)
;
; Total: 9 pages = 36 KiB.
;
; WHY PT_BASE IS LOADED LAST (2026-11-18).  The tables used to sit at a
; constant 0x01700000 with the comment "well above kernel image extents
; (~2 MiB)".  That was true when the kernel was 2 MiB; it is not true now:
; the kernel's .data/.bss PT_LOAD reaches 0x0193f470, so build_page_tables
; was writing 36 KiB of page tables straight into the middle of the kernel
; (0x01700000 is inside [0x100000, 0x193f470)), and then loading that
; clobbered memory into CR3.  The guest died at _start having printed
; nothing -- the entire symptom of run 120066423839.
;
; So the tables are now placed at run time by stage2_start.asm, after
; elf_load and after the initrd, using the same kernel extent that places
; the initrd: pt_base = align_2MiB(max(kernel_end, initrd_end)) + 2 MiB.
; The constant below is only the floor for small kernels, and
; paging_inc_set_base below is how stage2 moves it.
; =============================================================================

PT_BASE_DEFAULT equ 0x01700000
; Runtime base.  stage2_start.asm writes the chosen value here before
; calling build_page_tables, and longmode.inc reads it for CR3.  It is a
; memory cell, not an equ, precisely because the right answer depends on
; how big the kernel turned out to be.
pt_base:        dd PT_BASE_DEFAULT
PML4_ADDR_OFF   equ 0x0000
PDPT0_ADDR_OFF   equ 0x1000
PDPT256_ADDR_OFF   equ 0x2000
PDPT511_ADDR_OFF   equ 0x3000
PD0_ADDR_OFF   equ 0x4000
PD1_ADDR_OFF   equ 0x5000
PD2_ADDR_OFF   equ 0x6000
PD3_ADDR_OFF   equ 0x7000
PD511_ADDR_OFF   equ 0x8000
PT_TOTAL_BYTES equ 9 * 4096

; Page-table entry flags.
PTE_P          equ 0x001                    ; present
PTE_W          equ 0x002                    ; writable
PTE_PS         equ 0x080                    ; page size (2 MiB / 1 GiB)

; Kernel higher-half window: 16 x 2 MiB = 32 MiB, mirroring the UEFI loader
; (boot/uefi/efi_paging.c).  The SDF font atlas (kernel/gui/sdf/atlas.bin,
; ~12 MiB) is include_bytes!'d into .rodata, so the kernel image is now ~17 MiB
; and the old 3-page (6 MiB) window left .data/.bss/.got unmapped.
KERNEL_WINDOW_PAGES equ 16

; ---------------------------------------------------------------------------
; build_page_tables -- assemble the three PT hierarchies at PT_BASE.
;
; On entry: unreal mode active (FS = flat 4 GiB), interrupts safe.
; On exit : PML4 / PDPTs / PDs are written; CR3 is NOT loaded.
;           EAX / EBX / ECX / EDX / EDI clobbered.
; ---------------------------------------------------------------------------
build_page_tables:
    pushad
    mov  ebp, [pt_base]                   ; EBP = chosen table base

    ; Zero all table memory via FS dword stores.
    mov  edi, ebp
    mov  ecx, PT_TOTAL_BYTES / 4
    xor  eax, eax
.zloop:
    mov  [fs:edi], eax
    add  edi, 4
    dec  ecx
    jnz  .zloop

    ; ---- PML4 entries --------------------------------------------------
    ; PML4[0]   -> PDPT0    (identity map)
    ; PML4[256] -> PDPT256  (HHDM,  virt = 0xffff800000000000)
    ; PML4[511] -> PDPT511  (kernel higher half)
    mov  edi, ebp
    lea  eax, [ebp + PDPT0_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 0*8], eax
    mov  dword [fs:edi + 0*8 + 4], 0        ; upper 32 bits

    lea  eax, [ebp + PDPT256_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 256*8], eax
    mov  dword [fs:edi + 256*8 + 4], 0

    lea  eax, [ebp + PDPT511_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 511*8], eax
    mov  dword [fs:edi + 511*8 + 4], 0

    ; ---- PDPT0[0..3] -> PD0..PD3 (identity map, first 4 GiB) ----------
    lea  edi, [ebp + PDPT0_ADDR_OFF]
    lea  eax, [ebp + PD0_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 0*8], eax
    mov  dword [fs:edi + 0*8 + 4], 0
    lea  eax, [ebp + PD1_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 1*8], eax
    mov  dword [fs:edi + 1*8 + 4], 0
    lea  eax, [ebp + PD2_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 2*8], eax
    mov  dword [fs:edi + 2*8 + 4], 0
    lea  eax, [ebp + PD3_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 3*8], eax
    mov  dword [fs:edi + 3*8 + 4], 0

    ; ---- PDPT256[0..3] -> PD0..PD3 (HHDM shares identity leaves) -------
    lea  edi, [ebp + PDPT256_ADDR_OFF]
    lea  eax, [ebp + PD0_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 0*8], eax
    mov  dword [fs:edi + 0*8 + 4], 0
    lea  eax, [ebp + PD1_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 1*8], eax
    mov  dword [fs:edi + 1*8 + 4], 0
    lea  eax, [ebp + PD2_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 2*8], eax
    mov  dword [fs:edi + 2*8 + 4], 0
    lea  eax, [ebp + PD3_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 3*8], eax
    mov  dword [fs:edi + 3*8 + 4], 0

    ; ---- PDPT511[510] -> PD511 (kernel virt 0xFFFFFFFF80000000) --------
    ; The kernel higher half sits in PML4[511], PDPT[510], PD[0..].
    ; Compute 510 * 8 = 4080.
    lea  edi, [ebp + PDPT511_ADDR_OFF + 510*8]
    lea  eax, [ebp + PD511_ADDR_OFF]
    or   eax, (PTE_P | PTE_W)
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0

    ; ---- PD0: 512 x 2-MiB large pages covering physical 0..1 GiB -------
    ; Flags: PS | W | P = 0x83.  Each entry increments phys by 2 MiB.
    lea  edi, [ebp + PD0_ADDR_OFF]
    mov  ebx, 0x00000000                    ; running phys address (32-bit)
    mov  ecx, 512
.pd0:
    mov  eax, ebx
    or   eax, (PTE_PS | PTE_P | PTE_W)      ; 0x83
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0
    add  ebx, 0x00200000                    ; +2 MiB
    add  edi, 8
    dec  ecx
    jnz  .pd0

    ; ---- PD1: 512 x 2-MiB large pages covering physical 1..2 GiB -------
    lea  edi, [ebp + PD1_ADDR_OFF]
    mov  ebx, 0x40000000
    mov  ecx, 512
.pd1:
    mov  eax, ebx
    or   eax, (PTE_PS | PTE_P | PTE_W)
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0
    add  ebx, 0x00200000
    add  edi, 8
    dec  ecx
    jnz  .pd1

    ; ---- PD2: 512 x 2-MiB large pages covering physical 2..3 GiB -------
    lea  edi, [ebp + PD2_ADDR_OFF]
    mov  ebx, 0x80000000
    mov  ecx, 512
.pd2:
    mov  eax, ebx
    or   eax, (PTE_PS | PTE_P | PTE_W)
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0
    add  ebx, 0x00200000
    add  edi, 8
    dec  ecx
    jnz  .pd2

    ; ---- PD3: 512 x 2-MiB large pages covering physical 3..4 GiB -------
    lea  edi, [ebp + PD3_ADDR_OFF]
    mov  ebx, 0xC0000000
    mov  ecx, 512
.pd3:
    mov  eax, ebx
    or   eax, (PTE_PS | PTE_P | PTE_W)
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0
    add  ebx, 0x00200000
    add  edi, 8
    dec  ecx
    jnz  .pd3

    ; ---- PD511[0..15]: 16 x 2-MiB pages mapping virt 0xFFFFFFFF80000000
    ;                   .. +32 MiB to physical 0 .. 32 MiB.  Kernel image
    ;                   sits at phys 0x100000 which is inside the first
    ;                   2-MiB page.  History: the window was 2 x 2 MiB
    ;                   (4 MiB); SELFHOST SH5c widened it to 3 x 2 MiB
    ;                   (6 MiB) because the tcc-built kernel needs 4.27 MiB.
    ;                   The Rust SDF renderer (kernel/gui/sdf/lib.rs)
    ;                   include_bytes!'s a 12.6 MiB atlas.bin into .rodata,
    ;                   which took the clang kernel to ~17 MiB and made
    ;                   .data/.bss/.got fall outside a 6 MiB window -- the
    ;                   kernel then faulted in _start's .bss zero-fill with
    ;                   no IDT yet, i.e. a silent triple fault.  32 MiB
    ;                   mirrors the UEFI loader (efi_paging.c maps the same
    ;                   16 entries); the PMM keeps the low 56 MiB out of
    ;                   the allocator, so the wider window costs nothing.
    lea  edi, [ebp + PD511_ADDR_OFF]
    mov  ebx, 0x00000000                    ; running phys address
    mov  ecx, KERNEL_WINDOW_PAGES
.pd511:
    mov  eax, ebx
    or   eax, (PTE_PS | PTE_P | PTE_W)
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0
    add  ebx, 0x00200000
    add  edi, 8
    dec  ecx
    jnz  .pd511

    popad
    ret
