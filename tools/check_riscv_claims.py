#!/usr/bin/env python3
"""Cross-check RISCV_PLAN.md's status against the tree.

Decision D8 of the plan it checks: check_i386_claims.py shipped in the
same phase as its plan's COMPLETION and was proud of never having an
unchecked day; this one ships in V0, the plan's FIRST delivered phase,
and grows a claim per phase.  A plan checked from birth cannot drift
at all.

Same structure as its two predecessors: each claim ties a phase to an
artefact that only exists if the phase happened; the plan's Status
header is checked against its own phase table; --selftest proves the
checks can fail (against a doctored ROOT).

Usage:
    tools/check_riscv_claims.py [--selftest]
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
    plan   = read("RISCV_PLAN.md")
    makefl = read("Makefile")
    boots  = read("kernel", "arch", "riscv64", "boot.S")
    ld     = read("kernel", "arch", "riscv64", "kernelrv.ld")

    checks = [
        # --- V0: toolchain gates + the S-mode stub ---
        ("V0: the rv64 entry, SBI layer and linker script exist",
         exists("kernel", "arch", "riscv64", "boot.S") and
         exists("kernel", "arch", "riscv64", "sbi.c") and
         exists("kernel", "arch", "riscv64", "kernelrv.ld")),
        ("V0: the Makefile has the kernelrv target with the rv64 flags",
         "kernelrv" in makefl and "-march=rv64gc" in makefl and
         "elf64lriscv" in makefl),
        ("V0: the hart lottery parks secondary harts",
         "amoswap" in boots and "hart_lottery" in boots),
        ("V0: the payload-base-not-e_entry fact is enforced in the "
         "linker script",
         ".text.boot" in ld and ".text.boot" in boots),
        ("V0: boot smoke test present",
         exists("tests", "integration", "rv_boot_smoke.sh")),
        ("V0: this checker is registered in test-unit",
         "check_riscv_claims.py" in makefl),
    ]

    # --- V1: boot_info_t from the Device Tree ---
    # ARM64_PLAN A1 promoted the walker to kernel/dt/fdt.c (shared by
    # both DTB-consuming kernels); the V1 claims now check the shared
    # file -- the artefact moved, the contract did not -- plus one new
    # claim that the rv64 kernel really consumes the shared copy (a
    # promotion that forked would pass file-existence checks and still
    # be a lie).
    fdt   = read("kernel", "dt", "fdt.c")
    sweep = read("tests", "unit", "test_width_sweep.sh")
    smoke = read("tests", "integration", "rv_boot_smoke.sh")
    checks += [
        ("V1: the FDT shim exists (shared, post-A1) and reads "
         "big-endian only",
         exists("kernel", "dt", "fdt.c") and
         not exists("kernel", "arch", "riscv64", "fdt.c") and
         "be32" in fdt and "be64" in fdt),
        ("V1/A1: the rv64 kernel links the SHARED walker",
         "kernel/dt/fdt.c" in makefl and
         '#include "kernel/dt/fdt.h"'
         in read("kernel", "arch", "riscv64", "main_rv.c")),
        ("V1: magic is written LAST (the partially-filled rule)",
         # the assignment must be the file's final act before return 0
         "bi->magic = BOOT_MAGIC" in fdt and
         fdt.rindex("bi->magic = BOOT_MAGIC") > fdt.rindex("hhdm_offset")),
        ("V1: hhdm carries the D3 Sv39 constant",
         "0xFFFFFFC000000000" in fdt),
        ("V1: the width contract compiles at the third width",
         "--target=riscv64" in sweep and "biw_rv64" in sweep),
        ("V1: the smoke test asserts the handoff and the initrd path",
         "handoff magic OK" in smoke and "initrd" in smoke),
    ]

    # --- V2: traps, timer, PLIC ---
    trapc = read("kernel", "arch", "riscv64", "trap.c")
    trape = read("kernel", "arch", "riscv64", "trapentry.S")
    plicc = read("kernel", "arch", "riscv64", "plic.c")
    checks += [
        ("V2: the trap entry saves the full frame and sret-returns",
         "rv_trap_vector" in trape and "sret" in trape and
         "sd    x31" in trape),
        ("V2: scause decode has the 16 named exception codes",
         exists("kernel", "arch", "riscv64", "trap.c") and
         "Illegal Instruction" in trapc and
         "Store/AMO Page Fault" in trapc),
        ("V2: FIX_R0 discipline -- cpu= in the frame dump",
         "cpu=hart" in trapc),
        ("V2: the timer re-arms via SBI (one-shot contract)",
         "sbi_set_timer" in trapc),
        ("V2: PLIC completes even unhandled claims (no stuck gateway)",
         "*reg32(PLIC_CLAIM(s_ctx)) = irq" in plicc),
        ("V2: the smoke test gates isr/timer/plic PASS lines",
         "resumed" in smoke and "ticks observed at 100 Hz" in smoke and
         "claim/complete" in smoke),
    ]

    # --- V3: Sv39, PMM, heap, W^X ---
    pagec = read("kernel", "arch", "riscv64", "paging_rv.c")
    pmmc  = read("kernel", "arch", "riscv64", "pmm_rv.c")
    ldscr = read("kernel", "arch", "riscv64", "kernelrv.ld")
    boots2 = read("kernel", "arch", "riscv64", "boot.S")
    checks += [
        ("V3: the linker script is higher-half (HHDM VMA, physical LMA)",
         "HHDM = 0xFFFFFFC000000000" in ldscr and "AT(ADDR(" in ldscr),
        ("V3: boot.S turns Sv39 on before any C runs",
         "csrw  satp" in boots2 and "early_root" in boots2),
        ("V3: the PMM is bitmap.h's third consumer, header unedited",
         'include "kernel/lib/bitmap.h"' in pmmc and
         "pmm32" not in read("kernel", "lib", "bitmap.h")),
        ("V3: no W+X PTE is ever built (grep the flag combos)",
         # pte_leaf()'s R|W|X MASK is a test, not a mapping -- excluded.
         "PTE_R | PTE_X" in pagec and "PTE_R | PTE_W" in pagec and
         "PTE_W | PTE_X" not in
         "".join(l for l in pagec.splitlines() if "pte_leaf" not in l)),
        ("V3: the vmm self-test probes all three faults",
         "probe_store_text" in pagec and "probe_exec_data" in pagec and
         "probe_load_identity" in pagec),
        ("V3: the smoke test asserts higher-half sepc and the W^X pair",
         "sepc=0xffffffc0" in smoke and
         "execute-from-data faulted" in smoke),
    ]

    # --- V4: threads, scheduler, U-mode, ecall ---
    ctxs  = read("kernel", "arch", "riscv64", "context_rv.S")
    userc = read("kernel", "arch", "riscv64", "user_rv.c")
    trap2 = read("kernel", "arch", "riscv64", "trap.c")
    trape2 = read("kernel", "arch", "riscv64", "trapentry.S")
    checks += [
        ("V4: the context switch saves the psABI callee-saved set",
         "context_switch_rv" in ctxs and "s11" in ctxs and
         "sd    ra" in ctxs),
        ("V4: preemption sits AFTER the timer re-arm (post-EOI shape)",
         "sbi_set_timer(now + tick_interval)" in trap2 and
         trap2.index("sched_rv_maybe_preempt") >
         trap2.index("sbi_set_timer(now + tick_interval)")),
        ("V4: the trap entry swap-and-tests sscratch (the I7 lesson)",
         "csrrw sp, sscratch, sp" in trape2),
        ("V4: user text maps U|R|X and the stack U|R|W -- W^X in U-mode",
         "PTE_U | PTE_R | PTE_X" in userc and
         "PTE_U | PTE_R | PTE_W" in userc),
        ("V4: U-mode faults are contained as 128+scause",
         "128 + (int)scause" in userc),
        ("V4: user copies go through a SUM window, kernel buffer",
         "SUM" in userc and "csrs sstatus" in userc and
         "csrc sstatus" in userc),
        ("V4: the smoke test gates sched/RING-U-OK/exit-code/control",
         "RING-U-OK" in smoke and "never-yielding" in smoke and
         "code 130" in smoke),
    ]

    # --- V5: libcrv, the promoted shell, ELF loading, /binrv ---
    elfrv  = read("kernel", "arch", "riscv64", "elfrvload.c")
    smsh   = read("userspace", "system", "smallsh", "smallsh.c")
    shsmk  = read("tests", "integration", "rv_shell_smoke.sh")
    checks += [
        ("V5: libcrv exists (crt0 + ecall wrapper + header)",
         exists("lib", "libcrv", "crt0_rv.S") and
         exists("lib", "libcrv", "syscall_rv.S") and
         exists("lib", "libcrv", "libcrv.h")),
        ("V5: the shell is PROMOTED -- one source, seam by define",
         exists("userspace", "system", "smallsh", "smallsh.c") and
         not exists("userspace", "system", "shell32", "shell32.c") and
         "AURA_LIBC" in smsh and
         "SMALLSH_DEFS32" in makefl and "SMALLSH_DEFSRV" in makefl),
        ("V5: the loader refuses the other two arches AND W+X segments",
         "ELFCLASS64" in elfrv and "EM_RISCV" in elfrv and
         "W+X segment" in elfrv),
        ("V5: p_flags become real PTE bits",
         "PF_X" in elfrv and "PTE_X" in elfrv and
         "p_flags honoured" in elfrv),
        ("V5: /binrv is the initrd's third tenant",
         "binrv/init" in makefl and "binrv/smallsh" in makefl),
        ("V5: the shell smoke drives the same session as i386's",
         "run binrv/init" in shsmk and "auralite# " in shsmk and
         "definitely-not-a-cmd" in shsmk),
    ]

    # --- V6: the inline-assembly sweep ---
    archh  = read("kernel", "arch", "arch.h")
    sweepy = read("tools", "check_width_sweep.py")
    spinc  = read("kernel", "lib", "spinlock.c")
    checks += [
        ("V6: arch.h forwards irqflags for all three arches",
         "x86_64/irqflags.h" in archh and "i386/irqflags.h" in archh and
         "riscv64/irqflags.h" in archh),
        ("V6: the three irqflags backends exist with one contract",
         all(exists("kernel", "arch", a, "irqflags.h")
             for a in ("x86_64", "i386", "riscv64"))),
        ("V6: ratchet 4 is armed with the plan's baseline arithmetic",
         "BASELINE_ASM_FILES" in sweepy and
         "was 33 at V6 arming" in sweepy),
        ("V6: spinlock.c is C11 atomics, no inline asm statements",
         "atomic_compare_exchange_strong_explicit" in spinc and
         "volatile (" not in spinc),
        ("V6: port I/O on riscv is a named compile error, not a stub",
         "unavailable" in archh and "virtio-mmio" in archh),
    ]

    # --- V7: virtio-mmio, blk, net, UART RX ---
    vmmio = read("kernel", "arch", "riscv64", "virtio_mmio.c")
    vblk  = read("kernel", "arch", "riscv64", "vblk_rv.c")
    vnet  = read("kernel", "arch", "riscv64", "vnet_rv.c")
    uartv = read("kernel", "arch", "riscv64", "uart_rv.c")
    net32 = read("kernel", "arch", "i386", "net32.c")
    checks += [
        ("V7: the mmio transport reuses virtio_common.h's vrings",
         'include "drivers/virtio/virtio_common.h"' in
         read("kernel", "arch", "riscv64", "virtio_mmio.h") and
         "VM_QUEUE_PFN" in vmmio and "VM_QUEUE_READY" in vmmio),
        ("V7: blk gate is ata32's shape (known bytes + readback/restore)",
         "write/readback/restore" in vblk and "0x55" in vblk),
        ("V7: miniproto is SHARED -- both NICs consume it",
         exists("kernel", "net", "miniproto.c") and
         "miniproto_dhcp" in vnet and "miniproto_dhcp" in net32 and
         "KERNELRV_SHARED" in makefl and
         "kernel/net/miniproto.c" in makefl),
        ("V7: miniproto prints nothing (callers own their log lines)",
         "sbi_puts" not in read("kernel", "net", "miniproto.c") and
         "kprintf" not in read("kernel", "net", "miniproto.c")),
        ("V7: UART RX is interrupt-fed through the PLIC ring",
         "plic_enable" in uartv and "ring_push" in uartv and
         "uart_rv_getc" in read("kernel", "arch", "riscv64", "user_rv.c")),
        ("V7: the shell smoke asserts the PLIC-input receipt",
         "rx bytes via PLIC irq" in shsmk),
    ]

    # --- V8: parity, full crypto, the three-tenant audit ---
    rvcrypt = read("tests", "unit", "test_libatls_rv64.sh")
    parity  = read("tests", "integration", "rv_parity_smoke.sh")
    mkinit  = read("tools", "mkinitrd.sh")
    checks += [
        ("V8: the rv64 crypto gate runs the COMPLETE suite",
         "test_atls_x25519" in rvcrypt and "test_atls_ed25519" in rvcrypt
         and "test_atls_ecdsa" in rvcrypt and "qemu-riscv64" in rvcrypt),
        ("V8: the crypto gate is registered beside the m32 gate",
         "test_libatls_rv64.sh" in makefl and
         "test_libatls_m32.sh" in makefl),
        ("V8: the parity smoke asserts one gate per phase + no-FAIL",
         "RING-U-OK" in parity and "rx bytes via PLIC irq" in parity and
         'assert_no_grep "FAIL"' in parity),
        ("V8: mkinitrd audits all three tenants by e_machine",
         "audit_tenant bin   62" in mkinit and
         "audit_tenant bin32 3" in mkinit and
         "audit_tenant binrv 243" in mkinit),
        ("V8: the plan carries the per-arch status matrix draft",
         "| Subsystem | x86_64 | i386 | riscv64 |" in plan),
    ]

    # --- V9: CI, docs, the close ---
    ci     = read(".github", "workflows", "integration.yml")
    status = read("docs", "status.md")
    arch_d = read("docs", "architecture.md")
    sysabi = read("docs", "syscall_abi.md")
    readme = read("README.md")
    checks += [
        ("V9: the riscv-parity CI job exists with artefact-first order",
         "riscv-parity:" in ci and "rv_parity_smoke.sh" in ci and
         ci.index("binrv is in the shared initrd") <
         ci.index("rv_boot_smoke.sh")),
        ("V9: docs/status.md has the RISC-V section with by-design rows",
         "## RISC-V (rv64gc) — RISCV_PLAN" in status and
         "No own M-mode firmware" in status),
        ("V9: docs/architecture.md carries the third boot diagram",
         "The riscv64 boot flow" in arch_d and "hart lottery" in arch_d),
        ("V9: docs/syscall_abi.md has the ecall table beside the other two",
         "The riscv64 trap: `ecall`" in sysabi and
         "`a7` | Syscall number" in sysabi),
        ("V9: README names the third boot path",
         "RISC-V (rv64gc)" in readme and "run-rv" in readme),
    ]

    # Structural: the Status header and the phase table must agree.
    # While phases are pending the header says PLANNED/IN PROGRESS and
    # complete-rows == complete-headings; when it claims a range, the
    # range must match the table (the same checks the i386 checker
    # ended with -- installed here from day one).
    done_rows = len(re.findall(r"^\| V\d .*?✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### Phase V\d .*?✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete table row has a COMPLETE heading",
                   done_rows == done_heads))

    status_ok = False
    if re.search(r"^## Status: PLANNED", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"V0(?:–V(\d))? complete", plan)
        claimed = (int(m.group(1)) + 1 if m and m.group(1) else 1) if m else 0
        status_ok = bool(m) and claimed == done_rows
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == 10)
    checks.append(("plan: the Status header agrees with the table",
                   status_ok))

    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(passed for _, passed in results):
            print("check_riscv_claims: SELFTEST inconclusive (tree already "
                  "red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(passed for _, passed in doctored):
            print("check_riscv_claims: SELFTEST FAIL -- checks pass against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_riscv_claims: selftest PASS (doctored tree detected)")
        return 0

    failed = 0
    for desc, passed in claims():
        if not passed:
            print(f"check_riscv_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_riscv_claims: {failed} claim(s) disagree with the tree",
              file=sys.stderr)
        return 1
    print(f"check_riscv_claims: OK -- {len(claims())} claims verified "
          f"against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
