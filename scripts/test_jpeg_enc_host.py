#!/usr/bin/env python3
"""Host unit-test for userland/jpeg_enc.c: encode test images with the encoder,
decode with PIL, compare MAE against the reference.  Proves the baseline JPEG
encoder is correct WITHOUT booting QEMU (the decoder counterpart is
scripts/test_jpeg_host.py).

Usage:  python3 scripts/test_jpeg_enc_host.py
Requires: gcc, python3-PIL, numpy
"""
import subprocess, os, tempfile, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENC_C = os.path.join(ROOT, "userland", "jpeg_enc.c")

def main():
    tmp = tempfile.mkdtemp(prefix="jpegenc_")
    tests = [(640,480), (1280,800), (320,192)]
    # generate references
    for w, h in tests:
        arr = np.zeros((h, w, 3), dtype=np.uint8)
        for y in range(h):
            for x in range(w):
                arr[y,x] = (x*255//w, y*255//h, (x+y)*255//(w+h))
                if x > w*0.62 and x < w*0.8 and y > h*0.4 and y < h*0.7:
                    arr[y,x] = (255, 40, 40)
        arr.tofile(os.path.join(tmp, f"ref_{w}x{h}.raw"))

    harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include "''' + ENC_C.replace('\\','/') + r'''"
int main(int argc, char **argv) {
    int sizes[][2] = {{640,480},{1280,800},{320,192}};
    int nfail = 0;
    for (int k = 0; k < 3; k++) {
        int w = sizes[k][0], h = sizes[k][1];
        unsigned int *px = malloc((size_t)w*h*4);
        unsigned char *ref = malloc((size_t)w*h*3);
        char raw[256]; sprintf(raw, "%s/ref_%dx%d.raw", argv[1], w, h);
        FILE *fr = fopen(raw, "rb"); fread(ref, 1, (size_t)w*h*3, fr); fclose(fr);
        /* build px from ref bytes */
        for (int y=0;y<h;y++)for(int x=0;x<w;x++){
            int r=ref[(y*w+x)*3], g=ref[(y*w+x)*3+1], b=ref[(y*w+x)*3+2];
            px[y*w+x]=0xFF000000u|(r<<16)|(g<<8)|b;
        }
        unsigned char *out = malloc((size_t)w*h*2);
        int len = jpeg_encode(px, w, h, w, 85, out, (unsigned int)w*h*2);
        char fn[256]; sprintf(fn, "%s/out_%dx%d.jpg", argv[1], w, h);
        FILE *f = fopen(fn, "wb"); fwrite(out, 1, len, f); fclose(f);
        printf("ENCODED %dx%d len=%d\n", w, h, len);
        if (len <= 0) nfail++;
        free(px); free(ref); free(out);
    }
    return nfail ? 1 : 0;
}
'''
    hc = os.path.join(tmp, "h.c")
    with open(hc, "w") as f: f.write(harness)
    exe = os.path.join(tmp, "h")
    subprocess.run(["gcc", "-O2", "-o", exe, hc, "-lm"], check=True)
    subprocess.run([exe, tmp], check=True)

    # decode with PIL and compare
    nfail = 0
    for w, h in tests:
        img = Image.open(os.path.join(tmp, f"out_{w}x{h}.jpg")).convert("RGB")
        arr = np.array(img)
        ref = np.fromfile(os.path.join(tmp, f"ref_{w}x{h}.raw"), dtype=np.uint8).reshape(h, w, 3)
        err = np.abs(arr.astype(int) - ref.astype(int)).mean()
        ok = err < 6.0
        print(f"{'PASS' if ok else 'FAIL'} {w}x{h} MAE {err:.2f}")
        if not ok: nfail += 1
    sys.exit(1 if nfail else 0)

if __name__ == "__main__":
    main()
