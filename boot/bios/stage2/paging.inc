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
;   0x01000000  PML4
;   0x01001000  PDPT0    (identity map)
;   0x01002000  PDPT256  (HHDM base 0xffff800000000000)
;   0x01003000  PDPT511  (kernel higher half 0xFFFFFFFF80000000)
;   0x01004000  PD0      (2 MiB large pages for physical 0..1 GiB)
;   0x01005000  PD1      (2 MiB large pages for physical 1..2 GiB)
;   0x01006000  PD2      (2 MiB large pages for physical 2..3 GiB)
;   0x01007000  PD3      (2 MiB large pages for physical 3..4 GiB)
;   0x01008000  PD511    (2 MiB large pages for the kernel image)
;
; Total: 9 pages = 36 KiB.  Sits well above kernel image extents
; (~2 MiB from 0x100000) with a fat safety margin.
; =============================================================================

PT_BASE        equ 0x01000000
PML4_ADDR      equ PT_BASE + 0x0000
PDPT0_ADDR     equ PT_BASE + 0x1000
PDPT256_ADDR   equ PT_BASE + 0x2000
PDPT511_ADDR   equ PT_BASE + 0x3000
PD0_ADDR       equ PT_BASE + 0x4000
PD1_ADDR       equ PT_BASE + 0x5000
PD2_ADDR       equ PT_BASE + 0x6000
PD3_ADDR       equ PT_BASE + 0x7000
PD511_ADDR     equ PT_BASE + 0x8000
PT_TOTAL_BYTES equ 9 * 4096

; Page-table entry flags.
PTE_P          equ 0x001                    ; present
PTE_W          equ 0x002                    ; writable
PTE_PS         equ 0x080                    ; page size (2 MiB / 1 GiB)

; ---------------------------------------------------------------------------
; build_page_tables -- assemble the three PT hierarchies at PT_BASE.
;
; On entry: unreal mode active (FS = flat 4 GiB), interrupts safe.
; On exit : PML4 / PDPTs / PDs are written; CR3 is NOT loaded.
;           EAX / EBX / ECX / EDX / EDI clobbered.
; ---------------------------------------------------------------------------
build_page_tables:
    pushad

    ; Zero all table memory via FS dword stores.
    mov  edi, PT_BASE
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
    mov  edi, PML4_ADDR
    mov  eax, PDPT0_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 0*8], eax
    mov  dword [fs:edi + 0*8 + 4], 0        ; upper 32 bits

    mov  eax, PDPT256_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 256*8], eax
    mov  dword [fs:edi + 256*8 + 4], 0

    mov  eax, PDPT511_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 511*8], eax
    mov  dword [fs:edi + 511*8 + 4], 0

    ; ---- PDPT0[0..3] -> PD0..PD3 (identity map, first 4 GiB) ----------
    mov  edi, PDPT0_ADDR
    mov  eax, PD0_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 0*8], eax
    mov  dword [fs:edi + 0*8 + 4], 0
    mov  eax, PD1_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 1*8], eax
    mov  dword [fs:edi + 1*8 + 4], 0
    mov  eax, PD2_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 2*8], eax
    mov  dword [fs:edi + 2*8 + 4], 0
    mov  eax, PD3_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 3*8], eax
    mov  dword [fs:edi + 3*8 + 4], 0

    ; ---- PDPT256[0..3] -> PD0..PD3 (HHDM shares identity leaves) -------
    mov  edi, PDPT256_ADDR
    mov  eax, PD0_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 0*8], eax
    mov  dword [fs:edi + 0*8 + 4], 0
    mov  eax, PD1_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 1*8], eax
    mov  dword [fs:edi + 1*8 + 4], 0
    mov  eax, PD2_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 2*8], eax
    mov  dword [fs:edi + 2*8 + 4], 0
    mov  eax, PD3_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 3*8], eax
    mov  dword [fs:edi + 3*8 + 4], 0

    ; ---- PDPT511[510] -> PD511 (kernel virt 0xFFFFFFFF80000000) --------
    ; The kernel higher half sits in PML4[511], PDPT[510], PD[0..].
    ; Compute 510 * 8 = 4080.
    mov  edi, PDPT511_ADDR + 510*8
    mov  eax, PD511_ADDR | (PTE_P | PTE_W)
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0

    ; ---- PD0: 512 x 2-MiB large pages covering physical 0..1 GiB -------
    ; Flags: PS | W | P = 0x83.  Each entry increments phys by 2 MiB.
    mov  edi, PD0_ADDR
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
    mov  edi, PD1_ADDR
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
    mov  edi, PD2_ADDR
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
    mov  edi, PD3_ADDR
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

    ; ---- PD511[0..1]: 2 x 2-MiB pages mapping virt 0xFFFFFFFF80000000
    ;                   .. +4 MiB to physical 0 .. 4 MiB.  Kernel image
    ;                   sits at phys 0x100000 which is inside the first
    ;                   2-MiB page. ---------------------------------------
    mov  edi, PD511_ADDR
    mov  eax, 0x00000000 | (PTE_PS | PTE_P | PTE_W)
    mov  [fs:edi + 0], eax
    mov  dword [fs:edi + 4], 0
    mov  eax, 0x00200000 | (PTE_PS | PTE_P | PTE_W)
    mov  [fs:edi + 8], eax
    mov  dword [fs:edi + 12], 0

    popad
    ret
