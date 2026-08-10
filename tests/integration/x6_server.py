#!/usr/bin/env python3
"""x6_server.py — host-side wire gate for the X6 HTTP completeness phase.

QEMU/SLIRP forwards the guest's connections to 10.0.2.2:<port> here.  Two
listeners run in parallel:

  * HTTP keep-alive server on :<http_port>.  Every accepted socket and
    every parsed request is counted.  Serves /first /second /echo (POST
    echo) and /redirect (301 → https://<guest_ip>:<tls_port>/).
  * TLS marker server on :<tls_port> with the cert/key given on the
    command line (the case generates a fresh self-signed Ed25519 one).
    Answers every request with a page containing [X6_HTTPS_MARKER].

Usage: x6_server.py <guest_ip> <http_port> <tls_port> <cert> <key> <stats>

The counters are rewritten to <stats> after every event, so the case can
prove *on the wire* that the guest's four request steps travelled over
exactly one HTTP connection (connections=1 requests=4), independently of
whatever the guest logged about reuse.
"""

import socket
import ssl
import sys
import threading

GUEST_IP = sys.argv[1]
HTTP_PORT = int(sys.argv[2])
TLS_PORT = int(sys.argv[3])
CERT = sys.argv[4]
KEY = sys.argv[5]
STATS = sys.argv[6]

MARKER = "[X6_HTTPS_MARKER]"

lock = threading.Lock()
stats = {"connections": 0, "requests": 0,
         "https_connections": 0, "https_requests": 0}


def bump(key):
    with lock:
        stats[key] += 1
        line = ("connections=%d requests=%d https_connections=%d "
                "https_requests=%d\n" % (stats["connections"], stats["requests"],
                                         stats["https_connections"],
                                         stats["https_requests"]))
        with open(STATS, "w") as f:
            f.write(line)


def read_request(conn, buf):
    """Return (buf, method, path, headers, body) or (buf, None...) on EOF."""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return buf, None, None, None, None
        buf += chunk
        if len(buf) > 65536:
            return buf, None, None, None, None
    head, buf = buf.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    try:
        method, path, _ver = lines[0].decode("latin1").split(" ", 2)
    except ValueError:
        return buf, None, None, None, None
    headers = {}
    for ln in lines[1:]:
        if b":" in ln:
            k, v = ln.split(b":", 1)
            headers[k.decode("latin1").strip().lower()] = \
                v.decode("latin1").strip()
    body = b""
    cl = int(headers.get("content-length", "0") or "0")
    while len(body) < cl:
        if buf:
            take = min(cl - len(body), len(buf))
            body += buf[:take]
            buf = buf[take:]
        else:
            chunk = conn.recv(cl - len(body))
            if not chunk:
                return buf, None, None, None, None
            body += chunk
    return buf, method, path, headers, body


def respond(conn, status, body, extra=""):
    payload = body.encode("latin1") if isinstance(body, str) else body
    head = ("HTTP/1.1 %s\r\nContent-Length: %d\r\n"
            "Connection: keep-alive\r\n%s\r\n" % (status, len(payload), extra))
    conn.sendall(head.encode("latin1") + payload)


def serve_http_conn(conn, _addr):
    conn.settimeout(20)
    bump("connections")
    buf = b""
    try:
        while True:
            buf, method, path, headers, body = read_request(conn, buf)
            if method is None:
                return
            bump("requests")
            path_only = path.split("?", 1)[0]
            if path_only == "/first":
                respond(conn, "200 OK", "first-page")
            elif path_only == "/second":
                respond(conn, "200 OK", "second-page")
            elif path_only == "/echo":
                respond(conn, "200 OK", body,
                        "Content-Type: application/octet-stream\r\n")
            elif path_only == "/redirect":
                respond(conn, "301 Moved Permanently", "",
                        "Location: https://%s:%d/\r\n" % (GUEST_IP, TLS_PORT))
            else:
                respond(conn, "404 Not Found", "nf")
            if headers.get("connection", "").lower() == "close":
                return
    except (ConnectionError, socket.timeout, OSError):
        return
    finally:
        try:
            conn.close()
        except OSError:
            pass


def http_listener():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", HTTP_PORT))
    srv.listen(8)
    print("[x6srv] http on :%d" % HTTP_PORT, flush=True)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=serve_http_conn, args=(conn, addr),
                         daemon=True).start()


def tls_listener():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    ctx.set_alpn_protocols(["http/1.1"])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", TLS_PORT))
    srv.listen(8)
    print("[x6srv] tls on :%d" % TLS_PORT, flush=True)
    while True:
        raw, addr = srv.accept()
        threading.Thread(target=serve_tls_conn, args=(ctx, raw), daemon=True
                         ).start()


def serve_tls_conn(ctx, raw):
    bump("https_connections")
    raw.settimeout(20)
    try:
        conn = ctx.wrap_socket(raw, server_side=True)
    except (ssl.SSLError, OSError):
        raw.close()
        return
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                conn.close()
                return
            buf += chunk
            if len(buf) > 65536:
                conn.close()
                return
        bump("https_requests")
        page = ("<!doctype html><title>X6</title>"
                "<body>%s https-side of the redirect</body>" % MARKER)
        respond(conn, "200 OK", page)
    except (ssl.SSLError, ConnectionError, socket.timeout, OSError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


threading.Thread(target=http_listener, daemon=True).start()
threading.Thread(target=tls_listener, daemon=True).start()
print("[x6srv] ready http=%d tls=%d" % (HTTP_PORT, TLS_PORT), flush=True)
threading.Event().wait()
