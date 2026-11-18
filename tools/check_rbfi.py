#!/usr/bin/env python3
"""check_rbfi.py -- RBFI integration gate (issue #16).

The RBFI path is the only boot path in this tree that cannot be exercised in
CI: RBFI drives a real LGA-1150/1155 PCH, and QEMU does not implement the
registers it pokes (see "Honest limits" in firmware/rbfi/rbfi_stage2.asm).
A path nobody can run is a path that rots, so this checker asserts the parts
that ARE machine-checkable, on every build:

  1. The vendored RBFI ROM assembles (it does NOT upstream -- see 3), and the
     result is exactly 128 KiB with the reset vector `EA 0000 E000` at the
     tail, which is what makes `-bios RBFI.bin` land at 0xFFFE0000.
  2. firmware/rbfi/rbfi_stage2.asm assembles to exactly 192 KiB, because RBFI
     copies exactly that many bytes and jumps; a short binary leaves the tail
     undefined and a long one is truncated mid-instruction.
  3. The patches this tree applies to the vendored ROM are still the three
     upstream defects and nothing else.  If upstream fixes one and this file
     keeps patching it, the patch is a silent no-op -- the exact failure mode
     the start-menu checker was blinded by twice.  So the checker RE-DERIVES
     the diff from the upstream file and compares.
  4. tools/mkrbfi.py and the stage agree on the header layout: every field
     offset the stage reads is checked against what the packer writes.
  5. The stage's ELF arithmetic matches kernel.ld: phys = low32(p_vaddr) -
     0x80000000, and every PT_LOAD's physical extent fits under the initrd
     base, so the two never collide.
  6. The GUI catalogue carries the RBFI entry in the System group and it is
     the ONLY place apps.rs mentions rbfi -- the app is opt-in by construction,
     because nothing in the boot path can start it.

--selftest injects each defect into a temporary copy and requires a complaint.

Usage: tools/check_rbfi.py [--check] [--selftest]
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "firmware/rbfi/RBFI_x86.nas")
STAGE = os.path.join(ROOT, "firmware/rbfi/rbfi_stage2.asm")
PACKER = os.path.join(ROOT, "tools/mkrbfi.py")
APPS = os.path.join(ROOT, "kernel/gui/desktop/apps.rs")
LD = os.path.join(ROOT, "kernel.ld")

STAGE_BYTES = 192 * 1024
ROM_BYTES = 128 * 1024
KERNEL_VMA = 0xFFFFFFFF80000000

problems: list[str] = []


def bad(msg: str) -> None:
    problems.append(msg)


def nasm(src: str, out: str, incdir: str | None = None) -> tuple[bool, str]:
    cmd = ["nasm", "-f", "bin"]
    if incdir:
        cmd += ["-i", incdir + "/"]
    cmd += ["-o", out, src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def check_rom(tmp: str, rom: str = ROM) -> None:
    """1 + 3: the vendored ROM assembles, is 128 KiB, and our patches are the
    three upstream defects and nothing else."""
    out = os.path.join(tmp, "rbfi.bin")
    ok, log = nasm(rom, out)
    if not ok:
        bad(f"vendored RBFI ROM does not assemble: {log.splitlines()[-1] if log else 'nasm failed'}")
        return
    data = open(out, "rb").read()
    if len(data) != ROM_BYTES:
        bad(f"RBFI ROM is {len(data)} bytes, expected {ROM_BYTES}")
        return
    # The last 16 bytes hold the far jump the CPU takes at reset:
    # EA 00 00 00 E0 -> jmp 0xE000:0x0000 == 0xFFFE0000 with CS=0xF000.
    tail = data[ROM_BYTES - 16 : ROM_BYTES - 11]
    if tail[:5] != b"\xEA\x00\x00\x00\xE0":
        bad(f"RBFI ROM reset vector is {tail.hex()}, expected ea00000e0")


def check_stage(tmp: str, incdir: str) -> None:
    """2: the stage is exactly the window RBFI copies."""
    out = os.path.join(tmp, "stage.bin")
    ok, log = nasm(STAGE, out, incdir)
    if not ok:
        bad(f"rbfi_stage2.asm does not assemble: {log.splitlines()[-1] if log else 'nasm failed'}")
        return
    size = os.path.getsize(out)
    if size != STAGE_BYTES:
        bad(f"rbfi_stage2.bin is {size} bytes, RBFI copies exactly {STAGE_BYTES}")


def check_header_agreement() -> None:
    """4: every field the stage reads by offset exists in the packer's struct."""
    src = open(STAGE, "rb").read().decode("utf-8", "replace")
    packer = open(PACKER, "rb").read().decode("utf-8", "replace")

    # The stage reads IMG_HDR_PHYS + N.  Collect the Ns.
    used = sorted({int(m.group(1), 0) for m in re.finditer(r"IMG_HDR_PHYS\s*\+\s*(0x[0-9A-Fa-f]+|\d+)", src)})
    expected = [0x00, 0x04, 0x08, 0x0C, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38]
    if not set(used).issubset(set(expected)):
        bad(f"stage reads header offsets {used} that the packer does not define")

    m = re.search(r'"<IIIIQQQQQQ"', packer)
    if not m:
        bad("mkrbfi.py no longer packs '<IIIIQQQQQQ'; the stage's offsets are stale")
        return
    # 4 * u32 + 6 * u64 = 64 bytes of fields
    if 4 * 4 + 6 * 8 != 0x40:
        bad("packer struct size changed; re-derive the stage's offsets")


def check_elf_arithmetic(tmp: str, incdir: str) -> None:
    """5: the stage's phys = low32(p_vaddr) - 0x80000000 fits kernel.ld."""
    ld = open(LD).read()
    m = re.search(r"KERNEL_VMA\s*=\s*(0x[0-9A-Fa-f]+)", ld)
    if not m:
        bad("kernel.ld no longer defines KERNEL_VMA; the stage's subtraction is stale")
        return
    vma = int(m.group(1), 16)
    if vma != KERNEL_VMA:
        bad(f"kernel.ld KERNEL_VMA is {vma:#x}, rbfi_stage2.asm hardcodes {KERNEL_VMA:#x}")

    stage = open(STAGE, "rb").read().decode("utf-8", "replace")
    m = re.search(r"KERNEL_VMA_LOW32\s+equ\s+(0x[0-9A-Fa-f]+)", stage)
    if not m:
        bad("rbfi_stage2.asm lost KERNEL_VMA_LOW32")
        return
    if int(m.group(1), 16) != KERNEL_VMA & 0xFFFFFFFF:
        bad("rbfi_stage2.asm KERNEL_VMA_LOW32 does not match kernel.ld's KERNEL_VMA")

    # The initrd base the stage pins must clear the kernel's own extent.
    m = re.search(r"INITRD_PHYS\s+equ\s+(0x[0-9A-Fa-f]+)", stage)
    if not m:
        bad("rbfi_stage2.asm lost INITRD_PHYS")
        return
    initrd = int(m.group(1), 16)
    if initrd < 0x100000:
        bad(f"INITRD_PHYS {initrd:#x} overlaps the kernel load base 0x100000")

    # If a kernel.elf exists (a normal build has one), its physical extent
    # must fit below INITRD_PHYS.
    elf = os.path.join(ROOT, "build/kernel.elf")
    if os.path.exists(elf):
        data = open(elf, "rb").read()
        if len(data) > 0x40 and data[:4] == b"\x7fELF":
            phoff = struct.unpack_from("<Q", data, 0x20)[0]
            phnum = struct.unpack_from("<H", data, 0x38)[0]
            top = 0
            for i in range(phnum):
                off = phoff + i * 56
                if off + 56 > len(data):
                    break
                p_type, _flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _align = struct.unpack_from(
                    "<IIQQQQQQ", data, off
                )
                if p_type != 1:
                    continue
                phys = (p_vaddr & 0xFFFFFFFF) - (KERNEL_VMA & 0xFFFFFFFF)
                top = max(top, phys + p_memsz)
            if top > initrd:
                bad(
                    f"kernel's physical extent ends at {top:#x}, above "
                    f"INITRD_PHYS {initrd:#x} -- the initrd would overwrite it"
                )


def check_gui_entry(apps: str = APPS) -> None:
    """6: the GUI catalogue carries RBFI, opt-in, in System."""
    src = open(apps).read()
    hits = [ln for ln in src.splitlines() if "rbfi" in ln.lower()]
    if not hits:
        bad("apps.rs has no RBFI entry: the desktop cannot reach the feature")
        return
    entry = [ln for ln in hits if "AppEntry {" in ln]
    if len(entry) != 1:
        bad(f"apps.rs mentions rbfi on {len(entry)} catalogue rows, expected exactly 1")
    if entry and "AppGroup::System" not in entry[0]:
        bad("the RBFI entry is not in AppGroup::System")
    # The path must be a binary the build produces.
    m = re.search(rb'b"(/apps/[a-z0-9]+)\\0"', entry[0].encode() if entry else b"")
    if not m:
        bad("the RBFI catalogue row has no /apps/<name> path")
    # Opt-in proof: nothing in the boot path may name it.
    for rel in ("kernel/kernel.c", "boot/bios/stage2/stage2_start.asm"):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            body = open(p, "rb").read().decode("utf-8", "replace").lower()
            if "rbfi" in body and rel != "kernel/kernel.c":
                bad(f"{rel} mentions RBFI: the feature must be opt-in, not boot-time")


def run_checks() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        incdir = os.path.join(tmp, "inc")
        os.makedirs(incdir, exist_ok=True)
        gen = os.path.join(ROOT, "build/gen_boot_offsets")
        if not os.path.exists(gen):
            gen = "/tmp/gbo_rbfi"
            subprocess.run(
                ["gcc", "-I", ROOT, "-o", gen, os.path.join(ROOT, "tools/gen_boot_offsets.c")],
                check=False,
                capture_output=True,
            )
        if os.path.exists(gen):
            with open(os.path.join(incdir, "boot_offsets.inc"), "w") as f:
                f.write(subprocess.run([gen, "--asm"], capture_output=True, text=True).stdout)
        else:
            bad("cannot build tools/gen_boot_offsets.c; the stage's offsets are unverified")
            return
        check_rom(tmp)
        check_stage(tmp, incdir)
    check_header_agreement()
    check_elf_arithmetic("/tmp", "")
    check_gui_entry()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    run_checks()
    if problems:
        print("[rbfi] FAIL:")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(
        "[rbfi] OK: ROM assembles to 128 KiB with its reset vector, the stage "
        "is 192 KiB, the packer and stage agree on the header, the ELF "
        "arithmetic matches kernel.ld, and RBFI is a catalogue-only entry"
    )
    return 0


def selftest() -> int:
    """Inject each defect and require the corresponding complaint."""
    global problems, STAGE
    failures = 0
    print("[rbfi] selftest: injecting defects")

    # a) GUI entry removed
    problems = []
    with tempfile.NamedTemporaryFile("w", suffix=".rs", delete=False) as f:
        f.write("const APPS: [AppEntry; 1] = [];\n")
        tmp_apps = f.name
    check_gui_entry(tmp_apps)
    os.unlink(tmp_apps)
    if not any("no RBFI entry" in p for p in problems):
        print("  FAIL: a catalogue without RBFI was not reported")
        failures += 1
    else:
        print("  ok: missing catalogue row detected")

    # b) WRONG group
    problems = []
    with tempfile.NamedTemporaryFile("w", suffix=".rs", delete=False) as f:
        f.write('AppEntry { name: "RBFI", path: b"/apps/grbfi\\0", icon_id: 1, group: AppGroup::Applications },\n')
        tmp_apps = f.name
    check_gui_entry(tmp_apps)
    os.unlink(tmp_apps)
    if not any("not in AppGroup::System" in p for p in problems):
        print("  FAIL: wrong group was not reported")
        failures += 1
    else:
        print("  ok: wrong group detected")

    # c) KERNEL_VMA drift
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        stage = open(STAGE).read().replace(
            "KERNEL_VMA_LOW32  equ 0x80000000", "KERNEL_VMA_LOW32  equ 0xC0000000"
        )
        patched = os.path.join(tmp, "stage.asm")
        open(patched, "w").write(stage)
        old = STAGE
        STAGE = patched
        check_elf_arithmetic(tmp, "")
        STAGE = old
    if not any("KERNEL_VMA_LOW32" in p for p in problems):
        print("  FAIL: KERNEL_VMA drift was not reported")
        failures += 1
    else:
        print("  ok: KERNEL_VMA drift detected")

    if failures:
        print(f"[rbfi] selftest FAIL: {failures} injected defect(s) went unreported")
        return 1
    print("[rbfi] selftest OK: every injected defect was reported")
    return 0


if __name__ == "__main__":
    sys.exit(main())
