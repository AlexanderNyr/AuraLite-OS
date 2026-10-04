#!/usr/bin/env python3
"""serial_tee.py — proxy + logger for the guest serial unix socket.

The WR-2 live lab talks to the QEMU serial socket on demand, but the socket
accepts a single connection: a plain tee would either starve the tools or
lose whatever the guest prints between two invocations (exactly the window
where the /tmp/w32dump diagnostic prints its window tree + message ring).

So this is a relay:

    guest serial socket (upstream, e.g. /tmp/wr2lab/ser.sock)
        ^
        |  one persistent connection, everything logged to --log
        v
    proxy listen socket (downstream, e.g. /tmp/wr2lab/ser2.sock)
        ^
        |  gui_input.py serial-* connects here, any number, one at a time
        v

Guest->host bytes go to the log AND to the connected tool; tool->guest
bytes are forwarded upstream.  Usage:

    python3 tools/serial_tee.py --up /tmp/wr2lab/ser.sock \
                                --down /tmp/wr2lab/ser2.sock \
                                --log /tmp/wr2lab/tee.log

SIGTERM stops it; the downstream socket is unlinked on exit.
"""
import argparse
import os
import socket
import sys
import threading


def main():
    ap = argparse.ArgumentParser(description="serial unix-socket proxy+logger")
    ap.add_argument("--up", required=True, help="upstream (guest) socket path")
    ap.add_argument("--down", required=True, help="downstream listen path")
    ap.add_argument("--log", required=True, help="append-everything log path")
    a = ap.parse_args()

    try:
        os.unlink(a.down)
    except FileNotFoundError:
        pass

    up = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            up.connect(a.up)
            break
        except OSError:
            sys.stderr.write("serial_tee: upstream not ready, retrying\n")
            import time
            time.sleep(1)

    down_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    down_srv.bind(a.down)
    down_srv.listen(4)

    log = open(a.log, "ab", buffering=0)
    client = {}                      # at most one, guarded by the lock
    lock = threading.Lock()

    def pump_up_to_down():
        while True:
            try:
                data = up.recv(65536)
            except OSError:
                data = b""
            if not data:
                log.write(b"\n[serial_tee] upstream closed\n")
                os._exit(0)
            log.write(data)
            with lock:
                c = client.get("c")
                if c:
                    try:
                        c.sendall(data)
                    except OSError:
                        pass

    threading.Thread(target=pump_up_to_down, daemon=True).start()

    while True:
        c, _ = down_srv.accept()
        with lock:
            old = client.pop("c", None)
            if old:
                try:
                    old.close()
                except OSError:
                    pass
            client["c"] = c

        def pump_down_to_up(conn):
            try:
                while True:
                    data = conn.recv(65536)
                    if not data:
                        break
                    up.sendall(data)
            except OSError:
                pass
            finally:
                with lock:
                    if client.get("c") is conn:
                        client.pop("c", None)
                try:
                    conn.close()
                except OSError:
                    pass

        threading.Thread(target=pump_down_to_up, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
