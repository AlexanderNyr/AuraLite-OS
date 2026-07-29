#!/usr/bin/env python3
"""gen_ap_trampoline_inc.py -- embed the assembled AP trampoline into C.

Reads the flat binary produced by `nasm -f bin boot/smp/ap_trampoline.asm`
and writes a C header containing it as a byte array, which
kernel/arch/x86_64/smp.c then #includes and copies to the fixed low
physical address the AP starts executing from after its SIPI.

Usage: gen_ap_trampoline_inc.py <ap_trampoline.bin>   (header -> stdout)
"""

import sys


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: gen_ap_trampoline_inc.py <ap_trampoline.bin>\n")
        return 2

    with open(sys.argv[1], "rb") as f:
        blob = f.read()

    if len(blob) == 0:
        sys.stderr.write("gen_ap_trampoline_inc: empty input binary\n")
        return 2
    # The SIPI vector is one byte, so the trampoline must fit in one page
    # (and it must never straddle the page the handoff data lives in).
    if len(blob) > 4096:
        sys.stderr.write(
            "gen_ap_trampoline_inc: trampoline is %d bytes > one page\n"
            % len(blob))
        return 2

    out = []
    out.append("/* Auto-generated from boot/smp/ap_trampoline.asm by")
    out.append(" * tools/gen_ap_trampoline_inc.py -- do not edit by hand. */")
    out.append("#define AP_TRAMPOLINE_SIZE %d" % len(blob))
    out.append(
        "static const unsigned char ap_trampoline_blob[AP_TRAMPOLINE_SIZE] = {")
    for off in range(0, len(blob), 12):
        chunk = blob[off:off + 12]
        out.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    out.append("};")
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
