#!/usr/bin/env python3
"""Cross-check ARM64_PLAN.md's status against the tree.

Decision D8 of the plan it checks -- now a tradition, third checker in
the family: check_i386_claims.py shipped with its plan's completion,
check_riscv_claims.py shipped in V0, and this one ships in A0 for the
same reason.  A plan checked from birth cannot drift at all.

Same structure as its predecessors: each claim ties a phase to an
artefact that only exists if the phase happened; the plan's Status
header is checked against its own phase table; --selftest proves the
checks can fail (against a doctored ROOT).

Usage:
    tools/check_arm64_claims.py [--selftest]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def exists(*parts):
    return os.path.exists(os.path.join(ROOT, *parts))


def claims():
    plan   = read("ARM64_PLAN.md")
    makefl = read("Makefile")
    boots  = read("kernel", "arch", "aarch64", "boot.S")
    ld     = read("kernel", "arch", "aarch64", "kernela64.ld")
    mainc  = read("kernel", "arch", "aarch64", "main_a64.c")
    psci   = read("kernel", "arch", "aarch64", "psci.c")
    smoke  = read("tests", "integration", "a64_boot_smoke.sh")

    checks = [
        # --- A0: toolchain gates + the EL1 stub ---
        ("A0: the a64 entry, PL011/PSCI layers and linker script exist",
         exists("kernel", "arch", "aarch64", "boot.S") and
         exists("kernel", "arch", "aarch64", "pl011.c") and
         exists("kernel", "arch", "aarch64", "psci.c") and
         exists("kernel", "arch", "aarch64", "kernela64.ld")),
        ("A0: the Makefile has the kernela64 target with the a64 flags",
         "kernela64" in makefl and "aarch64-unknown-none-elf" in makefl and
         "aarch64linux" in makefl),
        ("A0: -mstrict-align is on (the pre-MMU Device-memory fact)",
         "-mstrict-align" in makefl),
        ("A0: the EL1 check refuses non-EL1 entry (D1)",
         "CurrentEL" in boots and "refusing to boot" in boots),
        ("A0: .text.boot placed first (the shared four-kernel discipline)",
         ".text.boot" in ld and ".text.boot" in boots),
        ("A0: the DTB is probed at the RAM base, not taken from x0 "
         "(the measured Fact 2 trap)",
         "0x40000000" in mainc and "fdt_magic_at_ram_base" in mainc and
         "not the DTB pointer" in mainc),
        ("A0: PSCI SYSTEM_OFF over hvc ends every healthy boot",
         "0x84000008" in psci and "hvc #0" in psci and
         "psci_system_off" in mainc),
        ("A0: boot smoke covers banner, EL1, x0=0, magic, power-off, "
         "-smp 4 and the EL2 refusal",
         "CurrentEL: EL1" in smoke and
         "x0 at entry: 0x0000000000000000" in smoke and
         "virtualization=on" in smoke),
        ("A0: this checker is registered in test-unit",
         "check_arm64_claims.py" in makefl),
    ]

    # --- A1: boot_info_t from the Device Tree, the walker promoted ---
    fdt_c  = read("kernel", "dt", "fdt.c")
    fdt_h  = read("kernel", "dt", "fdt.h")
    checks += [
        ("A1: the walker lives in kernel/dt/ and the riscv copy is GONE",
         exists("kernel", "dt", "fdt.c") and
         not exists("kernel", "arch", "riscv64", "fdt.c") and
         not exists("kernel", "arch", "riscv64", "fdt.h")),
        ("A1: BOTH kernels compile the shared walker (single-object "
         "promotion, not a fork)",
         "KERNELRV_SHARED := kernel/net/miniproto.c kernel/dt/fdt.c"
         in makefl and
         "KERNELA64_SHARED := kernel/dt/fdt.c" in makefl),
        ("A1: interrupt normalisation is central -- SPI+32/PPI+16 in "
         "the walker, never in a driver",
         "nr + 32" in fdt_c and "nr + 16" in fdt_c and
         "NORMALISED" in fdt_h),
        ("A1: the per-depth device-state lesson is recorded (the GIC's "
         "v2m child wiped the scalar)",
         "v2m" in fdt_c and "Per-DEPTH, not per-walk" in fdt_c and
         "ndev[depth]" in fdt_c),
        ("A1: the PSCI conduit is asserted against the tree, not assumed",
         "psci_method" in fdt_h and
         "FDT_PSCI_HVC" in read("kernel", "arch", "aarch64", "main_a64.c")),
        ("A1: both arches define the dt_phys_to_virt contract",
         "dt_phys_to_virt" in read("kernel", "arch", "riscv64",
                                   "main_rv.c") and
         "dt_phys_to_virt" in read("kernel", "arch", "aarch64",
                                   "main_a64.c")),
        ("A1: the smoke asserts normalised INTIDs and the shared-walker "
         "boot_info lines",
         "irq 33" in smoke and "virtio-mmio windows: 32" in smoke and
         "handoff magic OK" in smoke),
    ]

    # --- A2: exceptions, the generic timer, GICv2 ---
    vecs  = read("kernel", "arch", "aarch64", "vectors.S")
    trapc = read("kernel", "arch", "aarch64", "trap_a64.c")
    gicc_ = read("kernel", "arch", "aarch64", "gic.c")
    checks += [
        ("A2: the 16x128 vector table with one shared spill path",
         ".balign 2048" in vecs and ".balign 128" in vecs and
         "a64_trap_common" in vecs and "eret" in vecs),
        ("A2: the tag formula is position-major and its lesson is "
         "recorded (one formula, used twice, or none)",
         "origin * 4 + \\kind" in vecs and
         "dispatched as if it were another row" in vecs),
        ("A2: SPSel declared, not inherited -- the measured QEMU "
         "SPSel=0 entry fact lives in boot.S",
         "msr   spsel, #1" in boots and "MEASURED in A2" in boots),
        ("A2: ESR decode names the classes; unexpected rows halt "
         "loudly by vector name",
         "ec_name" in trapc and "UNEXPECTED vector row" in trapc and
         "kind_names" in trapc),
        ("A2: the timer is CNTFRQ-derived (a register, not a DTB "
         "field) with the TVAL re-arm property named",
         "cntfrq_read" in trapc and "TICK_HZ" in trapc and
         "un-asserts the line" in trapc),
        ("A2: the timer INTID is arithmetic with a paper trail, "
         "never a bare 27",
         "11u + 16u" in trapc),
        ("A2: the GIC completes even unhandled claims (no stuck "
         "gateway -- the plic_dispatch rule)",
         "EOIR" in gicc_ and "even for a line without" in gicc_),
        ("A2: the smoke gates isr/timer/gic/rng PASS lines",
         "resumed" in smoke and "ticks observed at 100 Hz" in smoke and
         "claim/complete" in smoke and "jitter" in smoke),
        ("A2: the alignment premise is probed, not trusted "
         "(-mstrict-align has a measured reason)",
         "trap_alignment_probe_a64" in trapc and
         "alignment" in smoke),
    ]

    # --- A3: TTBR1 39-bit VA, PMM, heap -- W^X twice over ---
    pagec = read("kernel", "arch", "aarch64", "paging_a64.c")
    pageh = read("kernel", "arch", "aarch64", "paging_a64.h")
    pmmc  = read("kernel", "arch", "aarch64", "pmm_a64.c")
    ldscr = read("kernel", "arch", "aarch64", "kernela64.ld")
    rvh   = read("kernel", "arch", "riscv64", "paging_rv.h")
    checks += [
        ("A3: the linker script is higher-half (HHDM VMA, physical LMA)",
         "HHDM = 0xFFFFFFC000000000" in ldscr and "AT(ADDR(" in ldscr),
        ("A3: boot.S turns the MMU on before any C runs, with the "
         "barrier discipline",
         "early_ttbr1" in boots and "msr   sctlr_el1, x1" in boots and
         "dsb   ish" in boots),
        ("A3/D3: HHDM_OFFSET equals riscv64's BY VALUE and the "
         "arithmetic argument is written down",
         "#define HHDM_OFFSET 0xFFFFFFC000000000UL" in pageh and
         "#define HHDM_OFFSET 0xFFFFFFC000000000UL" in rvh and
         "index 256" in pageh),
        ("A3: MAIR has exactly the two planned indices and mappings "
         "spell attributes through kinds, not raw bits",
         "MAIR_IDX_DEVICE" in pageh and "MAIR_IDX_NORMAL" in pageh and
         "A64_MAP_RW_DEVICE" in pagec),
        ("A3: the TLB barrier discipline lives in ONE helper",
         "tlb_flush_all" in pagec and
         pagec.count("tlbi vmalle1") == 1),
        ("A3: W^X twice over -- kernel text is UXN, nothing is W+X",
         "PTE_UXN" in pagec and "PTE_PXN" in pagec and
         "W^X holds twice over" in pagec),
        ("A3: the identity window dies by BLANK-at-switch TTBR0 (the "
         "register-flavoured drop, argued in the file; A4 made the "
         "root a live user tree and the claim tracked the change)",
         "root_lo" in pagec and "identity window dies" in pagec and
         "at THIS moment it is empty" in pagec),
        ("A3: the PMM is bitmap.h's fourth consumer, header unedited",
         'include "kernel/lib/bitmap.h"' in pmmc and
         "pmm_a64" not in read("kernel", "lib", "bitmap.h")),
        ("A3: the fault probes unwind by setjmp (elr += 4 cannot "
         "resume execute-from-data)",
         "a64_setjmp" in vecs and "a64_longjmp_entry" in trapc and
         "trap_run_fault_probe_a64" in pagec),
        ("A3: the TCG mapped-Device alignment behaviour is pinned, "
         "with the strict-align consequence named",
         "strict-align stays" in read("kernel", "arch", "aarch64",
                                      "main_a64.c") and
         "not faulted by TCG" in smoke),
        ("A3: the smoke gates pmm/vmm/heap and all three fault probes",
         "store to .text faulted" in smoke and
         "execute-from-data faulted" in smoke and
         "identity window confirmed dropped" in smoke and
         "64 alloc/free cycles" in smoke),
    ]

    # --- A4: threads, scheduler, EL0, svc ---
    ctx    = read("kernel", "arch", "aarch64", "context_a64.S")
    thrc   = read("kernel", "arch", "aarch64", "thread_a64.c")
    userc  = read("kernel", "arch", "aarch64", "user_a64.c")
    userh  = read("kernel", "arch", "aarch64", "user_a64.h")
    mainc2 = read("kernel", "arch", "aarch64", "main_a64.c")
    checks += [
        ("A4: the context switch saves FPU state EAGERLY (the M1 "
         "lesson, 528 bytes, lazy-save refused)",
         "q30, q31" in ctx and "fpcr" in ctx and "EAGERLY" in ctx and
         "624" in ctx),
        ("A4: CPACR opens the FPU before any switch exists, with the "
         "reason written down",
         "cpacr_el1" in boots and "EC 0x07" in boots),
        ("A4: the trap-stack contract is the hardware's SPSel switch "
         "-- the I7 esp0 lesson costs zero instructions here",
         "user_enter_a64" in ctx and "SPSel" in ctx and
         "hardware feature" in ctx),
        ("A4: preemption is post-EOI (after gic_dispatch returns), "
         "with the phase-6 freeze lineage",
         "sched_a64_maybe_preempt" in trapc and
         "post-EOI" in thrc),
        ("A4: D4 numbers spot-checked -- GETPID 39, EXIT 60, "
         "YIELD 158, and the Linux-aarch64-diverges note",
         "SYS_A64_GETPID  39" in userh and "SYS_A64_EXIT    60" in userh
         and "SYS_A64_YIELD   158" in userh and
         "diverge" in userh),
        ("A4: the EL0 exit unwind reuses the A3 setjmp pair (one "
         "mechanism, two tenants) with IRQ masked across it",
         "a64_longjmp_entry" in userc and "0x3C5" in userc),
        ("A4: the two-trees fact is recorded where it was measured "
         "(user VAs walk TTBR0's tree, not TTBR1's)",
         "WRONG TREE" in read("kernel", "arch", "aarch64",
                              "paging_a64.c")),
        ("A4: EL0 user programs are measured bytes, not hand-rolled "
         "(the assembler is the reviewer)",
         "MEASURED, not hand-rolled" in userc),
        ("A4: the smoke gates sched/fpu/user including the exact "
         "EL0 output string and the negative control",
         "A64-U-OK!" in smoke and "never-yielding workers" in smoke and
         "q8/q9 survived" in smoke and "privileged op contained" in smoke),

        # --- A5a: the Image exit ramp + the shared strings ---
        ("A5a: boot.S carries the arm64 Image header (magic + "
         "text_offset placing the Image at the ELF link address)",
         'ARM\\x64' in boots and "0x200000" in boots and
         "__image_size" in boots),
        ("A5a: kernela64.ld computes image_size by linker arithmetic",
         "__image_size" in ld),
        ("A5a: the Makefile packages the Image via llvm-objcopy and "
         "run-a64-img boots it with -initrd",
         "llvm-objcopy -O binary" in makefl and "run-a64-img" in makefl and
         "kernela64.img" in makefl),
        ("A5a [AMEND-2]: kernel/lib/string.c is in KERNELA64_SHARED "
         "(the fdt.c promotion shape)",
         "kernel/dt/fdt.c kernel/lib/string.c" in makefl),
        ("A5a: kmain verifies x0's magic before trusting it and names "
         "the DTB source it chose",
         "fdt_magic_at(x0_at_entry)" in mainc and
         "DTB source: x0 (Image boot protocol)" in mainc),
        ("A5a: the kernel proves initrd BYTES arrived (ustar magic "
         "read back), not just /chosen advertised them",
         "initrd magic: ustar OK" in mainc),
        ("A5a: the Image smoke exists and asserts both the x0 source "
         "and the ustar read-back",
         "DTB source: x0" in read("tests", "integration",
                                  "a64_image_smoke.sh") and
         "ustar OK" in read("tests", "integration",
                            "a64_image_smoke.sh")),

        # --- A5b: libca64 + the fourth tenant ---
        ("A5b: libca64 exists (crt0, svc wrapper, header, both linker "
         "scripts)",
         exists("lib", "libca64", "crt0_a64.S") and
         exists("lib", "libca64", "syscall_a64.S") and
         exists("lib", "libca64", "libca64.h") and
         exists("lib", "libca64", "user_a64.ld") and
         exists("lib", "libca64", "shella64.ld")),
        ("A5b: the svc wrapper speaks D4 (x8 number, svc #0)",
         "mov   x8, x0" in read("lib", "libca64", "syscall_a64.S") and
         "svc   #0" in read("lib", "libca64", "syscall_a64.S")),
        ("A5b: the shared shell compiles for a64 through the AURA_LIBC "
         "seam (no forked shell)",
         "SMALLSH_DEFSA64" in makefl and
         'libca64.h' in makefl and "bina64/init" in makefl),
        ("A5b [AMEND-7]: the a64 user links carry --gc-sections from "
         "birth",
         makefl.count("--gc-sections -T lib/libca64/") == 2),
        ("A5b: mkinitrd audits the fourth tenant (e_machine 183)",
         "audit_tenant bina64 183 aarch64" in read("tools",
                                                   "mkinitrd.sh")),
        ("A5b: the initrd staging ships /bina64 (init + smallsh)",
         "$(INITRD_DIR)/bina64/init" in makefl and
         "$(INITRD_DIR)/bina64/smallsh" in makefl),

        # --- A5c: ELF loading, the EL0 shell, cross-refusals ---
        ("A5c: the fourth tenant's kernel plumbing exists (initrd walk "
         "+ ELF loader)",
         exists("kernel", "arch", "aarch64", "initrd_a64.c") and
         exists("kernel", "arch", "aarch64", "elfa64load.c")),
        ("A5c: the loader accepts 183 and refuses the other three "
         "tenants BY NAME",
         "EM_AARCH64 183" in read("kernel", "arch", "aarch64",
                                  "elfa64load.c").replace("#define EM_AARCH64 183",
                                                          "EM_AARCH64 183") and
         "riscv64 -- the /binrv tenant" in read("kernel", "arch",
                                                "aarch64", "elfa64load.c")),
        ("A5c: the other kernels' refusal tables grew the 183 row",
         "aarch64, the /bina64 tenant" in read("kernel", "proc", "elf.c") and
         "aarch64 -- the /bina64 tenant" in read("kernel", "arch",
                                                 "riscv64", "elfrvload.c")),
        ("A5c: read() BLOCKS -- the cooked line waits on wfi with IRQs "
         "re-enabled (the I7 deadlock's DAIF edition, and the fix for "
         "the measured prompt flood)",
         "daifclr, #2" in read("kernel", "arch", "aarch64", "user_a64.c") and
         "cons_a64_readline" in read("kernel", "arch", "aarch64",
                                     "user_a64.c")),
        ("A5c: the nested-spawn SP_EL0 save/restore exists (the phase's "
         "measured bug, pinned where it was fixed)",
         "mrs %0, sp_el0" in read("kernel", "arch", "aarch64",
                                  "user_a64.c") and
         "msr sp_el0, %0" in read("kernel", "arch", "aarch64",
                                  "user_a64.c")),
        ("A5c [AMEND-4]: unmap is a precise TLBI VAE1IS, not vmalle1",
         "tlbi vae1is" in read("kernel", "arch", "aarch64",
                               "paging_a64.c")),
        ("A5c: user stacks carry a guard hole between nesting levels",
         "USTACK_STRIDE = USTACK_PAGES + 1" in read("kernel", "arch",
                                                    "aarch64",
                                                    "user_a64.c")),
        ("A5c: the shell smoke exists with the log-size fuse and the "
         "exit-code round-trip pin",
         "log-size fuse" in read("tests", "integration",
                                 "a64_shell_smoke.sh") and
         "exit code 7" in read("tests", "integration",
                               "a64_shell_smoke.sh")),
    ]

    # Structural: the Status header and the phase table must agree.
    # Installed in A0, before a single row is green -- the D8 shape.
    # The A5 split (2026-08-20) taught this arithmetic the sub-phase
    # grammar (A5a/A5b/A5c): phase labels are ordered NAMES now, not
    # digits -- the in-phase checker-fix D6 prescribes, not a new
    # baseline.
    PHASE_ORDER = ["A0", "A1", "A2", "A3", "A4",
                   "A5a", "A5b", "A5c", "A6", "A7", "A8", "A9"]
    done_rows = len(re.findall(r"^\| A\d[abc]? .*?✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### Phase A\d[abc]? .*?✅ COMPLETE",
                                plan, re.M))
    checks.append(("plan: every complete table row has a COMPLETE heading",
                   done_rows == done_heads))

    status_ok = False
    if re.search(r"^## Status: PLANNED", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"A0(?:–(A\d[abc]?))? complete", plan)
        if m:
            label = m.group(1) if m.group(1) else "A0"
            claimed = (PHASE_ORDER.index(label) + 1
                       if label in PHASE_ORDER else 0)
            status_ok = claimed == done_rows
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == len(PHASE_ORDER))
    checks.append(("plan: the Status header agrees with the table",
                   status_ok))

    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(passed for _, passed in results):
            print("check_arm64_claims: SELFTEST inconclusive (tree already "
                  "red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(passed for _, passed in doctored):
            print("check_arm64_claims: SELFTEST FAIL -- checks pass against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_arm64_claims: selftest PASS (doctored tree detected)")
        return 0

    failed = 0
    for desc, passed in claims():
        if not passed:
            print(f"check_arm64_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_arm64_claims: {failed} claim(s) disagree with the tree",
              file=sys.stderr)
        return 1
    print(f"check_arm64_claims: OK -- {len(claims())} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
