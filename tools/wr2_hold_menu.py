#!/usr/bin/env python3
"""WR-2 popup visual: press the menu bar's first item and HOLD (no release),
so the modal TrackPopupMenu dropdown stays on screen long enough to screendump.
Usage: wr2_hold_menu.py X Y HOLD_SEC"""
import sys, time, subprocess
sys.path.insert(0, "/home/user/AuraLite-OS/tools")
import gui_input as gi

x, y = int(sys.argv[1]), int(sys.argv[2])
hold = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0

s = gi._qmp_connect("/tmp/wr2lab/qmp.sock")
gi._send_events(s, gi._abs_events(x, y, 1280, 800), None)
time.sleep(0.05)
gi._send_events(s, gi._abs_events(x, y, 1280, 800) +
                [{"type": "btn", "data": {"down": True, "button": "left"}}], None)
time.sleep(0.8)
# shot while the button is held and the modal loop is up
subprocess.run(["python3", "/home/user/AuraLite-OS/tools/gui_input.py",
                "shot", "--monitor", "/tmp/wr2lab/mon.sock",
                "/tmp/wr2lab/popup.png"], check=False)
time.sleep(hold)
gi._send_events(s, [{"type": "btn", "data": {"down": False, "button": "left"}}], None)
s.close()
print("held", x, y)
