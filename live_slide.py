#!/usr/bin/env python3
"""OF6 live acceptance: the Slide editor and show, one boot, end to end.

Same harness shape as live_sheet.py: QEMU with a VNC display, serial
receipts and a monitor socket for paced keystrokes.  Assertions are on the
guest's own receipts and on screenshots, never only "no crash".
"""
import socket, subprocess, time, os

ISO = "build/auralite.iso"
SER, MON = "/tmp/aura_slide_ser.sock", "/tmp/aura_slide_mon.sock"
for f in (SER, MON):
    try: os.unlink(f)
    except FileNotFoundError: pass
os.makedirs("shots", exist_ok=True)

qemu = subprocess.Popen([
    "qemu-system-x86_64", "-drive", f"file={ISO},format=raw,if=ide,snapshot=on",
    "-boot", "order=c", "-m", "512M", "-smp", "2", "-vga", "std",
    "-vnc", "0.0.0.0:0", "-serial", f"unix:{SER},server,nowait",
    "-monitor", f"unix:{MON},server,nowait", "-no-reboot", "-cpu", "qemu64",
    "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
    "-usb", "-device", "usb-tablet",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

log = open("/tmp/live_slide_serial.log", "wb", buffering=0)
state = {"buf": b"", "fails": []}
def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name, flush=True)
    if not cond: state["fails"].append(name)

def connect(path):
    for _ in range(150):
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(path); return s
        except OSError: time.sleep(0.2)
    raise RuntimeError(path)

ser = connect(SER); ser.settimeout(0.3)
mon = connect(MON); mon.settimeout(0.3)
def drain():
    try:
        while True:
            d = ser.recv(65536)
            if not d: break
            state["buf"] += d; log.write(d)
    except socket.timeout: pass
    try:
        while mon.recv(65536): pass
    except socket.timeout: pass

def wait_for(text, timeout=120, mark=0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        drain()
        if text.encode() in state["buf"][mark:]: return True
        time.sleep(0.25)
    return False

def cmd(s, wait=1.0):
    ser.sendall((s + "\n").encode()); time.sleep(wait); drain()

def key(name, wait=0.15):
    mon.sendall(f"sendkey {name}\n".encode()); time.sleep(wait); drain()

def click_at(x, y, wait=0.6):
    result = subprocess.run(["vncdotool", "-s", "127.0.0.1:0",
                             "move", str(x), str(y), "click", "1"],
                            capture_output=True, timeout=20)
    if result.returncode != 0:
        raise RuntimeError("VNC click failed: " + result.stderr.decode("utf-8", "replace"))
    time.sleep(wait); drain()

def shot(name):
    subprocess.run(["vncdotool", "-s", "127.0.0.1:0", "capture", f"shots/{name}"],
                   capture_output=True, timeout=30)
    ok = os.path.exists(f"shots/{name}") and os.path.getsize(f"shots/{name}") > 3000
    print("shot", name, ok, flush=True)
    return ok

try:
    check("boot to shell", wait_for("auralite#", 150))
    cmd("run aoslide --demo /documents/Untitled.aoslide", 6)
    check("demo deck written", wait_for("SLIDE DEMO: wrote /documents/Untitled.aoslide", 30))
    check("demo self-check (serialize -> parse round-trip)",
          wait_for("SLIDE DEMO: self-check OK", 10))

    cmd("run aoslide /documents/Untitled.aoslide", 5)
    check("editor launched", wait_for("[slide] gui: deck ready (3 slides)", 30))
    shot("of6_01_editor.png")

    # F5 opens the borderless fullscreen show on slide 1.
    key("f5", 2.0)
    check("show: slide 1/3 receipt", wait_for("SLIDE-SHOW: slide 1/3", 15))
    shot("of6_02_show_1.png")

    click_at(512, 384, 1.0)
    check("show: mouse click advanced to slide 2/3",
          wait_for("SLIDE-SHOW: slide 2/3", 15))
    shot("of6_03_show_2.png")

    key("spc", 1.0)
    check("show: Space advanced to slide 3/3",
          wait_for("SLIDE-SHOW: slide 3/3", 15))
    shot("of6_04_show_3.png")

    # One more advance walks off the end: the show ends and closes.
    key("spc", 1.0)
    check("show: advancing past the last slide ends the show",
          wait_for("SLIDE-SHOW: end", 15))
    shot("of6_05_show_end.png")

    # Re-enter to exercise the presenter's dedicated Esc exit path.
    mark = len(state["buf"])
    key("f5", 1.5)
    check("show can start again", wait_for("SLIDE-SHOW: slide 1/3", 15, mark))
    key("esc", 1.5)
    check("Esc exits the active show", wait_for("SLIDE-SHOW: end (Esc)", 15))
    shot("of6_06_editor_back.png")

    # Esc again closes the editor to the shell.
    key("esc", 1.5)
    # The console replays the window's keystrokes: let it drain, then prove
    # the shell is responsive and the saved deck re-reads from disk.
    quiet = 0
    while quiet < 12:
        before = len(state["buf"]); time.sleep(0.5); drain()
        quiet = quiet + 1 if len(state["buf"]) == before else 0
    ser.sendall(b"\n"); time.sleep(1); drain()
    mark = len(state["buf"])
    cmd("run aoslide --show /documents/Untitled.aoslide", 5)
    check("headless show replays all slides", wait_for("SLIDE-SHOW: PASS", 30, mark))

    # The three slides must differ (accent bar / polyline / ellipse).
    def md5(name):
        import hashlib
        with open(f"shots/{name}", "rb") as f: return hashlib.md5(f.read()).hexdigest()
    s1, s2, s3 = md5("of6_02_show_1.png"), md5("of6_03_show_2.png"), md5("of6_04_show_3.png")
    check("show: slide 1 differs from slide 2", s1 != s2)
    check("show: slide 2 differs from slide 3", s2 != s3)

    from PIL import Image
    def bottom_pixel(name):
        with Image.open(f"shots/{name}").convert("RGB") as im:
            return im.getpixel((im.width // 2, im.height - 8))
    editor_edge = bottom_pixel("of6_01_editor.png")
    show_edge = bottom_pixel("of6_02_show_1.png")
    returned_edge = bottom_pixel("of6_06_editor_back.png")
    check("presentation covers the taskbar (black bottom edge)",
          max(show_edge) <= 8 and show_edge != editor_edge)
    check("Esc returns desktop chrome", returned_edge == editor_edge)

    check("no kernel panic", b"PANIC" not in state["buf"])
    verdict = "LIVE SLIDE: " + ("PASS" if not state["fails"] else "FAIL")
    print(verdict, flush=True)
    open("shots/VERDICT_SLIDE.txt", "w").write(verdict + "\n")
finally:
    qemu.terminate()
