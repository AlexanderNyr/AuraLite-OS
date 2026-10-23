#!/usr/bin/env python3
# WR-4 host-side echo server for the PuTTY -raw leg (guest dials 10.0.2.2:2323).
# Binds 127.0.0.1 (guest SLIRP forwards there; never 0.0.0.0: sandbox probes).
import socket
import threading

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 2323))
srv.listen(5)
print("wr4echo: listening on 127.0.0.1:2323", flush=True)


def handle(c, a):
    print("wr4echo: conn from %s" % (a,), flush=True)
    try:
        c.sendall(b"WR4-ECHO-READY\r\n")
        while True:
            d = c.recv(1024)
            if not d:
                break
            print("wr4echo: got %r" % d, flush=True)
            c.sendall(b"ECHO:" + d)
    except OSError as e:
        print("wr4echo: err %s" % e, flush=True)
    finally:
        c.close()
        print("wr4echo: closed %s" % (a,), flush=True)


while True:
    c, a = srv.accept()
    threading.Thread(target=handle, args=(c, a), daemon=True).start()
