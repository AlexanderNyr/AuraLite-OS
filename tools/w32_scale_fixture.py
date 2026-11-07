#!/usr/bin/env python3
"""tools/w32_scale_fixture.py — RESIDUE3_PLAN.md phase C8, the loader's scale.

Audacity 3.7.5 is the case this phase exists for, and it can never be committed
(see w32/app_ledger/audacity-3.7.5.gap: USER-SUPPLIED, only names/counts/hashes
are kept).  So the gate cannot run Audacity — but it CAN run an image with
Audacity's measured *shape*, which is what the loader's walls were about:

  wall 1  scale   90932 base relocations vs the old `static pe_reloc_t
                  relocs[16384]` buffer -- fatal before bind.
  wall 2  shape   a 117-DLL graph vs a 64-slot module table / depth 8.
  wall 3  gap     19 modules / 419 symbols with no personality -- refusal,
                  which now has to be REPORTED as a ledger, not as one line.

This tool emits three small PE32+ executables that the guest can run through
/apps/w32run, each carrying one of those shapes at exactly the measured size:

  w32scale.exe   N DIR64 relocations (default 90932), entry point `xor eax,
                 eax; ret` -> w32run must print
                 "[w32] relocations: 90932 applied (DIR64), delta ..., no fixed cap"
                 and exit 0.
  w32gap.exe     imports 3 symbols that no personality provides, in 2 modules
                 (winmm.dll, wsock32.dll) -> w32run must refuse, naming every
                 module and the total, not just the first symbol.
  w32type3.exe   16 relocations of type 3 (IMAGE_REL_BASED_HIGHLOW) -> the
                 refusal must name the type, not say "bad relocation table".

The images are built from the PE grammar the loader parses (nothing is copied
from a real binary): DOS header, COFF + PE32+ optional header, three sections
(.text, .data, .reloc; plus .idata for the gap fixture), one relocation block
per 4 KiB page of .data, and — for the gap image — an import directory with a
single descriptor and its IAT.  A fixture that a test can generate is a fixture
a reviewer can read.
"""

import argparse
import struct
import sys

PE_OFF = 0x80
OPT_OFF = PE_OFF + 24
OPT_SIZE = 240
SECT_OFF = OPT_OFF + OPT_SIZE            # 0x188
HDR_SIZE = 0x400

TEXT_RVA, TEXT_RAW = 0x1000, 0x400
DATA_RVA, DATA_RAW = 0x2000, 0x600
IDATA_RVA = 0x402000
RELOC_RVA = 0x500000
IMAGE_SPAN = 0x600000

TEXT_VSIZE = 0x200
TEXT_RAW_SIZE = 0x200
DATA_VSIZE = 0x400000                    # room for the relocation targets
DATA_RAW_SIZE = 0x200
ENT_PER_BLOCK = 511                      # 511 quads fill one 4 KiB page
IMAGE_BASE = 0x140000000

PE_REL_DIR64 = 10
PE_REL_HIGHLOW = 3


def u16(b, o, v):
    struct.pack_into("<H", b, o, v & 0xFFFF)


def u32(b, o, v):
    struct.pack_into("<I", b, o, v & 0xFFFFFFFF)


def u64(b, o, v):
    struct.pack_into("<Q", b, o, v)


def align(n, a):
    return (n + a - 1) // a * a


class Builder:
    def __init__(self, nrel, reloc_type, imports, code):
        self.nrel = nrel
        self.reloc_type = reloc_type
        self.imports = imports            # [(dll, [names...]), ...] or []
        self.code = code
        self.idata = self._build_idata() if imports else b""
        self.reloc = self._build_reloc()
        self.text = code.rjust(TEXT_RAW_SIZE, b"\x90")[:TEXT_RAW_SIZE]

    # -- .idata: one descriptor per DLL, IAT + hint/name table ---------------
    def _build_idata(self):
        """Lay out the import directory the way a linker does:
        descriptors, then each DLL's IAT (8-byte thunks), then the import
        name table (hint + ASCIIZ), then the DLL name strings."""
        blob = bytearray()
        ndesc = len(self.imports)
        desc_size = 20 * (ndesc + 1)             # terminated by a zero entry
        blob.extend(b"\0" * desc_size)

        iat_rvas, name_rvas, dll_rvas = [], [], []
        for _dll, names in self.imports:
            iat_rvas.append(IDATA_RVA + len(blob))
            blob.extend(b"\0" * (8 * (len(names) + 1)))   # null-terminated
        for _dll, names in self.imports:
            slot_names = []
            for nm in names:
                while len(blob) % 2:
                    blob.append(0)
                slot_names.append(IDATA_RVA + len(blob))
                blob.extend(b"\0\0")                       # Hint
                blob.extend(nm.encode("ascii") + b"\0")
            name_rvas.append(slot_names)
        for dll, _names in self.imports:
            dll_rvas.append(IDATA_RVA + len(blob))
            blob.extend(dll.encode("ascii") + b"\0")
        while len(blob) % 8:
            blob.append(0)

        for i, (_dll, names) in enumerate(self.imports):
            o = i * 20
            u32(blob, o + 0, iat_rvas[i])            # OriginalFirstThunk
            u32(blob, o + 12, dll_rvas[i])           # Name
            u32(blob, o + 16, iat_rvas[i])           # FirstThunk
            for k, nrva in enumerate(name_rvas[i]):
                u64(blob, iat_rvas[i] - IDATA_RVA + k * 8, nrva)
        return bytes(blob)

    # -- .reloc: one block per 4 KiB page of .data --------------------------
    def _build_reloc(self):
        if self.nrel == 0:
            return b""
        blob = bytearray()
        done, page = 0, 0
        while done < self.nrel:
            take = min(ENT_PER_BLOCK, self.nrel - done)
            page_rva = DATA_RVA + page * 0x1000
            blob.extend(struct.pack("<II", page_rva, 8 + 2 * take))
            for k in range(take):
                voff = (k * 8) & 0xFFF
                blob.extend(struct.pack("<H", (self.reloc_type << 12) | voff))
            done += take
            page += 1
        return bytes(blob)

    def build(self):
        secs = []
        body = bytearray()

        def add_section(name, rva, vsize, raw_off, raw):
            secs.append((name, rva, vsize, len(raw), raw_off))
            return raw

        nsec = 3 + (1 if self.idata else 0)
        raw_off = HDR_SIZE
        text_raw_off = raw_off; raw_off += TEXT_RAW_SIZE
        data_raw_off = raw_off; raw_off += DATA_RAW_SIZE
        idata_raw_off = raw_off
        if self.idata:
            raw_off += align(len(self.idata), 0x200)
        reloc_raw_off = raw_off
        raw_off += align(len(self.reloc), 0x200)
        total = raw_off

        f = bytearray(total)

        # DOS header
        u16(f, 0, 0x5A4D)                       # MZ
        u32(f, 0x3C, PE_OFF)
        # NT headers
        u32(f, PE_OFF, 0x00004550)              # PE\0\0
        u16(f, PE_OFF + 4, 0x8664)              # Machine = AMD64
        u16(f, PE_OFF + 6, nsec)
        u16(f, PE_OFF + 20, OPT_SIZE)
        u16(f, PE_OFF + 22, 0x0022)             # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE  # noqa: E501
        # PE32+ optional header
        u16(f, OPT_OFF + 0, 0x20B)
        u32(f, OPT_OFF + 16, TEXT_RVA)          # AddressOfEntryPoint
        u64(f, OPT_OFF + 24, IMAGE_BASE)
        u32(f, OPT_OFF + 32, 0x1000)            # SectionAlignment
        u32(f, OPT_OFF + 36, 0x200)             # FileAlignment
        u32(f, OPT_OFF + 56, IMAGE_SPAN)        # SizeOfImage
        u32(f, OPT_OFF + 60, HDR_SIZE)          # SizeOfHeaders
        u16(f, OPT_OFF + 68, 3)                 # Subsystem = CUI
        u32(f, OPT_OFF + 108, 16)               # NumberOfRvaAndSizes

        # section table
        def sect(i, name, rva, vsize, rawsize, rawoff, flags):
            o = SECT_OFF + i * 40
            f[o:o + 8] = name.encode("ascii").ljust(8, b"\0")
            u32(f, o + 8, vsize)
            u32(f, o + 12, rva)
            u32(f, o + 16, rawsize)
            u32(f, o + 20, rawoff)
            u32(f, o + 36, flags)

        CODE = 0x60000020
        IDATA = 0xC0000040
        RDATA = 0x40000040
        i = 0
        sect(i, ".text", TEXT_RVA, TEXT_VSIZE, TEXT_RAW_SIZE, text_raw_off, CODE); i += 1
        sect(i, ".data", DATA_RVA, DATA_VSIZE, DATA_RAW_SIZE, data_raw_off,
             0xC0000040); i += 1                          # R/W initialized
        if self.idata:
            sect(i, ".idata", IDATA_RVA, len(self.idata), len(self.idata),
                 idata_raw_off, IDATA); i += 1
            u32(f, OPT_OFF + 112 + 1 * 8, IDATA_RVA)      # import directory
            u32(f, OPT_OFF + 112 + 1 * 8 + 4, 20 * (len(self.imports) + 1))
        if self.reloc:
            sect(i, ".reloc", RELOC_RVA, len(self.reloc), len(self.reloc),
                 reloc_raw_off, RDATA); i += 1
            u32(f, OPT_OFF + 112 + 5 * 8, RELOC_RVA)      # base reloc directory
            u32(f, OPT_OFF + 112 + 5 * 8 + 4, len(self.reloc))

        f[text_raw_off:text_raw_off + len(self.text)] = self.text
        if self.idata:
            f[idata_raw_off:idata_raw_off + len(self.idata)] = self.idata
        if self.reloc:
            f[reloc_raw_off:reloc_raw_off + len(self.reloc)] = self.reloc
        return bytes(f)


# A bare entry point: xor eax, eax; ret -- ms_abi, no arguments, returns 0.
CODE_RET0 = bytes([0x31, 0xC0, 0xC3])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="build/user")
    ap.add_argument("--relocs", type=int, default=90932,
                    help="relocation count for w32scale.exe (default: the "
                         "Audacity 3.7.5 measurement)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv[1:])

    import os
    os.makedirs(args.out_dir, exist_ok=True)

    made = []

    # One REAL import on purpose: the shell routes a PE to /apps/w32run only
    # when its import directory is non-empty (init.c pe_needs_w32run), and a
    # fixture the shell executes directly would prove nothing about the loader.
    scale = Builder(args.relocs, PE_REL_DIR64,
                    [("KERNEL32.dll", ["ExitProcess"])],
                    CODE_RET0).build()
    p = os.path.join(args.out_dir, "w32scale.exe")
    open(p, "wb").write(scale)
    made.append((p, len(scale), f"{args.relocs} DIR64 relocations"))

    gap = Builder(16, PE_REL_DIR64,
                  [("winmm.dll", ["waveOutOpen", "timeSetEvent"]),
                   ("wsock32.dll", ["WSAStartup"])],
                  CODE_RET0).build()
    p = os.path.join(args.out_dir, "w32gap.exe")
    open(p, "wb").write(gap)
    made.append((p, len(gap), "2 missing modules, 3 missing symbols"))

    # Same reason as w32scale.exe: one real import, so the shell hands the
    # image to /apps/w32run.  A fixture the shell runs itself through the
    # kernel's PE loader never reaches the user-space relocation path this case
    # is about (and its bare `ret` has no C caller to return to -- the first
    # run of this fixture faulted at RIP 0x1 for exactly that reason).
    t3 = Builder(16, PE_REL_HIGHLOW,
                 [("KERNEL32.dll", ["ExitProcess"])],
                 CODE_RET0).build()
    p = os.path.join(args.out_dir, "w32type3.exe")
    open(p, "wb").write(t3)
    made.append((p, len(t3), "16 HIGHLOW (type 3) relocations"))

    if not args.quiet:
        for path, size, what in made:
            print(f"  [pe] {path} ({size} bytes, {what})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
