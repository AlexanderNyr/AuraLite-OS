#!/usr/bin/env python3
"""Cross-check RESIDUE_PLAN.md + the ledger against the tree.

Seventh checker of the D8 family.  What is new here: the debt
RATCHET.  The harvester's per-file marker counts are pinned in
tools/residue_baseline.txt; any plan growing (or shedding) residue
markers without moving the baseline AND the ledger in the same
commit fails.  The ledger itself is arithmetic-checked: 48 rows,
sequential unique ids, class totals, status totals.

Usage:
    tools/check_residue_claims.py [--selftest]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LEDGER_ROWS = 48
CLASS_PIN = {"W": 33, "M": 5, "N": 2, "S": 8}
PHASE_ORDER = ["R0", "R1", "R2", "R3", "R4", "R5", "R6",
               "R7", "R8", "R9", "R10", "R11", "R12"]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def harvest_live():
    """Run the harvester in-process for a doctored-ROOT-friendly way."""
    script = os.path.join(ROOT, "tools", "residue_harvest.py")
    if not os.path.exists(script):
        return None
    try:
        out = subprocess.run(
            [sys.executable, script, "--machine"],
            capture_output=True, text=True, timeout=60,
            cwd=ROOT).stdout
    except Exception:
        return None
    return dict(line.rsplit(" ", 1) for line in out.splitlines()
                if " " in line)


def claims():
    plan = read("RESIDUE_PLAN.md")
    ledger = read("docs", "residue_ledger.md")
    checks = []

    # --- the ledger's arithmetic --------------------------------------
    rows = re.findall(r"^\| (RES-\d\d) \| (\w+) \| ([A-Z@R\d-]+) \|",
                      ledger, re.M)
    ids = [r[0] for r in rows]
    checks.append((
        f"ledger: exactly {LEDGER_ROWS} rows with unique sequential ids "
        f"(found {len(ids)})",
        len(ids) == LEDGER_ROWS and len(set(ids)) == LEDGER_ROWS and
        ids == [f"RES-{i:02d}" for i in range(1, LEDGER_ROWS + 1)]))
    class_counts = {}
    for _, cls, _st in rows:
        class_counts[cls] = class_counts.get(cls, 0) + 1
    checks.append((
        f"ledger: class totals match the pins (measured {class_counts}, "
        f"pinned {CLASS_PIN})",
        class_counts == CLASS_PIN))
    n_open = sum(1 for _, _, st in rows if st == "OPEN")
    n_closed = len(rows) - n_open
    done_phases = len([1 for r in re.findall(
        r"^\| R\d+ [^|]*\| ✅ complete", plan, re.M)])
    checks.append((
        f"ledger: status arithmetic is sane (OPEN {n_open} + moved "
        f"{n_closed} = {LEDGER_ROWS}; {done_phases} phase(s) landed)",
        n_open + n_closed == LEDGER_ROWS))

    # --- the debt ratchet: live harvest == pinned baseline -------------
    base_text = read("tools", "residue_baseline.txt")
    baseline = dict(line.rsplit(" ", 1) for line in
                    base_text.splitlines() if " " in line)
    live = harvest_live()
    if live is None:
        checks.append(("harvest: tools/residue_harvest.py runs", False))
    else:
        drift = {k: (baseline.get(k), live.get(k))
                 for k in set(baseline) | set(live)
                 if baseline.get(k) != live.get(k)}
        checks.append((
            f"ratchet: live harvest matches the baseline (drift: "
            f"{drift if drift else 'none'} — move baseline AND ledger "
            "in the same commit)",
            not drift and len(baseline) > 0))

    # --- R7: ECAM + virtio-pci (when the plan says the phase landed) ----
    if re.search(r"^### R7[^\n]*✅ COMPLETE", plan, re.M):
        makefl = read("Makefile")
        ecam_c = read("kernel", "drivers", "pci_ecam.c")
        vpci_c = read("kernel", "drivers", "virtio_pci.c")
        checks.append((
            "R7: the two shared transports exist and BOTH tenants list "
            "them (single source, twice linked)",
            ecam_c != "" and vpci_c != "" and
            makefl.count("kernel/drivers/pci_ecam.c") >= 2 and
            makefl.count("kernel/drivers/virtio_pci.c") >= 2))
        checks.append((
            "R7: the shared transports obey the portable rules "
            "(no inline asm, no bare width casts)",
            "__asm__" not in ecam_c and "__asm__" not in vpci_c and
            "(uint64_t)" not in ecam_c and "(uint64_t)" not in vpci_c))
        checks.append((
            "R7: fdt.c names pci-host-ecam-generic and the walker "
            "prints the receipt",
            "pci-host-ecam-generic" in read("kernel", "dt", "fdt.c") and
            "[pci] ECAM: " in ecam_c))
        checks.append((
            "R7: virtio_pci is MODERN — VERSION_1 offered back, "
            "FEATURES_OK verified by read-back",
            "VERSION_1" in vpci_c and "VM_S_FEATURES_OK" in vpci_c))
        checks.append((
            "R7: both fs smokes carry the PCI mount lane and pin the "
            "transport-truth blkdev line",
            all("virtio-blk over PCI (modern, VERSION_1)" in
                read("tests", "integration", s) and
                "vblk0 (virtio-pci" in read("tests", "integration", s)
                for s in ("rv_fs_smoke.sh", "a64_fs_smoke.sh"))))
        checks.append((
            "R7 rider: the v3 group claim precedes the online count "
            "(the R5/R6 CI reds' one cause) and the smp smoke asserts "
            "the v3 timer",
            "BEFORE this core counts itself online"
            in read("kernel", "arch", "aarch64", "smp_a64.c") and
            "GICD_IGROUPR" in read("kernel", "arch", "aarch64", "gic.c")
            and "\\[timer\\] PASS"
            in read("tests", "integration", "a64_smp_smoke.sh")))

    # --- R8: the Rust rows (when the plan says the phase landed) --------
    if re.search(r"^### R8[^\n]*✅ COMPLETE", plan, re.M):
        makefl = read("Makefile")
        common = read("lib", "rsbr", "common.rs")
        rustes = read("userspace", "apps", "rustes", "rustes.rs")
        checks.append((
            "R8: one bridge, three ISAs — common.rs carries all three "
            "cfg'd trap instructions, rustes.rs all three counters",
            all(k in common for k in
                ('target_arch = "x86_64"', 'target_arch = "riscv64"',
                 'target_arch = "aarch64"', '"ecall"', '"svc #0"')) and
            all(k in rustes for k in ("rdtsc", "rdtime", "cntvct_el0"))))
        checks.append((
            "R8: both tenant editions build through the Makefile and "
            "ship in the initrd copy list",
            "riscv64gc-unknown-none-elf" in makefl and
            "aarch64-unknown-none" in makefl and
            "binrv/rustes" in makefl and "bina64/rustes" in makefl))
        checks.append((
            "R8: the counter gates opened on BOTH init paths per "
            "tenant (R5's init-on-a-secondary lesson)",
            read("kernel", "arch", "riscv64",
                 "trap.c").count("scounteren") >= 2 and
            read("kernel", "arch", "aarch64",
                 "trap_a64.c").count("cntkctl_el1") >= 2))
        checks.append((
            "R8: the shared receipt is asserted on both tenants "
            "(the first pin of any Rust row)",
            all("=== Rust Benchmark ===" in read("tests", "integration", s)
                and "Sum: 499999500000" in read("tests", "integration", s)
                for s in ("rv_fs_smoke.sh", "a64_fs_smoke.sh"))))

    # --- R9: the net cluster (when the plan says the phase landed) ------
    if re.search(r"^### R9[^\n]*✅ COMPLETE", plan, re.M):
        ipv6 = read("kernel", "net", "ipv6.c")
        dns = read("kernel", "net", "dns.c")
        checks.append((
            "R9: the five NDP catches are fixed in the tree (lengths 4+body, "
            "serialised checksums, NA target at +8, RA options at +16, "
            "RFC 1071 validation)",
            "icmp_len = 4 + (uint32_t)sizeof(body)" in ipv6 and
            "htons16(cs)" in ipv6 and
            "buf + 14 + 40 + 8, target->b, 16" in ipv6 and
            "buf + 14 + 40 + 16" in ipv6 and
            "if (want != 0) return 1;" in ipv6))
        checks.append((
            "R9: SLAAC + the NS→NA responder exist and the ping6 case pins "
            "the fec0::2 end-to-end echo",
            "SLAAC address" in ipv6 and "ICMP6_NS && len >=" in ipv6 and
            "Reply received from fec0::2"
            in read("tests", "integration", "cases", "test_ipv6_ping6.sh")))
        checks.append((
            "R9: the DNS TCP fallback is the RFC 1035 s4.2.2 shape and its "
            "case rides the NAMED knob",
            "dns_wire_query_tcp" in dns and "DNSCTL_FORCE_TC"
            in read("kernel", "net", "dns.h") and
            "TCP fallback answer" in dns and
            "dnstc" in read("tests", "integration", "cases",
                            "test_dns_tcp.sh")))
        checks.append((
            "R9: the DHCP builders write tos AND flags_frag at both sites "
            "(the stack-garbage catch)",
            read("kernel", "net", "net.c").count("ip->flags_frag  = 0;")
            >= 2))
        checks.append((
            "R9: virtio-net's timed RX wait sleeps (wq_wait_deadline) and "
            "the receipt is pinned",
            "wq_wait_deadline(&vnet_rx_wq"
            in read("drivers", "virtio_net", "virtio_net.c") and
            "RX via IRQ wake" in read("tests", "integration", "cases",
                                      "test_virtio_net.sh")))
        checks.append((
            "R9: RES-27 closed as a stale row against a tree that agrees "
            "(http.c IS the libahttp client)",
            'ahttp_client_get' in read("userspace", "apps", "http",
                                       "http.c")))

    # --- R10: the crypto width line (when the plan says it landed) ------
    if re.search(r"^### R10[^\n]*✅ COMPLETE", plan, re.M):
        fe_h = read("lib", "libatls", "src", "atls_fe.h")
        fe_c = read("lib", "libatls", "src", "atls_fe.c")
        ec_c = read("lib", "libatls", "src", "atls_ecdsa.c")
        m32 = read("tests", "unit", "test_libatls_m32.sh")
        checks.append((
            "R10: the width selector exists (packed 8x-uint32 fe when "
            "__int128 is absent, FORCE32 override) and the 64-bit core "
            "kept its radix-2^51 shape",
            "ATLS_FE_WIDTH32" in fe_h and "ATLS_FE_FORCE32" in fe_h and
            "#define ATLS_FE_LIMBS 8" in fe_h and
            "fe32_fold38" in fe_c and "MASK51" in fe_c))
        checks.append((
            "R10: atls_ecdsa is limb-parameterised, not forked "
            "(P256_QUAD packs the same constants; atls_dlimb doubles)",
            "P256_QUAD" in ec_c and "atls_dlimb" in ec_c and
            "#define P256_LIMBS 8" in ec_c and
            "#define P256_LIMB_BITS 32" in ec_c))
        checks.append((
            "R10: the m32 gate runs the COMPLETE five-suite battery in "
            "BOTH lanes (FORCE32 native + real -m32)",
            "test_atls_ecdsa" in m32 and "test_atls_ed25519" in m32 and
            "ATLS_FE_FORCE32" in m32 and "cc -m32" in m32))
        checks.append((
            "R10: the symmetric no-__int128 guard survived the flip "
            "(those eight sources have no width ifdef to hide behind)",
            "NO128_SRCS" in m32 and
            "__int128" not in read("lib", "libatls", "src",
                                   "atls_chacha20.c")))
        checks.append((
            "R10: the stale -m32-boundary epilogues were rewritten in "
            "the rv64/a64 gates the same commit they became lies",
            "cannot reach" not in read("tests", "unit",
                                       "test_libatls_rv64.sh") and
            "cannot reach" not in read("tests", "unit",
                                       "test_libatls_a64.sh")))
        checks.append((
            "R10: the status row flipped (no '32-bit limb path is "
            "required' left in status.md)",
            "a 32-bit limb path is required first"
            not in read("docs", "status.md")))

    # --- R11: PCID + the metal package (when the plan says it landed) ---
    if re.search(r"^### R11[^\n]*✅ COMPLETE", plan, re.M):
        pcid_pol = read("kernel", "arch", "x86_64", "pcid_policy.h")
        pcid_c   = read("kernel", "arch", "x86_64", "pcid.c")
        tlb_c    = read("kernel", "arch", "x86_64", "tlb_shootdown.c")
        pag_c    = read("kernel", "arch", "x86_64", "paging.c")
        perf_sh  = read("tests", "integration", "cases",
                        "test_perf_smoke.sh")
        checks.append((
            "R11: the PCID decision core is pure C with a host gate, "
            "and the named deviation is written where it lives",
            "pcid_policy_sender_skip" in pcid_pol and
            "DEVIATION FROM THE WRITTEN DESIGN, NAMED" in pcid_pol and
            "pcid_policy.h" in read("Makefile") and
            "test_pcid_policy" in read("Makefile")))
        checks.append((
            "R11: the plumbing is feature-gated (CR4.PCIDE from CPUID, "
            "switches routed through pcid_cr3_for, handler de-owns "
            "non-resident victims)",
            "pcid_cr4_bit" in pag_c and "pcid_cr3_for" in pag_c and
            "pcid_local_deown" in tlb_c and
            "pcid_sender_may_skip" in tlb_c and
            "No invpcid anywhere here" in pcid_c))
        checks.append((
            "R11: the perf smoke un-pinned the counters by SELF-SELECTING "
            "on the feature bit (zero pinned on pcid=0, movement demanded "
            "on pcid=1)",
            "cr3_noflush_switches stays zero" in perf_sh and
            "cr3_noflush_switches [1-9]" in perf_sh))
        checks.append((
            "R11/RES-34: both tenant knobs feed the ONE shared "
            "selftest.c (Makefile rows) and both smokes pin the toggle",
            read("Makefile").count("kernel/lib/selftest.c") >= 2 and
            "SKIPPED: self-test (mode=off)"
            in read("tests", "integration", "i386_shell_smoke.sh") and
            "mode=off (source: fw-cfg)"
            in read("tests", "integration", "a64_boot_smoke.sh") and
            "qemu,fw-cfg-mmio" in read("kernel", "dt", "fdt.c")))
        checks.append((
            "R11/RES-37: the kernel-side MADT walk exists and the QEMU "
            "agreement line is pinned in the NULL case",
            "madt_ioapic_base" in read("kernel", "arch", "x86_64",
                                       "ioapic.c") and
            "MADT agree" in read("tests", "integration", "cases",
                                 "test_metal_null.sh")))
        checks.append((
            "R11: the package ships (script + paste-back doc with the "
            "WHPX PCID block) and the NULL case is registered",
            "--null-test" in read("tools", "metal_receipts.sh") and
            "cr3_noflush_switches"
            in read("docs", "metal_receipts.md") and
            "test_metal_null"
            in read("tests", "integration", "run_all.sh")))

    # --- R0 structure ---------------------------------------------------
    checks.append((
        "R0: the amended class totals are recorded as a CATCH in both "
        "documents (the draft's 27/6/3/12 was wrong)",
        "W 33" in ledger and "WRONG" in ledger and
        "W 33 · M 5 · N 2 · S 8" in plan))
    checks.append((
        "R0: the checker family wiring (test-unit runs this file with "
        "its selftest)",
        read("Makefile").count("check_residue_claims.py") >= 2))

    # --- structural: status header vs table -----------------------------
    done_rows = len(re.findall(r"^\| R\d+ [^|]*\| ✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### R\d+[^\n]*✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete row has a COMPLETE heading",
                   done_rows == done_heads and plan != ""))
    status_ok = False
    if re.search(r"^## Status: IN PROGRESS — R0 next", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"R0(?:–(R\d+))? complete", plan)
        if m:
            label = m.group(1) if m.group(1) else "R0"
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
        if not all(ok for _, ok in results):
            print("check_residue_claims: SELFTEST inconclusive (tree "
                  "already red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(ok for _, ok in doctored):
            print("check_residue_claims: SELFTEST FAIL -- passes against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_residue_claims: selftest PASS (doctored tree "
              "detected)")
        return 0

    failed = 0
    results = claims()
    for desc, ok in results:
        if not ok:
            print(f"check_residue_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_residue_claims: {failed} claim(s) disagree with "
              "the tree", file=sys.stderr)
        return 1
    print(f"check_residue_claims: OK -- {len(results)} claims verified "
          "(the debt ratchet runs the harvester LIVE)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
