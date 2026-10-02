#!/usr/bin/env python3
# tools/gui_input.py — WR-0 (W32RUN_PLAN.md) the live console + input harness.
#
# The live-GUI lane runs QEMU with three UNIX sockets: a serial socket (the
# AuraLite shell), a monitor socket (HMP — screendump, keyboard) and a QMP
# socket (the absolute pointer; see the note above cmd_click for why HMP
# cannot drive a usb-tablet).
# This tool is the only thing that talks to them, so the lane script stays
# declarative.  It replaces the throwaway /tmp helpers used during WR-0
# bring-up and folds their two lessons in:
#
#   * the serial stream is flooded with buffer-cache "[bc] writeback ..."
#     spam — every read filters it out;
#   * the monitor works even under `-display none`, which is how a headless
#     CI box captures the GOP framebuffer.
#
# Subcommands (each takes --serial SOCK and/or --monitor SOCK):
#
#   serial-send   --serial S CMD...        send a shell command (+CR)
#   serial-drain  --serial S [--secs N]    print whatever serial emits for N s
#   serial-wait   --serial S --pattern RE [--timeout T]
#                                          block until RE appears; exit 1 on
#                                          timeout.  The right way to wait for
#                                          `auralite#` instead of guessing.
#   mon           --monitor M HMP...       raw HMP command, print the reply
#   key           --monitor M K...         `sendkey` each K (e.g. ret tab a)
#   type          --monitor M "text"       type ASCII as sendkey chords
#   shot          --monitor M /abs/out.ppm screendump to an absolute path
#   move          --qmp Q X Y              absolute pointer move
#   click         --qmp Q X Y              absolute move + click
#   dblclick      --qmp Q X Y              two clicks inside the guest's
#                                          double-click window
#
# Only the standard library is used.

import argparse
import json
import re
import socket
import sys
import time

BC_SPAM = re.compile(r"\[bc\][^\n]*\n?")

# ASCII -> HMP sendkey token(s).  Letters/digits map to themselves; the rest
# go through this table.  Upper-case and shifted symbols prepend `shift-`.
_PLAIN = {
    " ": "spc", "\n": "ret", "\t": "tab",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    "\\": "backslash", ",": "comma", ".": "dot", "/": "slash",
}
_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
    "{": "bracket_left", "}": "bracket_right", ":": "semicolon",
    '"': "apostrophe", "~": "grave_accent", "|": "backslash",
    "<": "comma", ">": "dot", "?": "slash",
}


def _connect(path, timeout=10.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    return s


def _drain(sock, secs, strip_spam=True):
    """Read from sock for `secs` seconds, return decoded text."""
    out = []
    end = time.time() + secs
    sock.settimeout(0.3)
    while time.time() < end:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        out.append(chunk.decode("latin-1"))
    text = "".join(out)
    if strip_spam:
        text = BC_SPAM.sub("", text)
    return text


def _mon_send(sock, cmd, settle=0.4):
    """Send one HMP command; return the reply text (banner-tolerant)."""
    # Give the monitor a moment to emit its (qemu) banner on first use.
    try:
        pre = _drain(sock, 0.2, strip_spam=False)
    except Exception:
        pre = ""
    sock.sendall((cmd + "\n").encode("latin-1"))
    time.sleep(settle)
    reply = _drain(sock, 0.4, strip_spam=False)
    return pre + reply


def cmd_serial_send(a):
    s = _connect(a.serial)
    line = " ".join(a.words)
    s.sendall((line + "\r").encode("latin-1"))
    # Echo back a short window so the caller sees the command land.
    sys.stdout.write(_drain(s, a.secs))
    s.close()
    return 0


def cmd_serial_drain(a):
    s = _connect(a.serial)
    sys.stdout.write(_drain(s, a.secs))
    s.close()
    return 0


def _append_log(path, text):
    if not path or not text:
        return
    try:
        with open(path, "a") as f:
            f.write(text)
    except OSError as e:
        sys.stderr.write("gui_input: cannot append to %s: %s\n" % (path, e))


def _read_until(sock, pattern, timeout, logpath):
    """Read from sock (filtering [bc] spam, teeing to logpath) until `pattern`
    matches or `timeout` elapses.  Returns (matched: bool, buf: str)."""
    rx = re.compile(pattern) if pattern else None
    buf = ""
    end = time.time() + timeout
    sock.settimeout(0.5)
    while time.time() < end:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            if rx is None:
                # No pattern: drain for the whole window.
                continue
            continue
        except OSError:
            break
        if not chunk:
            break
        piece = BC_SPAM.sub("", chunk.decode("latin-1"))
        buf += piece
        _append_log(logpath, piece)
        if rx is not None and rx.search(buf):
            return True, buf
    return (rx is None), buf


def cmd_serial_wait(a):
    s = _connect(a.serial)
    matched, buf = _read_until(s, a.pattern, a.timeout, a.log)
    s.close()
    if matched:
        print("serial-wait: matched /%s/" % a.pattern)
        return 0
    sys.stderr.write("serial-wait: TIMEOUT after %gs waiting for /%s/\n"
                     % (a.timeout, a.pattern))
    return 1


def cmd_serial_run(a):
    """One connection: send CMD, then read until --wait (or for --secs),
    teeing to --log.  The primary lane verb — avoids losing shell output
    between a separate send and wait."""
    s = _connect(a.serial)
    s.sendall((a.cmd + "\r").encode("latin-1"))
    if a.wait:
        matched, buf = _read_until(s, a.wait, a.timeout, a.log)
        s.close()
        sys.stdout.write(buf)
        if not matched:
            sys.stderr.write("serial-run: TIMEOUT waiting for /%s/\n" % a.wait)
            return 1
        return 0
    _, buf = _read_until(s, None, a.secs, a.log)
    s.close()
    sys.stdout.write(buf)
    return 0


def cmd_mon(a):
    s = _connect(a.monitor)
    sys.stdout.write(_mon_send(s, " ".join(a.words)))
    s.close()
    return 0


def cmd_key(a):
    s = _connect(a.monitor)
    for k in a.keys:
        _mon_send(s, "sendkey %s" % k, settle=a.delay)
    s.close()
    return 0


def _char_to_key(ch):
    if ch.isalnum() and ord(ch) < 128:
        if ch.isupper():
            return "shift-" + ch.lower()
        return ch
    if ch in _PLAIN:
        return _PLAIN[ch]
    if ch in _SHIFTED:
        return "shift-" + _SHIFTED[ch]
    return None


def cmd_type(a):
    s = _connect(a.monitor)
    for ch in a.text:
        k = _char_to_key(ch)
        if k is None:
            sys.stderr.write("type: skipping unmapped char %r\n" % ch)
            continue
        _mon_send(s, "sendkey %s" % k, settle=a.delay)
    s.close()
    return 0


def cmd_shot(a):
    s = _connect(a.monitor)
    _mon_send(s, "screendump %s" % a.path, settle=a.settle)
    s.close()
    print("shot -> %s" % a.path)
    return 0


# ---------------------------------------------------------------------------
# QMP: the pointer path.
#
# HMP's mouse_move/mouse_button were the obvious choice and they are WRONG for
# this lane.  Modern QEMU implements mouse_move with qemu_input_queue_rel() --
# RELATIVE motion -- while `-device usb-tablet` is an ABSOLUTE pointer.  The
# monitor accepts the command, answers with a clean prompt, and nothing moves.
# Measured on the WR-2 lane: `info mice` listed "* Mouse #3: QEMU HID Tablet
# (absolute)" as current, the commands were accepted without error, and the
# guest's HID driver logged 0 reports across 600 polls (6 s).  The same guest,
# same EHCI interrupt path, receives reports fine from a usb-kbd driven by
# `sendkey` (test_usb_hid_input is 9/9), so the gap was never in the OS.
#
# input-send-event with `abs` axes is the supported way to drive an absolute
# pointer, and it exists only on QMP -- hence the lane's second socket.
# Absolute axis values are in QEMU's 0..32767 normalised range.
ABS_MAX = 32767


def _qmp_connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10.0)
    s.connect(path)
    _qmp_read(s)                      # the greeting
    _qmp_cmd(s, {"execute": "qmp_capabilities"})
    return s


def _qmp_read(sock, secs=1.0):
    sock.settimeout(secs)
    buf = b""
    try:
        while True:
            d = sock.recv(65536)
            if not d:
                break
            buf += d
            if b"\n" in d:
                break
    except Exception:
        pass
    return buf.decode("utf-8", "replace")


def _qmp_cmd(sock, obj):
    sock.sendall((json.dumps(obj) + "\n").encode("utf-8"))
    return _qmp_read(sock)


def _abs_events(x, y, width, height):
    return [
        {"type": "abs",
         "data": {"axis": "x", "value": int(x * ABS_MAX / max(1, width))}},
        {"type": "abs",
         "data": {"axis": "y", "value": int(y * ABS_MAX / max(1, height))}},
    ]


def _send_events(sock, events, device=None):
    """Address the pointer by qdev id.

    Without `device`, input-send-event goes to the default console's input
    routing.  Under `-display none` that is not where the usb-tablet is, so
    QEMU answers {"return": {}} and nothing reaches the guest -- which is
    exactly the silent no-op this lane spent a day chasing.  The lane gives
    the tablet `id=gltablet` so the events can be addressed to it."""
    args = {"events": events}
    if device:
        args["device"] = device
    return _qmp_cmd(sock, {"execute": "input-send-event", "arguments": args})


def cmd_click(a):
    """Move the absolute pointer to (x, y) and click there.

    The move is sent as its own event batch before the button: a tablet
    reports position and buttons together, and a guest that tracks motion
    (the compositor moves its cursor on every report) must see the pointer
    arrive before it is told the button went down.
    """
    s = _qmp_connect(a.qmp)
    _send_events(s, _abs_events(a.x, a.y, a.width, a.height), a.device)
    time.sleep(0.05)
    _send_events(s, _abs_events(a.x, a.y, a.width, a.height) +
                    [{"type": "btn", "data": {"down": True, "button": "left"}}],
                 a.device)
    time.sleep(0.05)
    _send_events(s, [{"type": "btn", "data": {"down": False, "button": "left"}}],
                 a.device)
    s.close()
    print("click %d,%d" % (a.x, a.y))
    return 0


def cmd_dblclick(a):
    """Two clicks inside the guest's double-click window.

    gui.c uses MOUSE_DBLCLICK_TICKS (40 ticks ~= 400 ms at 99 Hz) and a 5 px
    slop, so the two presses must be close in both time and place; a naive
    `click; sleep 1; click` is two single clicks and was the first wrong
    reading this lane produced.
    """
    s = _qmp_connect(a.qmp)
    move = _abs_events(a.x, a.y, a.width, a.height)
    _send_events(s, move, a.device)
    time.sleep(0.05)
    for _ in range(2):
        _send_events(s, move +
                     [{"type": "btn", "data": {"down": True, "button": "left"}}],
                     a.device)
        time.sleep(0.04)
        _send_events(s, [{"type": "btn",
                          "data": {"down": False, "button": "left"}}], a.device)
        time.sleep(0.06)
    s.close()
    print("dblclick %d,%d" % (a.x, a.y))
    return 0


def cmd_move(a):
    s = _qmp_connect(a.qmp)
    _send_events(s, _abs_events(a.x, a.y, a.width, a.height), a.device)
    s.close()
    print("move %d,%d" % (a.x, a.y))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="AuraLite live console/input (WR-0)")
    sub = p.add_subparsers(dest="cmd", required=True)

    ss = sub.add_parser("serial-send")
    ss.add_argument("--serial", required=True)
    ss.add_argument("--secs", type=float, default=1.0)
    ss.add_argument("words", nargs="+")
    ss.set_defaults(fn=cmd_serial_send)

    sd = sub.add_parser("serial-drain")
    sd.add_argument("--serial", required=True)
    sd.add_argument("--secs", type=float, default=3.0)
    sd.set_defaults(fn=cmd_serial_drain)

    sw = sub.add_parser("serial-wait")
    sw.add_argument("--serial", required=True)
    sw.add_argument("--pattern", required=True)
    sw.add_argument("--timeout", type=float, default=90.0)
    sw.add_argument("--log", default=None)
    sw.set_defaults(fn=cmd_serial_wait)

    sr = sub.add_parser("serial-run")
    sr.add_argument("--serial", required=True)
    sr.add_argument("--cmd", required=True)
    sr.add_argument("--wait", default=None)
    sr.add_argument("--secs", type=float, default=3.0)
    sr.add_argument("--timeout", type=float, default=60.0)
    sr.add_argument("--log", default=None)
    sr.set_defaults(fn=cmd_serial_run)

    mo = sub.add_parser("mon")
    mo.add_argument("--monitor", required=True)
    mo.add_argument("words", nargs="+")
    mo.set_defaults(fn=cmd_mon)

    ky = sub.add_parser("key")
    ky.add_argument("--monitor", required=True)
    ky.add_argument("--delay", type=float, default=0.15)
    ky.add_argument("keys", nargs="+")
    ky.set_defaults(fn=cmd_key)

    ty = sub.add_parser("type")
    ty.add_argument("--monitor", required=True)
    ty.add_argument("--delay", type=float, default=0.08)
    ty.add_argument("text")
    ty.set_defaults(fn=cmd_type)

    sh = sub.add_parser("shot")
    sh.add_argument("--monitor", required=True)
    sh.add_argument("--settle", type=float, default=0.6)
    sh.add_argument("path")
    sh.set_defaults(fn=cmd_shot)

    for name, fn in (("click", cmd_click), ("dblclick", cmd_dblclick),
                     ("move", cmd_move)):
        c = sub.add_parser(name)
        c.add_argument("--qmp", required=True)
        c.add_argument("--monitor", default=None)   # accepted, unused
        c.add_argument("x", type=int)
        c.add_argument("y", type=int)
        c.add_argument("--width", type=int, default=1280)
        c.add_argument("--height", type=int, default=800)
        c.add_argument("--device", default="gltablet")
        c.set_defaults(fn=fn)

    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
