#!/usr/bin/env python3
"""Generate the RTL8822C PHY init table blob from the Linux rtw88 source.

Parses drivers/net/wireless/realtek/rtw88/rtw8822c_table.c (the 1.1 MB of
{addr, data} register tables) and emits a compact binary the kernel loads
from the initrd.  This avoids hand-transcribing register tables - a source of
bit-flip bugs - by extracting the data directly from the canonical source.

Usage:
    python3 scripts/gen_rtw_tables.py /path/to/rtw8822c_table.c out.bin

Blob format (all little-endian):
    u32 magic       = 0x50575452  ("RTWP")
    u32 n_tables
    per table:
        char name[16]            (e.g. "rtw8822c_bb")
        u32  n_pairs
        u32  cfg_kind            (0=mac,1=agc,2=bb,3=rf_a,4=rf_b,5=bb_pg)
        then n_pairs * 2 * u32   ({addr, data} pairs)
"""
import re
import struct
import sys

KEEP = {
    "rtw8822c_mac":          0,
    "rtw8822c_agc":          1,
    "rtw8822c_bb":           2,
    "rtw8822c_bb_pg_type0":  5,
    "rtw8822c_rf_a":         3,
    "rtw8822c_rf_b":         4,
}

ARRAY_RE = re.compile(
    r"static\s+const\s+u32\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};",
    re.DOTALL)


def parse_ints(body):
    toks = re.findall(r"0x[0-9a-fA-F]+|\d+", body)
    out = []
    for t in toks:
        v = int(t, 16) if t.lower().startswith("0x") else int(t)
        out.append(v & 0xFFFFFFFF)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "rtw8822c_phy.bin"
    text = open(src, encoding="utf-8", errors="replace").read()

    tables = []
    for name, body in ARRAY_RE.findall(text):
        if name not in KEEP:
            continue
        vals = parse_ints(body)
        if len(vals) % 2:
            print(f"WARN: {name} has odd u32 count ({len(vals)})")
            vals = vals[:len(vals) // 2 * 2]
        tables.append((name, KEEP[name], vals))
        print(f"  {name}: {len(vals)//2} pairs")

    if not tables:
        print("ERROR: no matching tables found")
        return 1

    blob = bytearray()
    blob += struct.pack("<II", 0x50575452, len(tables))
    for name, kind, vals in tables:
        name_b = name.encode()[:16].ljust(16, b"\x00")
        blob += name_b
        blob += struct.pack("<II", len(vals) // 2, kind)
        for v in vals:
            blob += struct.pack("<I", v)
    open(out, "wb").write(bytes(blob))
    print(f"wrote {out}: {len(blob)} bytes, {len(tables)} tables")
    return 0


if __name__ == "__main__":
    sys.exit(main())
