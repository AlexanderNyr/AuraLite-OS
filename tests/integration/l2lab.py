#!/usr/bin/env python3
"""tests/integration/l2lab.py -- RESIDUE2 T5: an L2 lab on a QEMU mcast
socket netdev, for tests/integration/cases/test_e1000_idle_drain.sh.

The QEMU command line pairs -device e1000 with
`-netdev socket,id=net0,mcast=230.10.10.10:4711`, so every frame the
guest transmits reaches this script (a member of the same group) and
everything this script sends is delivered to the guest NIC as raw
Ethernet.  That gives a deterministic L2 environment with no SLIRP in
the middle -- exactly what the idle-drain gate needs:

  1. mini-DHCP:  answer the guest's DISCOVER/REQUEST with OFFER/ACK for
     10.9.9.15 (the kernel's own DHCP parser accepts it -- no SLIRP
     involved), so the guest OWNS an address and unsolicited traffic
     has something to aim at.
  2. ARP probe:  a fake host asks "who-has 10.9.9.15" while the guest
     is IDLE (no syscall owns the NIC).  Before T5 the kernel never
     replied to inbound ARP at all; now the idle drain's passive input
     must answer.  The probe runs twice -- once before and once after
     the junk blast, proving the drain keeps serving through load.
  3. junk blast: 300 broadcast frames with an unclaimed ethertype
     (0x8899) at ~25 ms spacing.  The idle drain must consume them
     silently: the queue never clogs, so no "[e1000] RX overrun" line
     may appear in the guest log.

Receipts land in the receipt file (one "KEY value" per line); the case
greps them.  Frames this script itself multicasts loop back to the
socket (IP_MULTICAST_LOOP is deliberately left ON so the co-located
QEMU also receives them), so the sniffer filters by source MAC.

Usage: l2lab.py <idledrain|udpblock> <receipt-file>
"""

import socket
import struct
import sys
import time

MCAST_GROUP = "230.10.10.10"
MCAST_PORT = 4711
SERVER_MAC = bytes.fromhex("52aa00000001")   # the fake DHCP server
PROBE_MAC = bytes.fromhex("52aa00000099")    # the fake ARP prober
SERVER_IP = "10.9.9.1"
GUEST_IP = "10.9.9.15"
PROBE_IP = "10.9.9.99"
JUNK_FRAMES = 300
JUNK_GAP_S = 0.025
ETH_ARP = 0x0806
ETH_IPV4 = 0x0800
ETH_JUNK = 0x8899


def ip_checksum(data):
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def eth(dst, src, et, payload):
    frame = dst + src + struct.pack("!H", et) + payload
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return frame


def option(code, data):
    return bytes([code, len(data)]) + data


def dhcp_reply(op_type, xid, chaddr, yiaddr):
    opts = b""
    opts += option(53, bytes([op_type]))                    # msg type
    opts += option(1, socket.inet_aton("255.255.255.0"))    # subnet mask
    opts += option(3, socket.inet_aton(SERVER_IP))          # router
    opts += option(6, socket.inet_aton(SERVER_IP))          # DNS
    opts += option(51, struct.pack("!I", 3600))             # lease
    opts += option(54, socket.inet_aton(SERVER_IP))         # server id
    opts += b"\xff"
    bootp = struct.pack("!BBBBIHH", 2, 1, 6, 0, xid, 0, 0)
    bootp += struct.pack("!I", 0)                                  # ciaddr
    bootp += socket.inet_aton(yiaddr)                              # yiaddr
    bootp += socket.inet_aton(SERVER_IP)                           # siaddr
    bootp += struct.pack("!I", 0)                                  # giaddr
    bootp += chaddr[:16].ljust(16, b"\x00")
    bootp += b"\x00" * 64 + b"\x00" * 128                          # sname, file
    bootp += struct.pack("!I", 0x63825363)                         # magic cookie
    bootp += opts
    udp = struct.pack("!HHHH", 67, 68, 8 + len(bootp), 0) + bootp
    iph = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(udp), 0x1234, 0,
                      64, 17, 0)
    iph += socket.inet_aton(SERVER_IP) + socket.inet_aton("255.255.255.255")
    iph = iph[:10] + struct.pack("!H", ip_checksum(iph)) + iph[12:]
    bcast = bytes.fromhex("ffffffffffff")
    return eth(bcast, SERVER_MAC, ETH_IPV4, iph + udp)


def arp_request(sender_mac, sender_ip, target_ip):
    pkt = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
    pkt += sender_mac + socket.inet_aton(sender_ip)
    pkt += b"\x00" * 6 + socket.inet_aton(target_ip)
    bcast = bytes.fromhex("ffffffffffff")
    return eth(bcast, sender_mac, ETH_ARP, pkt)


def parse_dhcp_request(frame):
    """Return (xid, chaddr) when the frame is the guest's DHCP REQUEST."""
    if len(frame) < 14 + 20 + 8 + 240:
        return None
    if struct.unpack("!H", frame[12:14])[0] != ETH_IPV4:
        return None
    if frame[14 + 9] != 17:                      # IP proto UDP
        return None
    udp = 14 + 20
    sport, dport = struct.unpack("!HH", frame[udp:udp + 4])
    if sport != 68 or dport != 67:
        return None
    bootp = frame[udp + 8:]
    if bootp[0] != 1:                            # BOOTREQUEST
        return None
    xid = struct.unpack("!I", bootp[4:8])[0]
    chaddr = bootp[28:44]
    # option 53 == 3 (DHCPREQUEST) or 1 (DHCPDISCOVER)
    i = 240
    msg_type = None
    while i < len(bootp):
        code = bootp[i]
        if code == 0:
            i += 1
            continue
        if code == 255 or i + 1 >= len(bootp):
            break
        length = bootp[i + 1]
        if code == 53 and length == 1:
            msg_type = bootp[i + 2]
        i += 2 + length
    if msg_type not in (1, 3):
        return None
    return msg_type, xid, chaddr


def parse_arp_reply(frame):
    if len(frame) < 14 + 28:
        return None
    if struct.unpack("!H", frame[12:14])[0] != ETH_ARP:
        return None
    pkt = frame[14:14 + 28]
    opcode = struct.unpack("!H", pkt[6:8])[0]
    if opcode != 2:
        return None
    sender_ip = socket.inet_ntoa(pkt[14:18])
    sender_mac = pkt[8:14]
    return sender_ip, sender_mac


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "idledrain"
    receipt_path = sys.argv[2] if len(sys.argv) > 2 else "l2lab.receipt"
    receipts = []

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((MCAST_GROUP, MCAST_PORT))
    mreq = socket.inet_aton(MCAST_GROUP) + socket.inet_aton("0.0.0.0")
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    # IP_MULTICAST_LOOP stays ON on purpose: the co-located QEMU is a host
    # member of the same group and only receives our sends through it.
    sock.settimeout(0.2)

    def send(frame):
        sock.sendto(frame, (MCAST_GROUP, MCAST_PORT))

    def log(key, value):
        receipts.append("%s %s" % (key, value))
        with open(receipt_path, "w") as fh:
            fh.write("\n".join(receipts) + "\n")

    guest_mac = run_dhcp(sock, send, log, deadline_s=60)
    if not guest_mac:
        log("DHCP", "FAILED")
        sys.exit(1)

    if mode == "udpblock":
        sys.exit(run_udpblock(sock, send, log, guest_mac) or 0)
    sys.exit(run_idledrain(sock, send, log, guest_mac) or 0)


def run_dhcp(sock, send, log, deadline_s=60):
    """mini-DHCP: answer DISCOVER with OFFER, REQUEST with ACK.  Returns
    the guest MAC (or None)."""
    guest_mac = None
    deadline = time.time() + deadline_s
    state = "discover"
    while time.time() < deadline and state != "done":
        try:
            frame, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if frame[6:12] in (SERVER_MAC, PROBE_MAC):
            continue                              # our own loopback
        parsed = parse_dhcp_request(frame)
        if not parsed:
            continue
        msg_type, xid, chaddr = parsed
        guest_mac = chaddr[:6]
        if state == "discover" and msg_type == 1:
            send(dhcp_reply(2, xid, chaddr, GUEST_IP))       # OFFER
            state = "request"
        elif state == "request" and msg_type == 3:
            send(dhcp_reply(5, xid, chaddr, GUEST_IP))       # ACK
            log("DHCP_ACK_SENT", GUEST_IP)  # receipt the case greps
            state = "done"
    return guest_mac if state == "done" else None


def arp_reply_to(frame):
    """An ARP REPLY answering a guest REQUEST for one of our addresses."""
    if len(frame) < 14 + 28:
        return None
    if struct.unpack("!H", frame[12:14])[0] != ETH_ARP:
        return None
    pkt = frame[14:14 + 28]
    if struct.unpack("!H", pkt[6:8])[0] != 1:          # REQUEST
        return None
    target_ip = socket.inet_ntoa(pkt[24:28])
    if target_ip not in (PROBE_IP, SERVER_IP):
        return None
    asker_mac = pkt[8:14]
    rp = (struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2)
          + PROBE_MAC + socket.inet_aton(target_ip)
          + asker_mac + pkt[24:28])
    return eth(asker_mac, PROBE_MAC, ETH_ARP, rp)


def run_udpblock(sock, send, log, guest_mac):
    """RESIDUE2 T5 blocking-recvfrom lab: answer the guest's ARP for the
    fake peer, learn the knock's source port, reply only after ~4 s."""
    deadline = time.time() + 60
    knock_port = None
    knock_at = None
    answered_arp = False
    while time.time() < deadline:
        try:
            frame, _ = sock.recvfrom(2048)
        except socket.timeout:
            if knock_port and time.time() - knock_at >= 4:
                break
            continue
        if frame[6:12] in (SERVER_MAC, PROBE_MAC):
            continue
        # Guest ARP REQUEST for the fake peer: answer it.
        reply = arp_reply_to(frame)
        if reply is not None:
            send(reply)
            answered_arp = True
            continue
        # The knock: UDP from the guest to PROBE_IP:53001.
        if (len(frame) >= 14 + 20 + 8
                and struct.unpack("!H", frame[12:14])[0] == ETH_IPV4
                and frame[14 + 9] == 17):
            udp = 14 + 20
            sport, dport = struct.unpack("!HH", frame[udp:udp + 4])
            dst = socket.inet_ntoa(frame[14 + 16:14 + 20])
            if dport == 53001 and dst == PROBE_IP and knock_port is None:
                knock_port = sport
                knock_at = time.time()
                log("KNOCK_SEEN", "port %d" % sport)
        if knock_port and time.time() - knock_at >= 4:
            break
    if knock_port is None:
        log("UDP_REPLY", "NO-KNOCK")
        return 1

    time.sleep(max(0.0, 4 - (time.time() - knock_at)))
    # UDP datagram PROBE_IP:53001 -> GUEST_IP:knock_port, payload BLOCKED-OK.
    payload = b"BLOCKED-OK"
    udp = struct.pack("!HHHH", 53001, knock_port, 8 + len(payload), 0) + payload
    iph = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(udp), 0x4321, 0,
                      64, 17, 0)
    iph += socket.inet_aton(PROBE_IP) + socket.inet_aton(GUEST_IP)
    iph = iph[:10] + struct.pack("!H", ip_checksum(iph)) + iph[12:]
    send(eth(guest_mac, PROBE_MAC, ETH_IPV4, iph + udp))
    log("UDP_REPLY_SENT", "after %.1fs" % (time.time() - knock_at))
    log("LAB_DONE", "ok")
    return 0


def run_idledrain(sock, send, log, guest_mac):
    # ---- ARP probe while the guest is idle (no syscall owns the NIC) --
    def arp_probe(tag):
        send(arp_request(PROBE_MAC, PROBE_IP, GUEST_IP))
        end = time.time() + 5
        while time.time() < end:
            try:
                frame, _ = sock.recvfrom(2048)
            except socket.timeout:
                send(arp_request(PROBE_MAC, PROBE_IP, GUEST_IP))
                continue
            if frame[6:12] in (SERVER_MAC, PROBE_MAC):
                continue
            reply = parse_arp_reply(frame)
            if reply and reply[0] == GUEST_IP:
                log(tag, reply[1].hex())
                return True
        log(tag, "TIMEOUT")
        return False

    time.sleep(3)                                 # let the guest go idle
    if not arp_probe("ARP_REPLY_OK"):
        return 1

    # ---- junk blast: unclaimed ethertype, broadcast, paced ------------
    junk = eth(bytes.fromhex("ffffffffffff"), PROBE_MAC, ETH_JUNK,
               b"T5-IDLE-DRAIN-JUNK")
    for _ in range(JUNK_FRAMES):
        send(junk)
        time.sleep(JUNK_GAP_S)
    log("JUNK_SENT", JUNK_FRAMES)

    # ---- the drain must still serve through load ----------------------
    time.sleep(2)
    if not arp_probe("ARP_REPLY2_OK"):
        return 1

    log("LAB_DONE", "ok")
    return 0


if __name__ == "__main__":
    main()
