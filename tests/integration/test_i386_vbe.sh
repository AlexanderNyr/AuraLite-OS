#!/usr/bin/env bash
# tests/integration/test_i386_vbe.sh -- RESIDUE3 C3 / RES-12: the VBE path.
#
# Three lanes and one screenshot, all on the real `make iso` image:
#
#   1. qemu-system-x86_64 (SeaBIOS): Stage 2 probes VBE, sets 1024x768x32
#      with LIN_FA, publishes boot_info.fb, and the 64-bit kernel's WC
#      remap decodes the new framebuffer (768 pages at pitch 4096).  Before
#      RES-12 this lane printed "fb: none present" -- the BIOS boot had no
#      pixels at all.
#   2. qemu-system-i386: same loader lines, and the 32-bit kernel parses the
#      SAME boot_info_t at 32 bits (the width contract) and brings up a
#      PSF2 framebuffer console at the device-memory window 0xFD000000.
#      Its shadowless line is a recorded finding, not a failure.
#   3. Negative control, qemu-system-i386 -vga none: no VGA device, so no
#      VBE -- the loader must say so and the kernel must fall back to the
#      VGA text console.  A gate that only ever sees the happy path cannot
#      tell "VBE worked" from "VBE was never asked".
#
# The screenshot is the part no grep can substitute for: lane 2 dumps the
# framebuffer through the QEMU monitor and the pixel census must show the
# console's own two colours (bg 16,16,28 / fg 220,220,220) on a 1024x768
# surface.  "The kernel says it drew" and "pixels exist" are different
# claims, and this case asserts the second.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG64="$BUILD/i386_vbe_x64.log"
LOG32="$BUILD/i386_vbe_k32.log"
LOGNONE="$BUILD/i386_vbe_none.log"
SHOT="$BUILD/i386_vbe_k32.ppm"
MON="$BUILD/i386_vbe_k32.mon"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-vbe] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
ok()   { printf '  [i386-vbe] OK   %s\n' "$1"; }
bad()  { printf '  [i386-vbe] FAIL %s\n' "$1"; fail=1; }

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then ok "$desc"; else bad "$desc (no match: $pat)"; fi
}
assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then bad "$desc (matched: $pat)"; else ok "$desc"; fi
}

echo "[i386-vbe] RES-12: VBE linear framebuffer"

# ---- lane 1: the 64-bit kernel on the BIOS path ---------------------------
rm -f "$LOG64"
timeout 60 qemu-system-x86_64 \
    -drive format=raw,file="$ISO",if=ide,snapshot=on -boot order=c \
    -m 512M -vga std -display none -serial "file:$LOG64" -no-reboot \
    -fw_cfg name=opt/auralite.selftest,string=fast \
    >/dev/null 2>&1 || true
assert_grep "$LOG64" "\[vbe\] selecting 1024x768x32 (linear framebuffer)" \
            "loader picked 1024x768x32"
assert_grep "$LOG64" "\[vbe\] framebuffer live; boot_info.fb filled" \
            "loader published boot_info.fb"
assert_grep "$LOG64" "\[boot\] framebuffer console initialised" \
            "64-bit kernel brought the console up"
assert_grep "$LOG64" "fb: WC via PAT4 (768 pages" \
            "H3 WC remap now decodes the BIOS framebuffer"

# ---- lane 2: the 32-bit kernel, with a screenshot -------------------------
rm -f "$LOG32" "$SHOT" "$MON"
timeout 75 qemu-system-i386 \
    -drive format=raw,file="$ISO",if=ide,snapshot=on -boot order=c \
    -m 512M -vga std -display none -serial "file:$LOG32" -no-reboot \
    -monitor "unix:$MON,server,nowait" \
    >/dev/null 2>&1 &
QPID=$!
for _ in $(seq 1 100); do [ -S "$MON" ] && break; sleep 0.2; done
# Let the 32-bit kernel reach its console, then capture the actual surface.
sleep 22
python3 - "$MON" "$SHOT" <<'PY' || true
import socket, sys, time
mon, shot = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX)
s.connect(mon)
time.sleep(0.3)
s.send(b"screendump " + shot.encode() + b"\n")
time.sleep(1.5)
try:
    s.recv(65536)
except OSError:
    pass
s.send(b"quit\n")
PY
wait "$QPID" 2>/dev/null || true

assert_grep "$LOG32" "\[vbe\] selecting 1024x768x32 (linear framebuffer)" \
            "32-bit lane: loader picked 1024x768x32"
assert_grep "$LOG32" "\[vbe\] framebuffer live; boot_info.fb filled" \
            "32-bit lane: boot_info.fb published"
assert_grep "$LOG32" "\[boot\] framebuffer console online (1024x768x32, pitch 4096" \
            "32-bit kernel read the fb block through boot_info_t"
assert_grep "$LOG32" "\[fb\] shadowless console (i386" \
            "32-bit console states its shadowless finding"
assert_no_grep "$LOG32" "\[boot\] VGA text console online" \
            "the VGA console is NOT the primary sink any more"

if [ -s "$SHOT" ]; then
    ok "monitor screendump captured $(wc -c < "$SHOT") bytes"
    if python3 - "$SHOT" <<'PY'
import sys
from collections import Counter
data = open(sys.argv[1], "rb").read()
head = data.split(b"\n", 3)
assert head[0] == b"P6", "not a P6 PPM"
w, h = (int(v) for v in head[1].split())
px = head[3]
assert (w, h) == (1024, 768), "surface is %dx%d, expected 1024x768" % (w, h)
c = Counter()
for i in range(0, len(px) - 2, 3 * 5):
    c[(px[i], px[i + 1], px[i + 2])] += 1
bg, fg = (16, 16, 28), (220, 220, 220)
assert c[bg] > 0, "console background colour absent"
assert c[fg] > 200, "console glyph colour only %d samples (want > 200)" % c[fg]
print("      %dx%d surface, %d bg + %d fg samples"
      % (w, h, c[bg], c[fg]))
PY
    then ok "screenshot shows the 32-bit console's own pixels"
    else bad "screenshot pixel census"; fi
else
    bad "no screendump produced"
fi

# ---- lane 3: the negative control ----------------------------------------
rm -f "$LOGNONE"
timeout 40 qemu-system-i386 \
    -drive format=raw,file="$ISO",if=ide,snapshot=on -boot order=c \
    -m 512M -vga none -display none -serial "file:$LOGNONE" -no-reboot \
    -fw_cfg name=opt/auralite.selftest,string=fast \
    >/dev/null 2>&1 || true
assert_grep "$LOGNONE" "\[vbe\] no 32-bpp linear-framebuffer mode" \
            "no VGA device => the loader refuses honestly"
assert_grep "$LOGNONE" "\[boot\] VGA text console online" \
            "no framebuffer => the 32-bit kernel keeps text mode"

if [ "$fail" -ne 0 ]; then
    echo "[i386-vbe] FAILED"
    exit 1
fi
echo "[i386-vbe] PASS"
