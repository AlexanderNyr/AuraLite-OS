# =============================================================================
# kernel/arch/riscv64/boot.S -- rv64 kernel entry point (RISCV_PLAN V0;
# higher-half since V3).
#
# OpenSBI (M-mode firmware, D2) enters at _start in S-mode with:
#   a0 = hartid of the boot hart
#   a1 = physical address of the flattened device tree
#   satp = 0 (paging off), sstatus.SIE = 0
#
# V3 shape (boot32.asm's, one level deeper): everything below runs at
# VMA=LMA in .boot; it turns Sv39 on with a build-time-constant root
# table and long-jumps to _start_high in the higher-half .text, which
# clears .bss and calls kmain_rv.  The kernel never runs low again.
#
# The early root table is DATA, filled at assembly time: gigapage
# leaves are just constants, so no code has to build tables before a
# single allocator exists.
#   [2]       VA 0x80000000        -> PA 0x80000000  (identity, 1 GiB)
#             -- covers the instructions between `csrw satp` and the
#             jump high; paging_rv.c's final tables DROP it (V3 gate).
#   [256-259] VA HHDM+0..4 GiB     -> PA 0..4 GiB
#             -- the D3 direct map: RAM for the early kernel, MMIO
#             (UART/PLIC) for V2's bring-up, all RWX for now; the
#             final tables re-map everything with real permissions.
# PTE encoding: (PA >> 2) | flags; flags 0xCF = D|A|X|W|R|V (A/D set
# up front -- no Svade surprises, no first-touch faults).
#
# a0/a1 are live from OpenSBI until the kmain_rv call: only t*, sp
# touched here.
# =============================================================================

.option norvc                     # no compressed insns in the entry path;
                                  # keeps disassembly 1:1 with this file

.section .boot.data
.align 12                         # 2^12: the root table must be page-aligned
early_root:
    .fill 2,   8, 0
    .dword 0x200000CF             # [2]   identity gigapage @ 0x80000000
    .fill 253, 8, 0
    .dword 0x000000CF             # [256] HHDM+0 GiB -> PA 0
    .dword 0x100000CF             # [257] HHDM+1 GiB -> PA 1 GiB
    .dword 0x200000CF             # [258] HHDM+2 GiB -> PA 2 GiB
    .dword 0x300000CF             # [259] HHDM+3 GiB -> PA 3 GiB
    .fill 252, 8, 0

hart_lottery:
    .dword 0                      # 0 = unclaimed; first amoswap wins

# Literal pool: higher-half addresses are unreachable by auipc from a
# PC of 0x8020xxxx (the +-2 GiB medany window), so the jump target and
# stack pointer travel as 64-bit data instead of address arithmetic.
.align 3
boot_jump_pool:
    .dword _start_high            # VMA in the higher-half .text
    .dword boot_stack_top         # VMA in the higher-half .bss

# .text.boot, placed FIRST by kernelrv.ld -- and that placement is
# load-bearing, not cosmetic: measured during V0 bring-up, OpenSBI
# jumps to its "Domain0 Next Address" (0x80200000, the payload base),
# NOT to the ELF entry point (e_entry was 0x802003d4 and was ignored;
# the hart executed sbi.o's first function with garbage arguments).
# The ELF entry field still names _start for readers and debuggers,
# but the contract that actually boots the machine is "_start is the
# first byte of the first loaded segment".
.section .text.boot
.globl _start
_start:
    # ---- 1. One hart proceeds; the rest park. -------------------------
    la    t0, hart_lottery
    li    t1, 1
    amoswap.d t2, t1, (t0)        # t2 := old value, atomically set 1
    bnez  t2, .Lpark              # somebody already won

    # ---- 2. Sv39 on: satp = (mode 8 << 60) | PPN(early_root). ---------
    la    t0, early_root
    srli  t0, t0, 12
    li    t1, 8
    slli  t1, t1, 60
    or    t0, t0, t1
    csrw  satp, t0
    sfence.vma
    # PC is still 0x8020xxxx -- the identity gigapage [2] carries the
    # next two fetches; nothing else low is ever executed again.

    # ---- 3. Long jump to the higher half. ------------------------------
    la    t0, boot_jump_pool
    ld    t1, 0(t0)               # _start_high (VMA)
    ld    sp, 8(t0)               # boot stack top (VMA)
    jr    t1

.Lpark:
    wfi
    j     .Lpark

# =============================================================================
# The higher-half world.  From here every address is HHDM-shaped; la
# works again (PC and symbols are both up high).
# =============================================================================
.section .text
.globl _start_high
_start_high:
    # ---- 4. Zero .bss (the stack lives there; nothing is ON it yet;
    # a0/a1 are live and must survive -- t-registers only). -------------
    la    t0, __bss_start
    la    t1, __bss_end
.Lbss_clear:
    bgeu  t0, t1, .Lbss_done
    sd    zero, 0(t0)
    addi  t0, t0, 8
    j     .Lbss_clear
.Lbss_done:
    # The stack was just zeroed along with the rest of .bss; sp still
    # points at its top and nothing had been pushed.  Re-set anyway so
    # the invariant is visible rather than inherited:
    la    sp, boot_stack_top

    # ---- 5. kmain_rv(hartid=a0, dtb_phys=a1). --------------------------
    call  kmain_rv

    # ---- 6. Not reached in a healthy boot. -----------------------------
.Lpark_high:
    wfi
    j     .Lpark_high

# Physical/virtual layout, exported as data: C code compiled with
# -mcmodel=medany cannot take the address of an absolute low symbol
# like __kernel_start (auipc cannot span the HHDM gap), so the linker
# resolves them into this pool and C reads plain uint64_t[].
.section .rodata
.align 3
.globl kernel_layout
kernel_layout:
    .dword __kernel_start         # [0] image start, PHYS
    .dword __boot_end_phys        # [1] end of .boot, PHYS
    .dword __text_start           # [2] .text start, VMA
    .dword __text_end             # [3]
    .dword __rodata_start         # [4]
    .dword __rodata_end           # [5]
    .dword __data_start           # [6] .data start, VMA
    .dword __kernel_end           # [7] image end, PHYS

.section .bss
.align 16
boot_stack_bottom:
    .space 64 * 1024
.globl boot_stack_top
boot_stack_top:
