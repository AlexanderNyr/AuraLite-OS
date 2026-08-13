#!/usr/bin/env python3
"""mk_pe_efi_variant.py — WIN32_PLAN.md W32-3 negative fixture.

Copies a PE32+ image and rewrites exactly one field: the optional header's
Subsystem, set to IMAGE_SUBSYSTEM_EFI_APPLICATION (10).

Why patch rather than assemble a second binary: the refusal has to be
attributable.  If the negative fixture were built separately it could differ
from the good image in a dozen ways, and "the kernel refused it" would not
prove *which* difference caused the refusal.  One byte apart, it does.

This is the property that keeps AuraLite's own BOOTX64.EFI -- a valid PE32+
AMD64 executable -- from ever being launched as a user process.
"""
import sys, struct

def main():
    if len(sys.argv) != 3:
        print("usage: mk_pe_efi_variant.py <in.exe> <out.exe>", file=sys.stderr)
        return 2
    data = bytearray(open(sys.argv[1], "rb").read())

    if data[0:2] != b"MZ":
        print("not a PE: missing MZ", file=sys.stderr); return 1
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew+4] != b"PE\0\0":
        print("not a PE: missing PE signature", file=sys.stderr); return 1

    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        print(f"not PE32+: optional magic 0x{magic:X}", file=sys.stderr); return 1

    # Subsystem sits at offset 68 in the PE32+ optional header.
    off = opt + 68
    before = struct.unpack_from("<H", data, off)[0]
    struct.pack_into("<H", data, off, 10)      # EFI_APPLICATION

    open(sys.argv[2], "wb").write(data)
    print(f"  [pe] {sys.argv[2]} (subsystem {before} -> 10, EFI application)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
