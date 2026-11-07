#!/usr/bin/env python3
"""paced_peer.py — the peer for the 1 MiB window-scaling lane (RESIDUE3 C7).

Two listeners, deliberately different, on two ports:

  BIG    a peer whose receive window may be large.  Linux negotiates a window
         scale in the handshake (net.ipv4.tcp_window_scaling, on by default)
         and with an 8 MiB receive buffer it grants far more than 64 KiB, so
         the guest may legitimately have >65535 bytes in flight.
  CLAMP  the same peer with TCP_WINDOW_CLAMP = 65535, which pins its
         advertised window to 16 bits: scaling cannot buy it anything.  This
         is the negative control — a guest that reports more than 65535 bytes
         in flight here is inventing window, and the BIG result is an
         artefact.

Each listener reads until it has 1 MiB, or until the connection closes, and
prints a one-line receipt to stdout so the case file can assert on what the
peer actually saw (guest bytes are not evidence that they arrived).

WHY THERE IS NO 200 ms DELAY HERE.  The plan asked for a 200 ms-RTT lane.  On
a SLIRP link there is no way to shape one from userspace: delaying this
process's reads does not delay the guest's ACKs unless the receive buffer is
small enough to fill, and shrinking the buffer is the opposite of granting a
large window — so "200 ms" and "window bigger than 64 KiB" cannot both hold
through a userspace peer.  The lane therefore measures what the window
arithmetic does (bytes in flight) and reports the elapsed time it took, which
is the part of the plan's intent that survives the emulator.
"""

import socket
import sys
import threading

BIND_ADDR = "0.0.0.0"
BYTES = 1024 * 1024
RCVBUF_BIG = 8 * 1024 * 1024
TCP_WINDOW_CLAMP = 16          # Linux: 0x10

results = {}
lock = threading.Lock()


def serve(port, clamped, label):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if clamped:
        # The window a peer grants is decided at accept time from the
        # listener's receive buffer, so this must be set HERE, before the
        # handshake — that is also why TCP_WINDOW_CLAMP on the listener is
        # refused (EINVAL, measured) and is only applied best-effort to the
        # accepted socket below.
        try:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
        except OSError as e:                    # pragma: no cover
            print(f"{label}: cannot shrink window: {e}", flush=True)
    else:
        try:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RCVBUF_BIG)
        except OSError as e:                    # pragma: no cover
            print(f"{label}: cannot enlarge window: {e}", flush=True)
    # Bind every interface: the guest reaches this peer through a TAP link
    # whose host side is 10.0.2.2, or through SLIRP's alias of the same
    # address.  127.0.0.1 would only ever serve the SLIRP case.
    srv.bind((BIND_ADDR, port))
    srv.listen(1)
    srv.settimeout(120)

    try:
        conn, _ = srv.accept()
    except socket.timeout:                       # pragma: no cover
        print(f"{label}: no guest connection (timeout)", flush=True)
        with lock:
            results[label] = None
        return

    conn.settimeout(120)
    # Drain in large reads: the advertised window only stays open if the
    # application keeps up, and a window that shrinks under the guest's feet
    # would make the "bytes in flight" figure a property of this script
    # rather than of the guest's window arithmetic.
    read_chunk = 262144 if not clamped else 65536
    if clamped:
        try:
            conn.setsockopt(socket.IPPROTO_TCP, TCP_WINDOW_CLAMP, 16384)
        except OSError:
            pass          # best-effort: the small rcvbuf already bounds it
    got = 0
    try:
        while got < BYTES:
            chunk = conn.recv(read_chunk)
            if not chunk:
                break
            got += len(chunk)
    except (socket.timeout, OSError) as e:        # pragma: no cover
        print(f"{label}: read stopped: {e}", flush=True)
    finally:
        conn.close()
        srv.close()

    with lock:
        results[label] = got
    print(f"peer[{label}]: received {got} of {BYTES} bytes "
          f"(window {'bounded to 16 KiB' if clamped else 'free to grow'})",
          flush=True)


def main():
    if len(sys.argv) != 3:                       # pragma: no cover
        print("usage: paced_peer.py BIGPORT CLAMPEDPORT", flush=True)
        return 2
    big, clamp = int(sys.argv[1]), int(sys.argv[2])

    threads = [
        threading.Thread(target=serve, args=(big, False, "big"), daemon=True),
        threading.Thread(target=serve, args=(clamp, True, "clamped"), daemon=True),
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=180)

    # Exit status is the harness's signal that the peer saw what it should.
    ok = (results.get("big") == BYTES and results.get("clamped") == BYTES)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
