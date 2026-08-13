#!/usr/bin/env python3
"""mk_dll_variants.py — WIN32_PLAN.md W32-7 hostile fixtures.

Builds two variants of the test DLL from the good one:

  --forwarder  point an export's RVA back into the export directory, which
               is how PE encodes "this symbol lives in another DLL".  The
               loader must refuse it by name rather than return a pointer to
               the forwarder string, which the caller would then CALL.

  --dllmain-fails  make DllMain return 0.  The documented behaviour is that
               the load fails; the interesting part is that the mapping must
               be torn down rather than left behind for a program that
               believes LoadLibrary succeeded.

Both are derived from the real fixture rather than hand-written, so they
differ from a working DLL in exactly one respect.
"""
import struct
import sys


def secs_of(d, pe):
    n = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(n):
        o = pe + 24 + optsz + i * 40
        vs, va, rs, ro = struct.unpack_from("<IIII", d, o + 8)
        out.append((va, vs, ro, rs))
    return out


def r2o(secs, rva):
    for va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    return None


def main():
    if len(sys.argv) != 4:
        print("usage: mk_dll_variants.py <mode> <in.dll> <out.dll>",
              file=sys.stderr)
        return 2
    mode, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    d = bytearray(open(src, "rb").read())
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    secs = secs_of(d, pe)

    if mode == "--forwarder":
        exp_rva, exp_size = struct.unpack_from("<II", d, opt + 112)
        if not exp_rva:
            print("mk_dll_variants: no export directory", file=sys.stderr)
            return 1
        eo = r2o(secs, exp_rva)
        n_funcs = struct.unpack_from("<I", d, eo + 20)[0]
        func_rva = struct.unpack_from("<I", d, eo + 28)[0]
        fo = r2o(secs, func_rva)
        if n_funcs == 0 or fo is None:
            print("mk_dll_variants: no functions to poison", file=sys.stderr)
            return 1
        old = struct.unpack_from("<I", d, fo)[0]
        # Anywhere inside the export directory reads as a forwarder.
        struct.pack_into("<I", d, fo, exp_rva + 8)
        print(f"  [pe] {dst} (export[0] rva 0x{old:x} -> 0x{exp_rva + 8:x}, "
              f"inside the export directory = a forwarder)")

    elif mode == "--dllmain-fails":
        entry = struct.unpack_from("<I", d, opt + 16)[0]
        off = r2o(secs, entry)
        if off is None:
            print("mk_dll_variants: entry point not in a section",
                  file=sys.stderr)
            return 1
        # xor eax,eax ; ret  -- return FALSE immediately.
        d[off:off + 3] = b"\x31\xC0\xC3"
        print(f"  [pe] {dst} (DllMain at rva 0x{entry:x} patched to "
              f"return FALSE)")

    else:
        print(f"mk_dll_variants: unknown mode {mode}", file=sys.stderr)
        return 2

    open(dst, "wb").write(d)
    return 0


if __name__ == "__main__":
    sys.exit(main())
