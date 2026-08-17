# =============================================================================
# kernel/arch/riscv64/boot.S -- rv64 kernel entry point (RISCV_PLAN V0).
#
# The third architecture's first instructions.  OpenSBI (the M-mode
# platform firmware -- RISCV_PLAN D2) enters here in S-mode with:
#   a0 = hartid of the boot hart
#   a1 = physical address of the flattened device tree
#   satp = 0 (paging off), sstatus.SIE = 0
#
# GNU as syntax via clang's integrated assembler: there is no NASM on
# this arch, and no reason to want one -- the .S path also gets the
# C preprocessor for free.
#
# Responsibilities, in order (the boot32.asm shape, third verse):
#   1. Park non-boot harts.  QEMU virt with -smp > 1 enters ALL harts
#      here at once (unlike x86, where APs wait for a SIPI dance);
#      anyone who is not the boot hart goes to wfi immediately and
#      stays there until SBI HSM starts them on purpose (plan D5).
#      "Boot hart" = whoever wins the lottery ticket at hart_lottery:
#      the DTB does not promise hartid 0 boots first.
#   2. sp := boot stack top (64 KiB in .bss).
#   3. Zero .bss -- but the lottery word and the stack live there, so
#      the clear runs BEFORE the stack is used for calls and SKIPS the
#      lottery (it sits in .data instead, dodging the problem).
#   4. Call kmain_rv(hartid, dtb_phys) -- a0/a1 pass through untouched.
#   5. If kmain_rv returns: wfi loop.
# =============================================================================

.option norvc                     # no compressed insns in the entry path;
                                  # keeps disassembly 1:1 with this file

.section .data
hart_lottery:
    .dword 0                      # 0 = unclaimed; first amoswap wins

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

    # ---- 2. Stack for the boot hart. ----------------------------------
    la    sp, boot_stack_top

    # ---- 3. Zero .bss (the stack is in .bss but nothing is ON it yet;
    # a0/a1 are live and must survive -- use t-registers only). ---------
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

    # ---- 4. kmain_rv(hartid=a0, dtb_phys=a1). --------------------------
    call  kmain_rv

    # ---- 5. Not reached in a healthy boot. -----------------------------
.Lpark:
    wfi
    j     .Lpark

.section .bss
.align 16
boot_stack_bottom:
    .space 64 * 1024
.globl boot_stack_top
boot_stack_top:
