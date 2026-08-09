#!/usr/bin/env python3
"""x5_slow_server.py — host-side slow-drain TCP sink for the X5 wire gate.

Listens on 0.0.0.0:<port> (default 18099).  QEMU/SLIRP forwards the guest's
connections to 10.0.2.2:<port> here.  Reads only SLOW_STEP bytes every
SLOW_DELAY seconds, so SLIRP's proxied receive window toward the guest
releases piecemeal — the guest's TCP sender must park in its window-full
wait and consume a long run of small ACKs, exactly the X5 test gate.

Once EXPECTED bytes have been received the server stays for the remaining
FIN bookkeeping, then answers "OK <n>\n".
"""

import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18099
EXPECTED = int(sys.argv[2]) if len(sys.argv) > 2 else 1024 * 1024
SLOW_STEP = 1024
SLOW_DELAY = 0.01

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(1)
sys.stdout.write(f"[x5srv] listening on :{PORT} expecting {EXPECTED} bytes\n")
sys.stdout.flush()

conn, peer = srv.accept()
conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
sys.stdout.write(f"[x5srv] connection from {peer}\n")
sys.stdout.flush()

total = 0
conn.settimeout(30.0)
try:
    while total < EXPECTED:
        data = conn.recv(SLOW_STEP)
        if not data:
            break
        total += len(data)
        time.sleep(SLOW_DELAY)
except socket.timeout:
    sys.stdout.write(f"[x5srv] TIMEOUT after {total} bytes\n")
    sys.stdout.flush()
finally:
    try:
        conn.sendall(f"OK {total}\n".encode())
    except OSError:
        pass
    conn.close()
    srv.close()

sys.stdout.write(f"[x5srv] done: {total}/{EXPECTED} bytes\n")
sys.stdout.flush()
sys.exit(0 if total == EXPECTED else 1)
