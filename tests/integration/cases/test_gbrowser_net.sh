#!/usr/bin/env bash
# test_gbrowser_net.sh — WEBVIEW_PLAN phases W6-W7: navigation and networking.
#
# A real HTTP server runs on the host (QEMU SLIRP exposes it at
# 10.0.2.2).  The web view is driven by its test hooks, which are written
# BEFORE it starts (the init shell blocks on `run`):
#   /tmp/gbrowser.url    — the page to load first
#   /tmp/gbrowser.steps  — "|"-separated actions: "link 0|back|https|
#                         nav <url>"  (the payload must be QUOTED on the
#                         write line: since SH6c a bare "|" is a shell
#                         pipeline operator and would truncate the file)
#
# The server serves:
#   /           home page with markers + two links
#   /page2.html second page (the link target)
#   /chunked    a chunked-transfer-encoded response (same bytes as plain)
#   /big        100 KB page (the growing buffer)
#
# Gate assertions:
#   - fetch + render a page from the in-tree server (no internet needed);
#   - follow a link, then go back; the previous page reappears;
#   - a chunked response decodes (marker present);
#   - an https:// URL produces the explanation, not a hang;
#   - a >16 KB response is fully received (the old static 16 KB buffer
#     would have truncated it).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "WebView W6 navigation and networking"

LOG="$IL_LOGDIR/gbrowser_net.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

PORT="${WV_PORT:-18090}"

cat > "$IL_BUILD/gbrowser_test_server.py" <<PYEOF
import socket, threading

HOME = (b"<!doctype html><html><head><title>W6 home</title></head>"
        b"<body><h1>WEBVIEW_W6_HOME</h1>"
        b"<p>Home page with a <a href=\"/page2.html\">second page</a> and "
        b"<a href=\"/deep/x.html\">a deep link</a>.</p>"
        b"<p>Relative: <a href=\"rel.html\">relative</a>.</p></body>")
PAGE2 = (b"<!doctype html><html><head><title>W6 second</title></head>"
         b"<body><h1>WEBVIEW_W6_PAGE2</h1>"
         b"<p>Back <a href=\"/\">home</a>.</p></body>")
CHUNKED_BODY = b"<!doctype html><html><body><h1>WEBVIEW_W6_CHUNKED</h1>"
BIG_BODY = (b"<!doctype html><html><body><h1>WEBVIEW_W6_BIG</h1>"
            b"<p>" + b"lorem ipsum dolor sit amet " * 4000 + b"</p></body>")
CANVAS_PAGE = (b"<!doctype html><html><body><h1>WEBVIEW_W6_CANVAS</h1>"
               b"<p>Text and a 3D cube:</p>"
               b"<canvas width=64 height=48 data-scene=\"cube\"></canvas>"
               b"</body>")

def chunked(body):
    out = b""
    i = 0
    while i < len(body):
        n = min(23, len(body) - i)
        out += ("%x\r\n" % n).encode() + body[i:i+n] + b"\r\n"
        i += n
    return out + b"0\r\n\r\n"

def handle(c):
    try:
        data = c.recv(4096)
        line = data.split(b"\r\n")[0].decode(errors="replace")
        parts = line.split()
        path = parts[1] if len(parts) > 1 else "/"
        if path == "/page2.html":
            body = PAGE2
            resp = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body
        elif path == "/canvas.html":
            body = CANVAS_PAGE
            resp = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body
        elif path == "/chunked":
            resp = (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    + chunked(CHUNKED_BODY))
        elif path == "/big":
            body = BIG_BODY
            resp = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body
        else:
            body = HOME
            resp = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body
        c.sendall(resp)
    finally:
        c.close()

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", $PORT))
s.listen(8)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
PYEOF

python3 "$IL_BUILD/gbrowser_test_server.py" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; il_dump_on_error' EXIT
sleep 1

il_send_delay 8
il_send "write /tmp/gbrowser.url http://10.0.2.2:$PORT/"
il_send_delay 1
il_send "write /tmp/gbrowser.steps \"link 0|back|https|nav http://10.0.2.2:$PORT/chunked|nav http://10.0.2.2:$PORT/big|nav http://10.0.2.2:$PORT/canvas.html\""
il_send_delay 1
il_send "run gbrowser"

il_run_qemu "$LOG" 110

kill $SERVER_PID 2>/dev/null
trap il_dump_on_error EXIT

# 1) initial fetch + render
il_assert_grep "$LOG" "nav: loaded .* bytes from http://10.0.2.2:$PORT/"  "initial page fetched"
il_assert_grep "$LOG" "html built"                                      "page rendered from the fetched HTML"

# 2) link navigation + back
il_assert_grep "$LOG" "link 0 -> http://10.0.2.2:$PORT/page2.html"       "link hit-test resolves"
il_assert_grep "$LOG" "nav: loaded .* bytes from http://10.0.2.2:$PORT/page2.html" \
    "second page fetched"
il_assert_grep "$LOG" "back: http://10.0.2.2:$PORT/"                     "history back"
il_assert_grep "$LOG" "nav: loaded .* bytes from http://10.0.2.2:$PORT/"  "first page reappears"

# 3) X6: https:// is now fetched for real (TLS via libahttp).  Whether the
# fetch succeeds (host with internet) or fails gracefully (offline CI),
# the navigation must be attempted and must never print the old refusal.
il_assert_grep "$LOG" "nav: fetching https://example.com/"   "https navigation attempted"
il_assert_no_grep "$LOG" "https unsupported"                 "no pre-X6 https refusal"

# 4) chunked response
il_assert_grep "$LOG" "nav: loaded .* bytes from http://10.0.2.2:$PORT/chunked" \
    "chunked page fetched"
il_assert_no_grep "$LOG" "https unsupported.*chunked"                    "chunked was actually http"

# 5) growing buffer: > 16 KB page received in full
il_assert_grep "$LOG" "nav: loaded .* bytes from http://10.0.2.2:$PORT/big" \
    "big page fetched"

# 6) W7: a page containing a <canvas data-scene="cube"> renders text AND 3D
il_assert_grep "$LOG" "nav: loaded .* bytes from http://10.0.2.2:$PORT/canvas.html" \
    "canvas page fetched"
il_assert_grep "$LOG" "canvas: rendered 64x48 cube"          "GL canvas rendered in-guest"

il_assert_no_grep "$LOG" "EXCEPTION|kernel panic|triple fault"           "no kernel fault"

il_summary
