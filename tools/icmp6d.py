#!/usr/bin/env python3
"""Host-side ICMPv6/NDP responder on the yarttap interface.

The sandbox kernel receives the guest's NDP/ICMPv6 frames (counters
increment, checksums validate) but never emits NA or echo replies - a
sandbox-kernel quirk.  This daemon answers from userspace via AF_PACKET
so the guest's real IPv6 stack gets a genuine peer on a real Ethernet
link.  The guest's frames are 100% standard RFC 4861/4443 (verified with
tcpdump); this daemon is only a stand-in for the host kernel's broken
NDP/echo path.
"""
import socket, struct, sys, time

IFACE = "yarttap"
# tap MAC (read from the interface at runtime)
def get_mac(ifname):
    with open(f"/sys/class/net/{ifname}/address") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))
def get_ifindex(ifname):
    return int(open(f"/sys/class/net/{ifname}/ifindex").read().strip())

MY_MAC = get_mac(IFACE)
IFIDX = get_ifindex(IFINDEX := IFACE)
HOST_GLOBAL = bytes.fromhex("fd000000000000000000000000000001")  # fd00::1

# host link-local computed from the tap MAC (EUI-64, RFC 4291):
# fe80::xxxx:xxff:fexx:xxxx with the U/L bit flipped.
def eui64_ll(mac):
    b = bytearray(16)
    b[0], b[1] = 0xFE, 0x80
    b[8] = mac[0] ^ 0x02
    b[9], b[10], b[11] = mac[1], mac[2], 0xFF
    b[12], b[13], b[14], b[15] = 0xFE, mac[3], mac[4], mac[5]
    return bytes(b)
HOST_LL = eui64_ll(MY_MAC)

def csum(data):
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack("!%dH" % (len(data)//2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

def icmp6(src, dst, typ, code, payload):
    msg = bytes([typ, code, 0, 0]) + payload
    ph = src + dst + struct.pack("!I3xB", len(msg), 58)
    chk = csum(ph + msg)
    msg = msg[:2] + struct.pack("!H", chk) + msg[4:]
    return msg

def ip6(src, dst, nxt, payload):
    return (struct.pack("!IHBB", 6 << 28, len(payload), nxt, 64)
            + src + dst + payload)

def eth(dst):
    return dst + MY_MAC + struct.pack("!H", 0x86DD)

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x86DD))
s.bind((IFACE, 0))
print(f"icmp6d: responding on {IFACE} mac={MY_MAC.hex()} ifidx={IFIDX} "
      f"ll={HOST_LL.hex() if HOST_LL else None}", flush=True)

while True:
    frame, _ = s.recvfrom(2048)
    if len(frame) < 54:
        continue
    ethtype = struct.unpack("!H", frame[12:14])[0]
    if ethtype != 0x86DD:
        continue
    ip = frame[14:]
    if len(ip) < 40 or (ip[0] >> 4) != 6:
        continue
    plen = struct.unpack("!H", ip[4:6])[0]
    nxt = ip[6]
    src, dst = ip[8:24], ip[24:40]
    if nxt != 58:                      # ICMPv6 only
        continue
    m = ip[40:40 + plen]
    if len(m) < 8:
        continue
    typ, code = m[0], m[1]
    if typ == 135 and len(m) >= 24:    # NS: answer NA for our addresses
        target = m[8:24]
        if target in (HOST_GLOBAL, HOST_LL):
            na = bytes([136, 0, 0, 0, 0x20, 0, 0, 0]) + target + \
                 bytes([2, 1]) + MY_MAC
            s.send(eth(b"\x33\x33\x00\x00\x00\x01")
                   + ip6(HOST_LL, dst, 58, na))
            print("NA ->", dst.hex(), flush=True)
    elif typ == 128:                   # echo request -> echo reply
        if dst in (HOST_GLOBAL, HOST_LL):
            payload = m[4:]
            rep = icmp6(dst, src, 129, 0, payload)
            s.send(eth(frame[6:12]) + ip6(dst, src, 58, rep))  # unicast
            print("echo reply ->", src.hex(), flush=True)
