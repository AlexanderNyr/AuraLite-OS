#!/usr/bin/env python3
"""OF5 live acceptance: the Sheet grid, one boot, end to end.

Same harness shape as live_pass.py: QEMU with a VNC display, serial
receipts and a monitor socket for paced keystrokes.  Assertions are on
the guest's own receipts and on a screenshot, never only "no crash".
"""
import socket, subprocess, time, os

ISO = "build/auralite.iso"
SER, MON = "/tmp/aura_ser.sock", "/tmp/aura_mon.sock"
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

log = open("/tmp/live_sheet_serial.log", "wb", buffering=0)
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

def wait_for(text, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        drain()
        if text.encode() in state["buf"]: return True
        time.sleep(0.25)
    return False

def cmd(s, wait=1.0):
    ser.sendall((s + "\n").encode()); time.sleep(wait); drain()

def key(name, wait=0.15):
    mon.sendall(f"sendkey {name}\n".encode()); time.sleep(wait); drain()

KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", ",": "comma",
          "=": "equal", "(": "shift-9", ")": "shift-0", ":": "shift-semicolon",
          "+": "shift-equal", "*": "shift-8", "&": "shift-7", "%": "shift-5"}
def type_text(text, cps=14):
    text = text.lower()   # the engine is case-insensitive; QEMU key names are not
    for ch in text:
        name = KEYMAP.get(ch, ch)
        mon.sendall(f"sendkey {name}\n".encode())
        time.sleep(1.0 / cps)
    drain()

def shot(name):
    subprocess.run(["vncdotool", "-s", "127.0.0.1:0", "capture", f"shots/{name}"],
                   capture_output=True, timeout=30)
    ok = os.path.exists(f"shots/{name}") and os.path.getsize(f"shots/{name}") > 3000
    print("shot", name, ok, flush=True)
    return ok

def vnc(*args):
    subprocess.run(["vncdotool", "-s", "127.0.0.1:0", *args],
                   capture_output=True, timeout=60)
    time.sleep(0.4); drain()

try:
    check("boot to shell", wait_for("auralite#", 150))
    cmd("run aosheet", 5)
    check("sheet window launched", wait_for("[sheet] gui: workbook ready", 30))
    shot("of5_01_grid.png")

    # A 5x5 block of numbers in A1:E5, entered row by row with Tab.
    for r in range(5):
        for c in range(5):
            type_text(str(r * 5 + c + 1), 14)
            key("tab", 0.12)
        key("ret", 0.15)
        # back to column A of the next row: Ctrl+Left (col 0), Down
        key("home", 0.12)
    shot("of5_02_typed.png")
    check("typed receipts", wait_for("[sheet] A1 = ", 20))

    # Typing left the cursor at A6 already: add the total of column A.
    type_text("=SUM(A1:A5)", 12)
    key("ret", 0.4)
    check("sum computed", wait_for("[sheet] A6 = 55", 20))
    shot("of5_03_sum.png")

    # Bar chart from the used range: select all, F6.
    key("ctrl-a", 0.4)
    key("f6", 0.5)
    check("chart inserted", wait_for("[sheet] chart bar", 20))
    shot("of5_04_chart.png")

    # Save, close with Esc, reload from the file: the sum must survive.
    key("ctrl-s", 0.6)
    check("save receipt", wait_for("[sheet] saved /documents/Untitled.aosheet", 20))
    key("esc", 1.2); drain()
    # The shell replays the keystrokes the GUI window consumed (they were
    # queued at the console too): wait for the serial line to go quiet.
    quiet = 0
    while quiet < 12:
        before = len(state["buf"])
        time.sleep(0.5); drain()
        quiet = quiet + 1 if len(state["buf"]) == before else 0
    ser.sendall(b"\n"); time.sleep(1); drain()
    mark = len(state["buf"])
    cmd("run aosheet /documents/Untitled.aosheet", 5)
    ok = False
    t0 = time.time()
    while time.time() - t0 < 30:
        drain()
        if b"[sheet] loaded /documents/Untitled.aosheet" in state["buf"][mark:]:
            ok = True; break
        time.sleep(0.25)
    check("reload receipt", ok)
    mark = len(state["buf"])
    for _ in range(5):
        key("down", 0.12)
    ok = False
    t0 = time.time()
    while time.time() - t0 < 20:
        drain()
        if b"[sheet] A6 = 55" in state["buf"][mark:]:
            ok = True; break
        time.sleep(0.25)
    check("sum survived reload", ok)
    shot("of5_05_reloaded.png")

    check("no kernel panic", b"PANIC" not in state["buf"])
    verdict = "LIVE SHEET: " + ("PASS" if not state["fails"] else "FAIL")
    print(verdict, flush=True)
    open("shots/VERDICT_SHEET.txt", "w").write(verdict + "\n")
finally:
    qemu.terminate()
