#!/usr/bin/env python3
"""Host unit-test for userland/jpeg.c: generates JPEGs with PIL at several
sizes/subsamplings, compiles jpeg.c with the host gcc, decodes each, and
compares the mean-absolute-error against PIL's own decode.  Proves the
baseline JPEG decoder is correct WITHOUT booting QEMU.

Usage:  python3 scripts/test_jpeg_host.py
Requires: gcc, python3-PIL, numpy
"""
import subprocess, os, tempfile, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JPEG_C = os.path.join(ROOT, "userland", "jpeg.c")

def main():
    tmp = tempfile.mkdtemp(prefix="jpegtest_")
    tests = []
    for size in [(64,48), (128,96), (640,480)]:
        w, h = size
        arr = np.zeros((h, w, 3), dtype=np.uint8)
        for y in range(h):
            for x in range(w):
                arr[y,x] = (int(x*255/w), int(y*255/h), int((x+y)*255/(w+h)))
        img = Image.fromarray(arr, 'RGB')
        for subs, name in [(0,'444'),(1,'422'),(2,'420')]:
            jpg = os.path.join(tmp, f"t_{w}x{h}_{name}.jpg")
            img.save(jpg, 'JPEG', quality=90, subsampling=subs)
            ref = np.array(Image.open(jpg).convert('RGB'))
            raw = os.path.join(tmp, f"t_{w}x{h}_{name}.raw")
            ref.tofile(raw)
            tests.append((jpg, w, h, raw))

    with open(os.path.join(tmp, "list.txt"), "w") as f:
        for jpg, w, h, raw in tests:
            f.write(f"{jpg} {w} {h} {raw}\n")

    harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include "''' + JPEG_C.replace('\\','/') + r'''"
int main(int argc, char **argv) {
    FILE *fl = fopen(argv[1], "r"); if (!fl) return 1;
    char jpg[512], raw[512]; int w, h, npass=0, nfail=0;
    while (fscanf(fl, "%511s %d %d %511s", jpg, &w, &h, raw) == 4) {
        FILE *fj = fopen(jpg,"rb"); fseek(fj,0,SEEK_END); long jl=ftell(fj); fseek(fj,0,SEEK_SET);
        unsigned char *jb = malloc(jl); fread(jb,1,jl,fj); fclose(fj);
        unsigned int *out = malloc((size_t)w*h*4);
        int ow,oh;
        int r = jpeg_decode(jb, jl, out, w, 0, 0, &ow, &oh);
        if (r || ow!=w || oh!=h) { printf("FAIL %s (r=%d %dx%d)\n", jpg, r, ow, oh); nfail++; free(jb); free(out); continue; }
        FILE *fr = fopen(raw,"rb"); unsigned char *ref = malloc((size_t)w*h*3); fread(ref,1,(size_t)w*h*3,fr); fclose(fr);
        long long err=0;
        for (int i=0;i<w*h;i++){ int rr=(out[i]>>16)&255, gg=(out[i]>>8)&255, bb=out[i]&255;
            err += abs(rr-ref[i*3]) + abs(gg-ref[i*3+1]) + abs(bb-ref[i*3+2]); }
        double mae = (double)err/(w*h*3);
        if (mae < 4.0) { printf("PASS %s (mae %.2f)\n", jpg, mae); npass++; }
        else { printf("FAIL %s (mae %.2f)\n", jpg, mae); nfail++; }
        free(ref); free(jb); free(out);
    }
    fclose(fl);
    printf("=== %d pass, %d fail ===\n", npass, nfail);
    return nfail ? 1 : 0;
}
'''
    hc = os.path.join(tmp, "h.c")
    with open(hc, "w") as f: f.write(harness)
    exe = os.path.join(tmp, "h")
    subprocess.run(["gcc", "-O2", "-o", exe, hc, "-lm"], check=True)
    r = subprocess.run([exe, os.path.join(tmp, "list.txt")])
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
