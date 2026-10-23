#!/usr/bin/env python3
# WR-4 host-side SSH server for the PuTTY -ssh leg (guest dials 10.0.2.2:2222).
# Paramiko-based: password auth wr4/wr4pass, canned shell. Deterministic:
# sends banner+prompt, answers "echo SSH-OK" with "SSH-OK".
# Binds 127.0.0.1. Host key is the pinned wr4_ssh_host_rsa.key next to this file.
import os
import socket
import threading
import traceback

import paramiko

HERE = os.path.dirname(os.path.abspath(__file__))
KEYF = os.path.join(HERE, "wr4_ssh_host_rsa.key")
USER = "wr4"
PW = "wr4pass"
PROMPT = b"wr4@test:$ "


class Srv(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return (
            paramiko.AUTH_SUCCESSFUL
            if (username == USER and password == PW)
            else paramiko.AUTH_FAILED
        )

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_shell_request(self, channel):
        return True

    def check_channel_pty_request(self, *args):
        return True


def handle(c):
    t = paramiko.Transport(c)
    t.add_server_key(paramiko.RSAKey.from_private_key_file(KEYF))
    try:
        t.start_server(server=Srv())
        ch = t.accept(30)
        if ch is None:
            print("wr4ssh: no channel", flush=True)
            return
        print("wr4ssh: shell open", flush=True)
        ch.send(b"WR4-SSH-READY\r\n" + PROMPT)
        buf = b""
        while True:
            d = ch.recv(1024)
            if not d:
                break
            buf += d
            while b"\r" in buf or b"\n" in buf:
                line, _, rest = buf.replace(b"\r", b"\n").partition(b"\n")
                buf = rest
                cmd = line.decode("utf-8", "replace").strip()
                print("wr4ssh: cmd %r" % cmd, flush=True)
                if cmd == "echo SSH-OK":
                    ch.send(b"SSH-OK\r\n" + PROMPT)
                elif cmd == "exit":
                    ch.send(b"bye\r\n")
                    ch.close()
                    return
                elif cmd:
                    ch.send(b"unk: " + cmd.encode()[:32] + b"\r\n" + PROMPT)
                else:
                    ch.send(PROMPT)
    except Exception:
        traceback.print_exc()
    finally:
        try:
            t.close()
        except Exception:
            pass
        print("wr4ssh: closed", flush=True)


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 2222))
srv.listen(5)
print("wr4ssh: listening on 127.0.0.1:2222", flush=True)
while True:
    c, a = srv.accept()
    print("wr4ssh: conn from %s" % (a,), flush=True)
    threading.Thread(target=handle, args=(c,), daemon=True).start()
