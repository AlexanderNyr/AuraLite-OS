#!/usr/bin/env python3
"""mkrbfi.py -- pack the AuraLite image that RBFI's loader reads.

RBFI reads ONE flat blob from the boot USB stick into 0x00100000 and jumps to
its first byte.  That blob is firmware/rbfi/rbfi_stage2.asm (192 KiB, fixed);
this script produces its *data*: the 512-byte header the stage parses and the
payloads it copies.

Layout (all sector-aligned, because RBFI's BCBW transfer has no scatter):

    LBA 0           512-byte header (see firmware/rbfi/rbfi_stage2.asm)
    LBA 1 ..        kernel ELF, padded to a sector
    ...             initrd tar, padded to a sector (optional)

Usage:
    tools/mkrbfi.py --kernel build/kernel.elf --initrd build/initrd.tar \\
                    --stage build/rbfi_stage2.bin --out build/auralite.rbfi

The output is a disk image: sector 0 is the header, sector 1 is where the
stage expects the kernel.  `--stage` is placed at LBA 0 only when
--embed-stage is given, which is the form a USB stick needs (RBFI copies the
first 192 KiB straight to 1 MiB); without it the output is the payload alone,
which is what the unit test compares against.
"""

from __future__ import annotations

import argparse
import struct
import sys

MAGIC = 0x49464252  # 'RBFI'
VERSION = 1
HEADER_BYTES = 512
FLAG_INITRD = 0x1

SECTOR = 512


def align(n: int, to: int = SECTOR) -> int:
    return (n + to - 1) // to * to


def build(kernel: bytes, initrd: bytes | None, ram_bytes: int, entry: int) -> bytes:
    """Return the payload image (header + kernel + initrd), sector padded."""
    kernel_off = HEADER_BYTES
    kernel_len = len(kernel)
    initrd_off = align(kernel_off + kernel_len) if initrd else 0
    initrd_len = len(initrd) if initrd else 0

    flags = FLAG_INITRD if initrd else 0

    hdr = struct.pack(
        "<IIIIQQQQQQ",
        MAGIC,
        VERSION,
        HEADER_BYTES,
        flags,
        kernel_off,
        kernel_len,
        initrd_off,
        initrd_len,
        ram_bytes,
        entry,
    )
    assert len(hdr) <= HEADER_BYTES, "header overran its sector"
    out = bytearray(hdr.ljust(HEADER_BYTES, b"\0"))
    out += kernel
    out += b"\0" * (align(len(out)) - len(out))
    if initrd:
        out += initrd
        out += b"\0" * (align(len(out)) - len(out))
    return bytes(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd")
    ap.add_argument("--stage", help="rbfi_stage2.bin to embed at LBA 0")
    ap.add_argument("--embed-stage", action="store_true")
    ap.add_argument("--ram", type=lambda s: int(s, 0), default=512 * 1024 * 1024)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)

    kernel = open(args.kernel, "rb").read()
    initrd = open(args.initrd, "rb").read() if args.initrd else None

    if kernel[:4] != b"\x7fELF":
        print(f"mkrbfi: {args.kernel} is not an ELF", file=sys.stderr)
        return 1

    # e_entry from the ELF header, informational only -- the stage trusts
    # the ELF's own field, this copy exists so the packer and the stage can
    # be cross-checked by tools/check_rbfi.py.
    entry = struct.unpack_from("<Q", kernel, 0x18)[0]

    payload = build(kernel, initrd, args.ram, entry)

    if args.embed_stage:
        if not args.stage:
            print("mkrbfi: --embed-stage needs --stage", file=sys.stderr)
            return 1
        stage = open(args.stage, "rb").read()
        if len(stage) != 192 * 1024:
            print(
                f"mkrbfi: stage is {len(stage)} bytes, RBFI copies exactly "
                f"{192 * 1024}; rebuild rbfi_stage2.asm",
                file=sys.stderr,
            )
            return 1
        # RBFI copies the first 192 KiB to 1 MiB and jumps there, so the
        # stage owns sectors 0..383 and the payload starts at LBA 384.
        payload = stage + payload

    open(args.out, "wb").write(payload)
    print(
        f"mkrbfi: {args.out}: {len(payload)} bytes "
        f"({len(payload) // SECTOR} sectors), kernel {len(kernel)} B @ "
        f"{((384 * 512) if args.embed_stage else 512):#x}, "
        f"initrd {len(initrd) if initrd else 0} B"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
