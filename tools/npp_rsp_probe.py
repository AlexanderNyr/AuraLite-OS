#!/usr/bin/env python3
# npp_rsp_probe.py — WR-3 guest debugger for the Notepad++ bring-up (D-WR3).
#
# Boots the lab QEMU twice-paused (`-S -s`, see the wr3lab/wr3gdb stand in
# the WR-3 receipts), attaches to the QEMU gdbstub with RAW RSP (no gdb
# binary needed), sets a hardware breakpoint (Z1) on the user-mode faulting
# VA inside the guest process, and on the hit dumps registers, a linear scan
# of the guest stack for return addresses inside the NPP image, and the CRT
# globals the crash tripped over.  QEMU system-mode gdbstub reads are
# translated through the *stopped thread's* CR3, so plain guest VAs work;
# the hit thread must be selected first (Hgp<pid>.<tid>) or reads E14.
# This is how the step2 blocker was isolated to the .CRT table walk order.
#
# Paths assume the /tmp/wr3gdb lab stand; adapt LAB/SER/FAULT1 for a new
# investigation.  Keep out of CI: interactive by construction.
import socket, struct, subprocess, time, re, sys, os

LAB = "/tmp/wr3gdb"
SER = LAB + "/full_serial.log"
NPP = 0x400000000000
FAULT1 = NPP + 0x402d19
CR3 = None

def pkt(data: bytes) -> bytes:
    c = sum(data) & 0xff
    return b"$" + data + b"#" + ("%02x" % c).encode()

def recv_until(sock, timeout=6.0):
    sock.settimeout(timeout)
    buf = b""
    while True:
        try:
            b = sock.recv(4096)
        except socket.timeout:
            return buf
        if not b:
            return buf
        buf += b
        while b"$" in buf or b"#" in buf:
            if buf.startswith(b"+"):
                buf = buf[1:]; continue
            if buf.startswith(b"-"):
                return b""
            try:
                i = buf.index(b"#"); j = i + 2
            except ValueError:
                break
            if len(buf) >= j:
                return buf[1:i]

class Rsp:
    def __init__(self, port=1234):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.log = open("/tmp/wr3gdb/rsp.log", "wb", buffering=0)

    def cmd(self, data, timeout=6.0):
        self.s.sendall(pkt(data))
        r = recv_until(self.s, timeout)
        self.log.write(b"> " + data + b"\n< " + r + b"\n")
        return r

    def readmem(self, addr, n):
        out = b""
        while len(out) < n:
            chunk = min(0x400, n - len(out))
            r = self.cmd(b"m%x,%x" % (addr + len(out), chunk))
            if not r or r.startswith(b"E"):
                return None
            out += bytes.fromhex(r.decode())
        return out

    def mem_u64(self, addr):
        b = self.readmem(addr, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def continue_(self):
        self.s.sendall(pkt(b"vCont;c"))

    def poll_stop(self, timeout):
        self.s.settimeout(timeout)
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.s.settimeout(4.0)
            try:
                b = self.s.recv(4096)
            except socket.timeout:
                continue
            if not b:
                return None
            buf += b
            while b"$" in buf:
                i = buf.find(b"$")
                if buf[i:].startswith(b"$") and b"#" in buf[i:]:
                    j = buf.index(b"#", i)
                    body = buf[i+1:j]
                    buf = buf[:i] + buf[j+2:]
                    if body.startswith(b"S") or body.startswith(b"T"):
                        return body
                else:
                    break
        return None

    def stop(self):
        self.s.sendall(b"\x03"); time.sleep(0.6)
        return self.poll_stop(8)

def walk(rd, va):
    idx = [(va >> 39) & 0x1FF, (va >> 30) & 0x1FF, (va >> 21) & 0x1FF,
           (va >> 12) & 0x1FF]
    tbl = CR3 & 0x000FFFFFFFFFF000
    for i in range(4):
        b = rd.readmem(tbl + 8 * idx[i], 8)
        if not b: return None
        e = struct.unpack("<Q", b)[0]
        if not (e & 1): return None
        if i < 3 and (e & 0x80):
            if i == 1: return (e & 0xFE000000000) | (va & 0x3FFFFFFF)
            return (e & 0x000FFFFFFFE00000) | (va & 0x1FFFFF)
        tbl = e & 0x000FFFFFFFFFF000
    return tbl | (va & 0xFFF)

def main():
    env = dict(os.environ)
    qemu = subprocess.Popen(
        ["bash", "-c",
         "cd ${WR3_HOME:-/home/user} && ./wr3gdb.sh boot"], stdout=open("/tmp/wr3gdb/boot.log","w"),
        stderr=subprocess.STDOUT)
    print("waiting for QEMU gdb port...")
    r = None
    t0 = time.time()
    while time.time() - t0 < 60:
        try:
            r = Rsp(); break
        except OSError:
            time.sleep(1)
    if not r:
        print("no gdbstub"); sys.exit(1)
    r.cmd(b"+qSupported:multiprocess+;xmlRegisters=i386;qRelocInfo+#00"[:len(b"+qSupported:multiprocess+")])
    r.cmd(b"qSupported:multiprocess+")
    print("attached; releasing CPU")
    r.s.sendall(b"+"); r.continue_()
    # wait for shell prompt in serial log, then launch NPP
    t0 = time.time()
    while time.time() - t0 < 400:
        try:
            if b"auralite#" in open(SER, "rb").read():
                break
        except FileNotFoundError:
            pass
        time.sleep(1)
    else:
        print("no shell"); sys.exit(1)
    print("shell up; setting bp + launching")
    r.stop()
    print("Z1:", r.cmd(b"Z1,%x,1" % FAULT1))
    r.continue_()
    subprocess.run(["bash", "-c", "cd ${WR3_HOME:-/home/user} && ./wr3gdb.sh send 'w32run /fat/notepad++.exe &' >/dev/null 2>&1"], timeout=120)
    # NPP reaches the fault a few seconds after bind; poll for stop via the ack we get.
    body = r.poll_stop(240)
    print("stop:", body)

    if not body or not (body.startswith(b"T") or body.startswith(b"S")):
        print("no stop; serial tail:"); os.system("grep -aE 'bound|PF|Done' %s | tail -5" % SER); sys.exit(1)
    regs = r.cmd(b"g")
    if regs and regs != b"":
        hx = regs.decode()
        import struct as _st
        v = [_st.unpack("<Q", bytes.fromhex(hx[i*16:(i+1)*16]))[0] for i in range(27)]
        names = ["rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15","rip","eflags"]
        for i, n in enumerate(names):
            print("  %s=%x" % (n, v[i]))
        rsp = v[7]; rip = v[16]
    else:
        print("no g response"); sys.exit(1)
    print("--- stack scan VA directly (npp range = npp+offset) ---")
    for off in range(0, 0x1800, 8):
        b = r.readmem(rsp + off, 8)
        if not b:
            print("  read fail at rsp+0x%x" % off); break
        val = struct.unpack("<Q", b)[0]
        if NPP + 0x1000 < val < NPP + 0x800000:
            print("  rsp+0x%04x -> npp+0x%x" % (off, val - NPP))
    b = r.readmem(NPP + 0x54dda8, 16)
    if b:
        c, p = struct.unpack("<QQ", b)
        print("  count[dda8]=%x tableptr[ddb0]=%x" % (c, p))
    cs = r.readmem(NPP + 0x54de00, 8 * 8)
    if cs:
        print("  cs array:", [hex(struct.unpack("<Q", cs[i*8:i*8+8])[0]) for i in range(8)])
    code = r.readmem(NPP + 0x402cf4, 0x30)
    print("  reader prologue:", code.hex() if code else "FAIL")
    for lbl, o in (("__acrt_heap[54e930]", 0x54e930), ("IAT HeapAlloc[42f8f8]", 0x42f8f8),
                   ("IAT EnterCritSec[42f678]", 0x42f678), ("IAT[42f680]", 0x42f680),
                   ("IAT[42f688]", 0x42f688), ("IAT[42f690]", 0x42f690)):
        b = r.readmem(NPP + o, 8)
        print("  %s = %s" % (lbl, "%x" % struct.unpack("<Q", b)[0] if b else "UNMAPPED"))
    b = r.readmem(NPP + 0x3faa9c, 0x24)
    print("  0x3faa9c bytes:", b.hex() if b else "FAIL")
    r.cmd(b"z1,%x,1" % FAULT1)
    try: r.s.sendall(b"vKill;#00"[:7])
    except Exception: pass
    print("RSP_DONE")

main()
