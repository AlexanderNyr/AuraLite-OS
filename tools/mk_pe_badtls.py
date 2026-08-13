#!/usr/bin/env python3
"""mk_pe_badtls.py — WIN32_PLAN.md W32-6 hostile fixture.

Rewrites the TLS callback array of a PE so its first callback points far
outside the image.

The loader must REFUSE to call it.  This is the sharpest hostile case in the
phase: the callback array is data straight out of the file, it is followed
before any of the program's own code runs, and a loader that dereferences it
unchecked hands control to whatever the file names.  Unlike the W32-5
WNDPROC case -- where the fault is the program's own fault, caught in user
mode -- here nothing should be called at all.

The TLS directory holds absolute VAs, so the fixture must be linked at a
fixed base (it is: -base:0x140000000) for these to be meaningful.
"""
import struct
import sys


def sections(d, pe):
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        va, = struct.unpack_from("<I", d, o + 12)
        vs, = struct.unpack_from("<I", d, o + 8)
        ro, = struct.unpack_from("<I", d, o + 20)
        rs, = struct.unpack_from("<I", d, o + 16)
        out.append((va, vs, ro, rs))
    return out


def rva_to_off(secs, rva):
    for va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    return None


def main():
    if len(sys.argv) != 3:
        print("usage: mk_pe_badtls.py <in.exe> <out.exe>", file=sys.stderr)
        return 2

    d = bytearray(open(sys.argv[1], "rb").read())
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<Q", d, opt + 24)[0]
    secs = sections(d, pe)

    tls_rva, tls_size = struct.unpack_from("<II", d, opt + 112 + 9 * 8)
    if tls_rva == 0 or tls_size == 0:
        print("mk_pe_badtls: input has no TLS directory; the fixture would "
              "not be hostile, refusing to write it", file=sys.stderr)
        return 1

    tls_off = rva_to_off(secs, tls_rva)
    if tls_off is None:
        print("mk_pe_badtls: TLS directory RVA not in any section",
              file=sys.stderr)
        return 1

    # IMAGE_TLS_DIRECTORY64: +0x18 is AddressOfCallBacks (a VA).
    cb_va = struct.unpack_from("<Q", d, tls_off + 0x18)[0]
    cb_off = rva_to_off(secs, cb_va - image_base)
    if cb_off is None:
        print("mk_pe_badtls: callback array not in any section",
              file=sys.stderr)
        return 1

    old = struct.unpack_from("<Q", d, cb_off)[0]
    if old == 0:
        print("mk_pe_badtls: callback array is empty; nothing to poison",
              file=sys.stderr)
        return 1

    # Far above the image but still a canonical, plausible-looking address,
    # so the loader's bounds check is what refuses it rather than the
    # address being obviously malformed.
    poisoned = image_base + 0x40000000
    struct.pack_into("<Q", d, cb_off, poisoned)

    open(sys.argv[2], "wb").write(d)
    print(f"  [pe] {sys.argv[2]} (TLS callback[0] 0x{old:x} -> "
          f"0x{poisoned:x}, outside the image)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
