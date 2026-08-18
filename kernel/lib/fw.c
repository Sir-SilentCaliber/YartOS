/* Yart OS - firmware loader: VFS -> kernel heap buffer. */
#include <yart/types.h>
#include <yart/mm.h>
#include <yart/fs.h>
#include <yart/string.h>
#include <yart/fw.h>

int fw_load(const char *path, u8 **out, size_t *out_len) {
    /* NOTE: vfs_lookup() returns the tree's vnode WITHOUT taking a reference,
     * so it must NOT be unref'd here — doing so frees the node (and its data
     * buffer) out from under the tree. */
    vnode_t *v = vfs_lookup(path);
    if (!v || v->type != VN_FILE) return -1;
    size_t sz = v->size;
    if (!sz) { *out = NULL; *out_len = 0; return 0; }
    u8 *buf = kmalloc(sz);
    if (!buf) return -1;
    int r = vfs_read(v, buf, 0, sz);
    if (r != (int)sz) { kfree(buf); return -1; }
    *out = buf;
    *out_len = sz;
    return 0;
}

void fw_free(void *buf) { if (buf) kfree(buf); }

int fw_selftest(void) {
    u8 *buf = NULL;
    size_t len = 0;
    if (fw_load("/lib/firmware/rtw8822c_fw.bin", &buf, &len)) return 1;
    if (len < 8 || len > 1048576) { fw_free(buf); return 2; }  /* 8B..1MB stand-in */                 /* 64 KiB blob */
    if (buf[0] != 'R' || buf[1] != 'T' || buf[2] != 'W') { fw_free(buf); return 3; }
    for (size_t i = 4; i < len; i++)
        if (buf[i] != (u8)(i * 31 + 7)) { fw_free(buf); return 4; }
    fw_free(buf);
    return 0;
}
