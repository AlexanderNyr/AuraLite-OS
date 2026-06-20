#!/usr/bin/env python3
"""Decode build/screen.png (no external deps) and render it as ASCII art,
so on-screen text can be verified without vision. Each 8x8 character cell is
collapsed to one character by average brightness.
"""
import struct
import sys
import zlib

PATH = "/home/user/novos/build/screen.png"


def decode_png(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"

    width = height = bitdepth = colortype = None
    idat = b""
    i = 8
    while i < len(data):
        (length,) = struct.unpack(">I", data[i:i + 4])
        ctype = data[i + 4:i + 8]
        body = data[i + 8:i + 8 + length]
        if ctype == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(">IIBB", body[:10])
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        i += 12 + length

    bpp = {2: 3, 6: 4}[colortype]
    raw = zlib.decompress(idat)
    stride = width * bpp

    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                line[x] = (line[x] + pr) & 0xFF
        out += line
        prev = line
    return width, height, bpp, bytes(out)


def main():
    width, height, bpp, px = decode_png(PATH)

    # Distinct colours + fg/bg ratio, as a sanity check that text was drawn.
    bg = px[0:3]  # top-left pixel is background
    colors = set()
    nfg = 0
    for y in range(height):
        for x in range(width):
            o = (y * width + x) * bpp
            r, g, b = px[o], px[o + 1], px[o + 2]
            colors.add((r, g, b))
    n_total = width * height
    for o in range(0, len(px), bpp):
        r, g, b = px[o], px[o + 1], px[o + 2]
        lum = (r + g + b) // 3
        if lum > 40:
            nfg += 1
    print(f"image: {width}x{height} bpp={bpp * 8}")
    print(f"distinct colors: {len(colors)} ; bright(fg) pixels: "
          f"{nfg} ({100.0 * nfg / n_total:.1f}%)")

    # ASCII-art: collapse each 8x8 cell to one char by average brightness.
    ramp = " .:-=+*#%@"
    cell = 8
    cols = min(width // cell, 70)
    rows = min(height // cell, 20)
    print(f"\n--- screen (top-left {cols}x{rows} cells) ---")
    for cy in range(rows):
        line = ""
        for cx in range(cols):
            acc = 0
            for dy in range(cell):
                for dx in range(cell):
                    o = ((cy * cell + dy) * width + (cx * cell + dx)) * bpp
                    acc += (px[o] + px[o + 1] + px[o + 2]) // 3
            avg = acc // (cell * cell)
            line += ramp[avg * (len(ramp) - 1) // 255]
        print(line)


if __name__ == "__main__":
    main()
