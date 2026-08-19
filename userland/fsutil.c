/* fsutil.c — shared filesystem helpers (see fsutil.h). */
#include "fsutil.h"

int fs_mkdir_p(const char *path) {
    if (!path || !path[0]) return -1;
    char buf[256];
    int i = 0;
    while (path[i] && i < (int)sizeof(buf) - 1) { buf[i] = path[i]; i++; }
    buf[i] = 0;
    /* walk components, mkdir each prefix */
    for (int k = 1; buf[k]; k++) {
        if (buf[k] == '/') {
            char saved = buf[k];
            buf[k] = 0;
            /* ignore EEXIST (mkdir returns -1 either way; that's fine) */
            mkdir(buf);
            buf[k] = saved;
        }
    }
    mkdir(buf);
    return 0;
}

long fs_read_file(const char *path, char *buf, long cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        if (total >= cap - 1) break;
        long want = cap - 1 - total;
        if (want > FS_CHUNK) want = FS_CHUNK;   /* kernel caps a read/write at 1 MiB */
        long n = read(fd, buf + total, (size_t)want);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    buf[total] = 0;
    return total;
}

int fs_write_file(const char *path, const void *buf, long len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long off = 0;
    while (off < len) {
        long want = len - off;
        if (want > FS_CHUNK) want = FS_CHUNK;
        long w = write(fd, (const char *)buf + off, (size_t)want);
        if (w <= 0) break;
        off += w;
    }
    fsync(fd);
    close(fd);
    return (off == len) ? 0 : -1;
}

int fs_append_file(const char *path, const void *buf, long len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) return -1;
    long off = 0;
    while (off < len) {
        long want = len - off;
        if (want > FS_CHUNK) want = FS_CHUNK;
        long w = write(fd, (const char *)buf + off, (size_t)want);
        if (w <= 0) break;
        off += w;
    }
    fsync(fd);
    close(fd);
    return (off == len) ? 0 : -1;
}

void fs_itoa(long v, char *out) {
    char tmp[24];
    int i = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0 && i < 22) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0;
    if (neg) out[k++] = '-';
    while (i > 0) out[k++] = tmp[--i];
    out[k] = 0;
}

int fs_endswith(const char *s, const char *suffix) {
    long a = (long)strlen(s), b = (long)strlen(suffix);
    if (b > a) return 0;
    return strcmp(s + (a - b), suffix) == 0;
}

int fs_next_free(const char *dir, const char *prefix, const char *ext,
                 char *out, int cap) {
    /* highest existing number + 1; 0 if none */
    int fd = open(dir, O_RDONLY);
    int best = -1;
    if (fd >= 0) {
        yart_dirent_t d[16];
        long n;
        while ((n = getdents(fd, d, 16)) > 0) {
            for (long i = 0; i < n; i++) {
                const char *nm = d[i].name;
                if (strncmp(nm, prefix, strlen(prefix)) != 0) continue;
                if (!fs_endswith(nm, ext)) continue;
                int num = 0;
                const char *p = nm + strlen(prefix);
                while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
                if (num > best) best = num;
            }
        }
        close(fd);
    }
    int next = best + 1;
    int k = 0;
    for (const char *p = dir; *p && k < cap - 1; p++) out[k++] = *p;
    if (k && out[k - 1] != '/') out[k++] = '/';
    for (const char *p = prefix; *p && k < cap - 1; p++) out[k++] = *p;
    char num[16]; fs_itoa(next, num);
    for (const char *p = num; *p && k < cap - 1; p++) out[k++] = *p;
    for (const char *p = ext; *p && k < cap - 1; p++) out[k++] = *p;
    out[k] = 0;
    return next;
}
