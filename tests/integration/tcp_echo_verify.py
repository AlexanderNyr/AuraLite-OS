#!/usr/bin/env python3
"""tcp_echo_verify.py — RESIDUE2 T5 host peer for the ordering gate.

Listens on 0.0.0.0:<port> (default 18098); QEMU/SLIRP exposes the host
as 10.0.2.2 to the guest.  The guest's tcpordtest uploads 512 KiB of a
position-dependent pattern; this server VERIFIES every byte against the
same pattern (any reorder/duplicate/corruption fails by offset), echoes
the stream back, and prints the wall-clock throughput receipt the
integration case greps:

    VERIFY-OK 524288 bytes (pattern intact)
    ECHO-SENT 524288 bytes
    THROUGHPUT 524288 bytes in 4.2s (124925 B/s)
"""

import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18098
TOTAL = 512 * 1024


def pat_byte(off):
    return (off * 31 + (off >> 8) + 7) & 0xFF


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(1)
print("listening on %d" % PORT, flush=True)

conn, _ = srv.accept()
conn.settimeout(120)
start = time.time()

buf = bytearray()
while len(buf) < TOTAL:
    data = conn.recv(65536)
    if not data:
        break
    buf += data

bad = None
if len(buf) != TOTAL:
    print("VERIFY-FAIL length %d != %d" % (len(buf), TOTAL), flush=True)
else:
    for i in range(TOTAL):
        if buf[i] != pat_byte(i):
            bad = i
            break
    if bad is None:
        print("VERIFY-OK %d bytes (pattern intact)" % TOTAL, flush=True)
    else:
        print("VERIFY-FAIL at offset %d" % bad, flush=True)

if bad is None and len(buf) == TOTAL:
    conn.sendall(bytes(buf))
    print("ECHO-SENT %d bytes" % TOTAL, flush=True)
    elapsed = time.time() - start
    print("THROUGHPUT %d bytes in %.1fs (%.0f B/s)"
          % (TOTAL, elapsed, TOTAL / max(elapsed, 0.001)), flush=True)

conn.close()
srv.close()
