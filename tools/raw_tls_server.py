#!/usr/bin/env python3
"""Raw TLS 1.2 RSA server (0x003C) for testing YartOS's client.
Same math as OpenSSL but with explicit checks at every step so a
failure pinpoints the exact byte that's wrong."""
import socket, os, struct, hmac, hashlib, sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEY = serialization.load_pem_private_key(open('/tmp/crypto/key.pem','rb').read(), None)
CERT = open('/tmp/crypto/cert.der','rb').read()

def read_record(conn):
    hdr = b""
    while len(hdr) < 5:
        d = conn.recv(5 - len(hdr))
        if not d: return None
        hdr += d
    rl = int.from_bytes(hdr[3:5], 'big')
    body = b""
    while len(body) < rl:
        d = conn.recv(rl - len(body))
        if not d: return None
        body += d
    return hdr[0], body

def prf(secret, label, seed, n):
    out = b""
    a = hmac.new(secret, label + seed, hashlib.sha256).digest()
    while len(out) < n:
        out += hmac.new(secret, a + label + seed, hashlib.sha256).digest()
        a = hmac.new(secret, a, hashlib.sha256).digest()
    return out[:n]

def mac(seq, key, type_, version, data):
    m = seq.to_bytes(8,'big') + bytes([type_]) + version + len(data).to_bytes(2,'big') + data
    return hmac.new(key, m, hashlib.sha256).digest()

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 9443)); s.listen(4)
sys.stdout.write("raw TLS server on :9443\n"); sys.stdout.flush()
while True:
    c, _ = s.accept()
    try:
        # --- ClientHello ---
        t, ch = read_record(c)
        hslen = int.from_bytes(ch[1:4],'big')
        hs = ch[:4+hslen]
        version = hs[4:6]
        cr = hs[6:38]
        print("CH ok, version", version.hex(), flush=True)
        # --- ServerHello + Cert + SHD ---
        sr = os.urandom(32)
        sh_body = b"\x03\x03" + sr + b"\x00" + b"\x00\x3c" + b"\x00"
        sh = b"\x02" + len(sh_body).to_bytes(3,'big') + sh_body
        cert_body = (3 + len(CERT)).to_bytes(3,'big') + len(CERT).to_bytes(3,'big') + CERT
        certmsg = b"\x0b" + len(cert_body).to_bytes(3,'big') + cert_body
        shd = b"\x0e" + b"\x00\x00\x00"
        ch_msg = b"\x01" + (len(ch)-4).to_bytes(3,'big') + ch[4:]
        hs_hash = hashlib.sha256()
        for m in (ch_msg, sh, certmsg, shd): hs_hash.update(m)   # RFC ORDER
        record = lambda t_, b_: b"\x16\x03\x03" + len(b_).to_bytes(2,'big') + b_
        c.sendall(record(22, sh + certmsg + shd))
        print("SH+Cert+SHD sent", flush=True)
        # --- ClientKeyExchange ---
        t, cke = read_record(c)
        cke_body = cke[4:]
        enc_len = int.from_bytes(cke_body[:2],'big')
        enc = cke_body[2:2+enc_len]
        pre = KEY.decrypt(enc, padding.PKCS1v15())
        print("CKE decrypted, premaster", pre.hex()[:8], flush=True)
        master = prf(pre, b"master secret", cr + sr, 48)
        kb = prf(master, b"key expansion", sr + cr, 128)
        s_mac = kb[32:64]; s_key = kb[80:96]; s_iv = kb[112:128]
        c_mac = kb[0:32];  c_key = kb[64:80];  c_iv = kb[96:112]
        # --- CCS + Finished ---
        t, ccs = read_record(c)
        print("CCS received:", ccs.hex(), flush=True)
        t, fin_rec = read_record(c)
        print("Finished record type", t, "len", len(fin_rec), flush=True)
        iv = fin_rec[:16]
        dec = Cipher(algorithms.AES(c_key), modes.CBC(iv)).decryptor().update(fin_rec[16:])
        pad = dec[-1]
        plain = dec[:-pad]
        data, macv = plain[:len(plain)-32], plain[len(plain)-32:]
        exp = mac(0, c_mac, 22, b"\x03\x03", data)
        print("Finished MAC:", "OK" if macv == exp else "FAIL", flush=True)
        hs_hash.update(b"\x10" + (len(cke)-4).to_bytes(3,'big') + cke[4:])
        digest = hs_hash.digest()
        vd = prf(master, b"client finished", digest, 12)
        print("verify_data:", "OK" if data[4:] == vd else "FAIL", flush=True)
        # --- send our CCS + Finished ---
        c.sendall(b"\x14\x03\x03\x00\x01\x01")
        h = hashlib.sha256()
        for m in (ch_msg, sh, certmsg, shd,
                  b"\x10"+(len(cke)-4).to_bytes(3,'big')+cke[4:],
                  b"\x14\x00\x00\x0c"+vd):
            h.update(m)
        sfin = b"\x14\x00\x00\x0c" + prf(master, b"server finished", h.digest(), 12)
        print("server digest:", h.digest().hex(), flush=True)
        inner = sfin + mac(0, s_mac, 22, b"\x03\x03", sfin)
        padlen = 16 - (len(inner) % 16) or 16
        padb = bytes([padlen]) * padlen
        plain2 = inner + padb
        iv2 = os.urandom(16)
        ct2 = Cipher(algorithms.AES(s_key), modes.CBC(iv2)).encryptor().update(plain2)
        c.sendall(b"\x16\x03\x03" + (16+len(ct2)).to_bytes(2,'big') + iv2 + ct2)
        print("server Finished sent", flush=True)
        # --- read app data ---
        t, app = read_record(c)
        iv3 = app[:16]
        dec3 = Cipher(algorithms.AES(c_key), modes.CBC(iv3)).decryptor().update(app[16:])
        p3 = dec3[:-dec3[-1]]
        print("APP DATA:", p3[:40], flush=True)
        # respond
        resp = b"HTTP/1.1 200 OK\r\nContent-Length: 18\r\nConnection: close\r\n\r\nTLS-FROM-YART-HOST"
        inner = resp + mac(1, s_mac, 23, b"\x03\x03", resp)
        padlen = 16 - (len(inner) % 16) or 16
        padb = bytes([padlen]) * padlen
        iv4 = os.urandom(16)
        ct4 = Cipher(algorithms.AES(s_key), modes.CBC(iv4)).encryptor().update(inner + padb)
        print("DBG resp=%d inner=%d pad=%d ct4=%d last_pt=%s" % (
            len(resp), len(inner), len(padb), len(ct4),
            (inner+padb)[-16:].hex()), flush=True)
        c.sendall(b"\x17\x03\x03" + (16+len(ct4)).to_bytes(2,'big') + iv4 + ct4)
        print("response sent", flush=True)
        c.close()
    except Exception as e:
        print("ERR:", e, flush=True)
        try: c.close()
        except Exception: pass
