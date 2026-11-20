#!/usr/bin/env bash
# Test the OVMF override and the actual first browser frame, not a buffer hash.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh
gl_require_ovmf

il_section "UEFI boot resolution and visible gbrowser startup"
GL_SELFTEST=fast
GL_LOG="$IL_LOGDIR/gui_resolution_browser.log"
GL_SHOTDIR="$IL_LOGDIR/gui-resolution-browser-shots"
trap 'gl_stop; il_dump_on_error' EXIT

gl_boot -fw_cfg name=opt/auralite.resolution,string=1920x1080 || {
    il_fail "could not boot the requested UEFI mode"; il_summary; exit 1;
}
il_assert_grep_fixed "$GL_LOG" "[BL6] requested GOP resolution active" "loader selected the requested mode"

gl_cmd 'run gbrowser' '\[gbrowser\] page rendered and presented' 30 || {
    il_fail "browser could not present its first frame"; il_summary; exit 1;
}
# One frame can be in flight when the serial marker arrives. The browser
# must not depend on input events or on test-mode background repaints.
sleep 1
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_shot browser && python3 - "$GL_LAST_PNG" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
assert im.size == (1920, 1080), f'actual GOP framebuffer is {im.size}'
# The browser opens at (40,40); sample inside the address bar and demo
# HTML, away from the window borders, shadow and default cursor.
bar = im.crop((200, 78, 780, 93))
page = im.crop((50, 104, 620, 245))
white = (255, 255, 255)
assert sum(n for n, p in bar.getcolors(16384) if p == white) > 8000, 'address field did not reach the screen'
assert sum(n for n, p in page.getcolors(65536) if p == white) > 50000, 'white demo page missing'
assert sum(n for n, p in page.getcolors(65536) if max(p) < 100) > 250, 'demo page text missing'
PY
then
    il_pass "1920x1080 framebuffer contains the URL bar and rendered HTML"
else
    il_fail "wrong framebuffer size or missing browser pixels"
fi
il_assert_no_grep "$GL_LOG" 'paint smoke:|blit benchmark' 'normal startup skips test workloads'
il_summary
