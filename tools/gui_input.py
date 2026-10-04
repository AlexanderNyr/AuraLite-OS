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
# this lane's TABLET.  Modern QEMU implements mouse_move with
# qemu_input_queue_rel() -- RELATIVE motion -- while `-device usb-tablet` is an
# ABSOLUTE pointer.  The monitor accepts the command, answers with a clean
# prompt, and nothing moves.  Measured on the WR-2 lane: `info mice` listed
# "* Mouse #3: QEMU HID Tablet (absolute)" as current, the commands were
# accepted without error, and the guest's HID driver logged 0 reports across
# 600 polls (6 s).  The same guest, same EHCI interrupt path, receives reports
# fine from a usb-kbd driven by `sendkey` (test_usb_hid_input is 9/9), so the
# gap was never in the OS.
#
# input-send-event with `abs` axes is the supported way to drive an absolute
# pointer, and it exists only on QMP -- hence the lane's second socket.
# Absolute axis values are in QEMU's 0..32767 normalised range.
#
# QEMU >= 9 aborts the whole emulator on input-send-event, though:
#     Unexpected error in object_property_find_err() at qom/object.c:
#     Property 'qemu-fixed-text-console.device' not found
# measured on Debian 13's qemu 10.0.13 for EVERY event type (abs, rel, btn),
# device-addressed or not, with -display none.  The command is simply not
# usable there.  The PS/2 lane below is the portable fallback: QEMU's machine
# always has an emulated PS/2 mouse (the guest boots "[mouse] IntelliMouse
# wheel mode enabled"), HMP mouse_move/mouse_button are ancient and stable,
# and `mouse_set` makes that PS/2 mouse the *current* device so the relative
# events actually reach it.  The pointer verbs auto-select the path from the
# QEMU major version (query-version via QMP, info version as fallback).
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


# ---------------------------------------------------------------------------
# The PS/2 relative pointer path (QEMU >= 9, where input-send-event aborts).
# All of it goes through the HMP monitor socket: mouse_set to select the PS/2
# mouse, mouse_move for relative motion (positive dy = DOWN on screen, as the
# PS/2 protocol carries y-up), mouse_button for the buttons (1=L 2=M 4=R).
# ---------------------------------------------------------------------------

REL_STEP_PX = 60     # per-command delta.  Bigger bursts (100+ px several times
                     # in quick succession) can outrun the guest's IRQ12 drain
                     # and lose movement (measured: 3x100 px at 0.12 s spacing
                     # moved the cursor 19 px), so keep the steps small.
REL_STEP_SEC = 0.12
CURSOR_TOL = 60      # per-channel match tolerance for the cursor sprite


def _qemu_major(a):
    """QEMU major version via QMP query-version (a pure query; the >=9 abort
    is specific to input-send-event), falling back to HMP `info version`.
    None when nothing answerable is reachable."""
    if getattr(a, "qmp", None):
        try:
            s = _qmp_connect(a.qmp)
            r = _qmp_cmd(s, {"execute": "query-version"})
            s.close()
            return int(r["return"]["qemu"]["major"])
        except Exception:
            pass
    if getattr(a, "monitor", None):
        try:
            s = _connect(a.monitor)
            out = _mon_send(s, "info version")
            s.close()
            m = re.search(r"QEMU (\d+)\.", out)
            if m:
                return int(m.group(1))
        except Exception:
            pass
    return None


def _ps2_select(monitor):
    """Make the emulated PS/2 mouse the current HMP mouse (info mice -> the
    'QEMU PS/2 Mouse' index), so mouse_move/mouse_button reach a device that
    actually reports relative motion.  Returns the index or None."""
    s = _connect(monitor)
    try:
        out = _mon_send(s, "info mice")
        m = re.search(r"Mouse #(\d+):[^\r\n]*PS/2", out)
        if not m:
            return None
        idx = int(m.group(1))
        _mon_send(s, "mouse_set %d" % idx)
        return idx
    finally:
        s.close()


def _cursor_template():
    """The compositor's arrow cursor (kernel/gui/gui.c draw_cursor) as two
    pixel lists, sprite-local: the white fill and the black outline that
    overdraws it.  Matching only constrains pixels the sprite itself paints,
    so the match works over any background colour."""
    white = set()
    for i in range(16):
        for j in range(i // 2 + 1):
            white.add((j, i))
    black = set()

    def line(x0, y0, x1, y1):
        # Bresenham, identical to gfx_draw_line in drivers/framebuffer/graphics.c
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            black.add((x0, y0))
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    line(0, 0, 0, 15)
    line(0, 15, 7, 11)
    line(0, 0, 11, 11)
    line(7, 11, 5, 15)
    white -= black                    # the outline is drawn after the fill
    return sorted(white), sorted(black)


_CURSOR_WHITE, _CURSOR_BLACK = _cursor_template()


def _read_ppm(path):
    """Parse a binary P6 screendump into (width, height, bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM: %s" % path)
    pos, fields = 2, []
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1                          # exactly one whitespace before the raster
    w, h, _max = fields
    return w, h, data[pos:pos + w * h * 3]


def _cursor_locate(monitor, settle=0.5):
    """Screendump via HMP and find the arrow cursor sprite.  Returns (x, y)
    (the sprite's top-left == the guest's mouse_x/mouse_y) or None."""
    import tempfile, os
    tmp = tempfile.mktemp(suffix=".ppm", prefix="guicur_")
    try:
        s = _connect(monitor)
        try:
            if os.path.exists(tmp):
                os.unlink(tmp)
            _mon_send(s, "screendump %s" % tmp, settle=settle)
        finally:
            s.close()
        for _ in range(40):           # wait for the dump to land + stabilise
            try:
                if os.path.getsize(tmp) > 0:
                    with open(tmp, "rb") as f:
                        head = f.read(64)
                    if head.startswith(b"P6"):
                        break
            except OSError:
                pass
            time.sleep(0.1)
        w, h, raster = _read_ppm(tmp)
        white, black = _CURSOR_WHITE, _CURSOR_BLACK
        tol = CURSOR_TOL
        # Probe order matters for speed: the outline's vertical 16px black
        # edge at x=0 rejects nearly every candidate immediately.
        for y in range(0, h - 15):
            for x in range(0, w - 11):
                ok = True
                for (px, py) in black:
                    o = ((y + py) * w + (x + px)) * 3
                    r, g, b = raster[o], raster[o + 1], raster[o + 2]
                    if r > tol or g > tol or b > tol:
                        ok = False
                        break
                if not ok:
                    continue
                for (px, py) in white:
                    o = ((y + py) * w + (x + px)) * 3
                    r, g, b = raster[o], raster[o + 1], raster[o + 2]
                    if r < 255 - tol or g < 255 - tol or b < 255 - tol:
                        ok = False
                        break
                if ok:
                    return (x, y)
        return None
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def _rel_move_steps(monitor, dx, dy):
    """One relative displacement, split into REL_STEP_PX HMP commands."""
    s = _connect(monitor)
    try:
        while dx or dy:
            sx = max(-REL_STEP_PX, min(REL_STEP_PX, dx))
            sy = max(-REL_STEP_PX, min(REL_STEP_PX, dy))
            _mon_send(s, "mouse_move %d %d" % (sx, sy), settle=REL_STEP_SEC)
            dx -= sx
            dy -= sy
    finally:
        s.close()


def _rel_home(monitor):
    """Drive the cursor to (0, 0): the guest's PS/2 handler clamps motion to
    the screen, so repeated negative steps pin it at the origin regardless of
    where it started.  Verified there rather than trusted: sprite-locate
    afterwards, and only believe (0, 0) if the sprite is seen there."""
    _rel_move_steps(monitor, -2000, 0)
    _rel_move_steps(monitor, 0, -2000)
    time.sleep(0.3)
    pos = _cursor_locate(monitor)
    return pos if pos == (0, 0) else None


def _rel_goto(a, tx, ty):
    """Move the PS/2 pointer to absolute guest coordinates (tx, ty).
    Returns True when the sprite is verified at the target afterwards."""
    if _ps2_select(a.monitor) is None:
        raise SystemExit("gui_input: no PS/2 mouse in `info mice` -- "
                         "cannot use the relative pointer path")
    cur = _cursor_locate(a.monitor)
    if cur is None:
        cur = _rel_home(a.monitor)
        if cur is None:
            raise SystemExit("gui_input: cannot locate the guest cursor "
                             "(screendump/template match failed)")
    (cx, cy) = cur
    _rel_move_steps(a.monitor, tx - cx, ty - cy)
    time.sleep(0.25)
    got = _cursor_locate(a.monitor)
    return got == (tx, ty)


def _use_rel_pointer(a):
    """Prefer the PS/2 relative path on QEMU >= 9 (input-send-event aborts
    the emulator there); keep the proven QMP abs path on older QEMU.  The
    relative path needs the HMP monitor; without it there is no choice."""
    major = _qemu_major(a)
    if getattr(a, "monitor", None):
        if major is None or major >= 9:
            return True
    return False



def cmd_click(a):
    """Move the (absolute) pointer to (x, y) and click there.

    QMP path (QEMU < 9): the move is sent as its own event batch before the
    button: a tablet reports position and buttons together, and a guest that
    tracks motion (the compositor moves its cursor on every report) must see
    the pointer arrive before it is told the button went down.

    PS/2 path (QEMU >= 9, or --pointer rel): mouse_set to the emulated PS/2
    mouse, relative stepwise motion with sprite-verified landing, then
    mouse_button 1/0 over HMP.
    """
    if getattr(a, "pointer", "auto") == "rel" or \
       (a.pointer == "auto" and _use_rel_pointer(a)):
        if not _rel_goto(a, a.x, a.y):
            sys.stderr.write("gui_input: click: cursor did not land on "
                             "(%d,%d)\n" % (a.x, a.y))
            return 1
        s = _connect(a.monitor)
        try:
            _mon_send(s, "mouse_button 1", settle=0.08)
            _mon_send(s, "mouse_button 0", settle=0.15)
        finally:
            s.close()
        print("click %d,%d (ps/2)" % (a.x, a.y))
        return 0
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
    if getattr(a, "pointer", "auto") == "rel" or \
       (a.pointer == "auto" and _use_rel_pointer(a)):
        if not _rel_goto(a, a.x, a.y):
            sys.stderr.write("gui_input: dblclick: cursor did not land on "
                             "(%d,%d)\n" % (a.x, a.y))
            return 1
        s = _connect(a.monitor)
        try:
            # Raw-fire the four button commands with tiny gaps: each HMP
            # round-trip costs >600 ms in banner+settle overhead, and the
            # guest's double-click window is 40 ticks ~= 400 ms -- four
            # _mon_send() calls put the second press ~800 ms after the
            # first, which the compositor correctly read as two single
            # clicks.  Raw sends land the second press ~150 ms in.
            _drain(s, 0.2, strip_spam=False)
            s.sendall(b"mouse_button 1\n"); time.sleep(0.05)
            s.sendall(b"mouse_button 0\n"); time.sleep(0.10)
            s.sendall(b"mouse_button 1\n"); time.sleep(0.05)
            s.sendall(b"mouse_button 0\n"); time.sleep(0.25)
            _drain(s, 0.3, strip_spam=False)
        finally:
            s.close()
        print("dblclick %d,%d (ps/2)" % (a.x, a.y))
        return 0
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
    if getattr(a, "pointer", "auto") == "rel" or \
       (a.pointer == "auto" and _use_rel_pointer(a)):
        if not _rel_goto(a, a.x, a.y):
            sys.stderr.write("gui_input: move: cursor did not land on "
                             "(%d,%d)\n" % (a.x, a.y))
            return 1
        print("move %d,%d (ps/2)" % (a.x, a.y))
        return 0
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
        c.add_argument("--qmp", default=None)
        c.add_argument("--monitor", default=None)   # required for the ps/2 path
        c.add_argument("x", type=int)
        c.add_argument("y", type=int)
        c.add_argument("--width", type=int, default=1280)
        c.add_argument("--height", type=int, default=800)
        c.add_argument("--device", default="gltablet")
        c.add_argument("--pointer", choices=["auto", "abs", "rel"],
                       default="auto",
                       help="abs: QMP input-send-event (QEMU < 9); rel: HMP "
                            "PS/2 mouse (portable, QEMU >= 9-safe); "
                            "auto: pick by QEMU version (needs --monitor)")
        c.set_defaults(fn=fn)

    a = p.parse_args(argv)

    # The pointer verbs need at least one socket: QMP for the abs path
    # (QEMU < 9), the HMP monitor for the ps/2 relative path.
    if a.cmd in ("click", "dblclick", "move"):
        want_abs = (a.pointer == "abs") or \
                   (a.pointer == "auto" and not a.monitor) or \
                   (a.pointer == "auto" and a.monitor and
                    (_qemu_major(a) or 8) < 9)
        if want_abs and not a.qmp:
            p.error("%s needs --qmp (abs pointer) or --monitor "
                    "(ps/2 relative pointer)" % a.cmd)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
