#!/usr/bin/env python3
# tools/fb_oracle.py — WR-0 (W32RUN_PLAN.md) the framebuffer oracle.
#
# The live-GUI lane (tests/integration/lib/gui_lane.sh) boots AuraLite under
# OVMF — the only firmware with a linear framebuffer (BIOS Stage 2 sets no VBE
# mode; the GUI composes off-screen and the frame is black — fb.c's own
# comment) — and captures the GOP framebuffer with the QEMU monitor
# `screendump` verb.  This tool turns such a capture (PPM or PNG) into a
# PASS/FAIL, so a GUI claim is a fact about pixels, not a log line.
#
# It is deliberately made of small, deterministic primitives, not a fragile
# "recognise the app" heuristic (W32RUN_PLAN D-WR4: name the gap, never fake
# the pass).  Gates compose them:
#
#   brightness IMG [--floor N]
#       mean luma of the whole frame (or --region).  Exit 0 iff > floor.
#       The not-black gate: proves the LFB exists and the compositor reached
#       the screen.  This is exactly what test_gui.sh could only SKIP on the
#       BIOS lane.
#
#   region-brightness IMG --region X0 Y0 X1 Y1
#       print the mean luma of a rectangle (no gate) — for structural
#       comparisons the caller thresholds itself.
#
#   band-brighter IMG --band  X0 Y0 X1 Y1 \
#                     --ref   X0 Y0 X1 Y1 [--by N]
#       exit 0 iff mean(band) - mean(ref) >= N.  The taskbar/titlebar
#       structural gate: a chrome band is brighter than the desktop behind
#       it, with no colour hard-coding.
#
#   find-color IMG R G B [--tol T] [--region ...] [--min-frac F]
#       fraction of pixels within L-inf tol T of (R,G,B); print it and the
#       bbox of the match.  Exit 0 iff fraction >= min-frac.  Locates a
#       window title bar, a red close box, a selection, etc.
#
#   delta IMG_A IMG_B [--region ...] [--thresh T]
#       bbox of pixels that changed by more than thresh between two frames;
#       exit 0 iff the bbox is non-empty.  The "an injected action changed
#       the listing" gate.
#
# Only Pillow is required (present in the toolchain image).  All coordinates
# are pixels, origin top-left, x1/y1 exclusive.

import argparse
import sys

try:
    from PIL import Image, ImageChops
except Exception as e:  # pragma: no cover - environment gate
    sys.stderr.write("fb_oracle: Pillow (PIL) is required: %s\n" % e)
    sys.exit(2)


def _open_rgb(path):
    return Image.open(path).convert("RGB")


def _clip_region(im, region):
    if not region:
        return im, (0, 0, im.width, im.height)
    x0, y0, x1, y1 = region
    x0 = max(0, min(x0, im.width))
    y0 = max(0, min(y0, im.height))
    x1 = max(x0, min(x1, im.width))
    y1 = max(y0, min(y1, im.height))
    return im.crop((x0, y0, x1, y1)), (x0, y0, x1, y1)


def _mean_luma(im):
    b = im.convert("L").tobytes()
    if not b:
        return 0.0
    return sum(b) / len(b)


def cmd_brightness(a):
    im = _open_rgb(a.image)
    sub, box = _clip_region(im, a.region)
    m = _mean_luma(sub)
    print("brightness mean=%.2f region=%s" % (m, box))
    return 0 if m > a.floor else 1


def cmd_region_brightness(a):
    im = _open_rgb(a.image)
    sub, box = _clip_region(im, a.region)
    m = _mean_luma(sub)
    print("region-brightness mean=%.2f region=%s" % (m, box))
    return 0


def cmd_band_brighter(a):
    im = _open_rgb(a.image)
    band, bbox = _clip_region(im, a.band)
    ref, rbox = _clip_region(im, a.ref)
    mb = _mean_luma(band)
    mr = _mean_luma(ref)
    diff = mb - mr
    print("band-brighter band=%.2f%s ref=%.2f%s diff=%.2f need>=%.2f"
          % (mb, bbox, mr, rbox, diff, a.by))
    return 0 if diff >= a.by else 1


def cmd_find_color(a):
    im = _open_rgb(a.image)
    sub, box = _clip_region(im, a.region)
    px = sub.load()
    w, h = sub.width, sub.height
    tot = w * h
    if tot == 0:
        print("find-color frac=0.0000 (empty region)")
        return 1
    r0, g0, b0 = a.rgb
    tol = a.tol
    hit = 0
    minx = w
    miny = h
    maxx = -1
    maxy = -1
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if abs(r - r0) <= tol and abs(g - g0) <= tol and abs(b - b0) <= tol:
                hit += 1
                if x < minx:
                    minx = x
                if y < miny:
                    miny = y
                if x > maxx:
                    maxx = x
                if y > maxy:
                    maxy = y
    frac = hit / tot
    if maxx < 0:
        bbox = None
    else:
        ox, oy = box[0], box[1]
        bbox = (ox + minx, oy + miny, ox + maxx + 1, oy + maxy + 1)
    print("find-color frac=%.4f hits=%d/%d bbox=%s target=%s tol=%d"
          % (frac, hit, tot, bbox, (r0, g0, b0), tol))
    return 0 if frac >= a.min_frac else 1


def cmd_delta(a):
    ia = _open_rgb(a.image_a)
    ib = _open_rgb(a.image_b)
    if ia.size != ib.size:
        # Resize B onto A so a differing capture size is not a false delta.
        ib = ib.resize(ia.size)
    if a.region:
        ia, box = _clip_region(ia, a.region)
        ib, _ = _clip_region(ib, a.region)
    else:
        box = (0, 0, ia.width, ia.height)
    diff = ImageChops.difference(ia, ib).convert("L")
    if a.thresh > 0:
        diff = diff.point(lambda v: 255 if v > a.thresh else 0)
    bbox = diff.getbbox()
    if bbox is not None:
        ox, oy = box[0], box[1]
        bbox = (ox + bbox[0], oy + bbox[1], ox + bbox[2], oy + bbox[3])
    print("delta bbox=%s region=%s thresh=%d" % (bbox, box, a.thresh))
    return 0 if bbox is not None else 1


def _region(v):
    parts = [int(x) for x in v.replace(",", " ").split()]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("region needs 4 ints: X0 Y0 X1 Y1")
    return tuple(parts)


def main(argv=None):
    p = argparse.ArgumentParser(description="AuraLite framebuffer oracle (WR-0)")
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("brightness")
    b.add_argument("image")
    b.add_argument("--floor", type=float, default=10.0)
    b.add_argument("--region", type=_region, default=None)
    b.set_defaults(fn=cmd_brightness)

    rb = sub.add_parser("region-brightness")
    rb.add_argument("image")
    rb.add_argument("--region", type=_region, default=None)
    rb.set_defaults(fn=cmd_region_brightness)

    bb = sub.add_parser("band-brighter")
    bb.add_argument("image")
    bb.add_argument("--band", type=_region, required=True)
    bb.add_argument("--ref", type=_region, required=True)
    bb.add_argument("--by", type=float, default=5.0)
    bb.set_defaults(fn=cmd_band_brighter)

    fc = sub.add_parser("find-color")
    fc.add_argument("image")
    fc.add_argument("r", type=int)
    fc.add_argument("g", type=int)
    fc.add_argument("b", type=int)
    fc.add_argument("--tol", type=int, default=24)
    fc.add_argument("--region", type=_region, default=None)
    fc.add_argument("--min-frac", type=float, default=0.0005)
    fc.set_defaults(fn=lambda a: cmd_find_color(_with_rgb(a)))

    d = sub.add_parser("delta")
    d.add_argument("image_a")
    d.add_argument("image_b")
    d.add_argument("--region", type=_region, default=None)
    d.add_argument("--thresh", type=int, default=16)
    d.set_defaults(fn=cmd_delta)

    a = p.parse_args(argv)
    return a.fn(a)


def _with_rgb(a):
    a.rgb = (a.r, a.g, a.b)
    return a


if __name__ == "__main__":
    sys.exit(main())
