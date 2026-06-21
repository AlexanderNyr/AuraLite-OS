#!/usr/bin/env python3
"""High-resolution ASCII dump of a horizontal band of build/screen.png so the
rendered text can be read directly. fg (bright) pixels -> '#', bg -> '.'.
"""
import sys
from analyze_screen import decode_png  # reuse the decoder

PNG = "/home/user/auralite/build/screen.png"


def main():
    width, height, bpp, px = decode_png(PNG)

    start_char_row = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    num_rows = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    max_chars = int(sys.argv[3]) if len(sys.argv) > 3 else 60

    cell = 8
    y0 = start_char_row * cell
    y1 = y0 + num_rows * cell
    x1 = min(width, max_chars * cell)

    print(f"rows {start_char_row}..{start_char_row + num_rows - 1} "
          f"(y {y0}-{y1}, x 0-{x1})")
    for y in range(y0, min(y1, height)):
        line = ""
        for x in range(0, x1):
            o = (y * width + x) * bpp
            lum = (px[o] + px[o + 1] + px[o + 2]) // 3
            line += "#" if lum > 40 else " "
        print(line)


if __name__ == "__main__":
    main()
