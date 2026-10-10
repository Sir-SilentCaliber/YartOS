#!/usr/bin/env python3
"""Build a YartOS .ypkg package archive.

Usage:
  python3 scripts/mkpkg.py <out.ypkg> --name calc --version 1.0.0 \
      --desc "A simple calculator" --icon calculator [--desktop] \
      --file build/calc.elf:/bin/calc:exec \
      [--file other:/path:file] ...

File entries are  src:dest:mode  where mode is `file` (0644) or `exec` (0755).
Format (see userland/apk_core.c):
  header: u32 magic("YPKG") u32 version u32 nfiles
          char name[32] ver[32] desc[128] icon[32] u32 desktop
  then nfiles entries: u32 plen, path bytes, u32 mode, u32 size, data
"""
import struct, sys, os

MAGIC = 0x4B505059  # "YPKG"

def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0)
    out = args[0]
    rest = args[1:]
    name = ver = desc = icon = None
    desktop = 0
    files = []
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "--name": name = rest[i+1]; i += 2
        elif a == "--version": ver = rest[i+1]; i += 2
        elif a == "--desc": desc = rest[i+1]; i += 2
        elif a == "--icon": icon = rest[i+1]; i += 2
        elif a == "--desktop": desktop = 1; i += 1
        elif a == "--file": files.append(rest[i+1]); i += 2
        else:
            sys.stderr.write("unknown arg: %s\n" % a); sys.exit(2)
    if not name or not ver:
        sys.stderr.write("--name and --version required\n"); sys.exit(2)

    def cstr(s, n):
        b = s.encode()[:n-1]
        return b + b"\x00" * (n - len(b))

    hdr = struct.pack("<III", MAGIC, 1, len(files))
    hdr += cstr(name, 32) + cstr(ver, 32) + cstr(desc or "", 128) + cstr(icon or "", 32)
    hdr += struct.pack("<I", desktop)

    body = b""
    for f in files:
        src, dest, mode = f.split(":")
        data = open(src, "rb").read()
        dp = dest.encode()
        m = 1 if mode == "exec" else 0
        body += struct.pack("<I", len(dp)) + dp + struct.pack("<II", m, len(data)) + data

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(hdr + body)
    print("wrote %s (%s %s, %d files, %d bytes)" %
          (out, name, ver, len(files), len(hdr) + len(body)))

if __name__ == "__main__":
    main()
