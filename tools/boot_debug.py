#!/usr/bin/env python3
"""Boot NovOS in a managed QEMU subprocess, capture a framebuffer screenshot
and the serial log, for headless debugging in the sandbox.

Produces build/screen.png (from QEMU screendump, PPM->PNG if needed) and prints
build/serial.log. QEMU is always terminated before the script exits.
"""
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = "/home/user/novos"
ISO = ROOT + "/build/novos.iso"
SOCK = ROOT + "/build/qmon.sock"
SER = ROOT + "/build/serial.log"
PPM = ROOT + "/build/screen.ppm"
PNG = ROOT + "/build/screen.png"


def hmp(sock, cmd, settle=1.5):
    sock.sendall((cmd + "\n").encode())
    time.sleep(settle)
    out = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    return out.decode(errors="replace")


def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", "screendump produced non-P6 PPM"
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        if data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx] not in b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\n\r":
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1  # one whitespace after maxval
    w, h, _mx = fields
    raw = data[idx:idx + w * h * 3]

    def chunk(typ, payload):
        c = struct.pack(">I", len(payload)) + typ + payload
        crc = zlib.crc32(typ + payload) & 0xFFFFFFFF
        return c + struct.pack(">I", crc)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8-bit RGB
    scan = bytearray()
    for y in range(h):
        scan.append(0)
        scan.extend(raw[y * w * 3:(y + 1) * w * 3])
    idat = zlib.compress(bytes(scan), 9)
    with open(png_path, "wb") as f:
        f.write(sig + chunk(b"IHDR", ihdr) +
                chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def main():
    try:
        os.remove(SOCK)
    except FileNotFoundError:
        pass

    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-cdrom", ISO, "-m", "512M",
        "-vga", "std", "-display", "none",
        "-serial", "file:" + SER, "-no-reboot", "-cpu", "qemu64",
        "-monitor", "unix:" + SOCK + ",server=on,wait=off",
    ], stderr=subprocess.STDOUT)

    try:
        boot_wait = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
        time.sleep(boot_wait)

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2.0)
        for _ in range(30):
            try:
                s.connect(SOCK)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        time.sleep(0.3)
        try:
            s.recv(4096)
        except socket.timeout:
            pass

        hmp(s, "screendump " + PNG + " png", 1.5)
        if not (os.path.exists(PNG) and os.path.getsize(PNG) > 0):
            hmp(s, "screendump " + PPM, 1.5)
            if os.path.exists(PPM) and os.path.getsize(PPM) > 0:
                ppm_to_png(PPM, PNG)
        status = hmp(s, "info status", 0.5)
        print("QEMU status:", status.strip())
        s.close()
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()

    print("=== SERIAL LOG ===")
    try:
        print(open(SER).read())
    except FileNotFoundError:
        print("(no serial log)")
    print("=== SCREENSHOT ===")
    print(PNG, os.path.getsize(PNG) if os.path.exists(PNG) else "MISSING")


if __name__ == "__main__":
    main()
