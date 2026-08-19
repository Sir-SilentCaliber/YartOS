/* fsutil.h — small shared filesystem helpers for userland apps.
 *
 * The VFS only supports mkdir() one level deep (sys_mkdir resolves the
 * PARENT, not the whole chain), so these helpers provide the mkdir -p,
 * whole-file read/write, and "next free N" numbering that the camera and
 * viewer apps need.
 */
#pragma once
#include "sys.h"

/* The kernel caps a single read()/write() at USER_BUF_MAX (1 MiB), so all
 * whole-file helpers transfer in chunks no larger than this. */
#define FS_CHUNK  (256 * 1024)

/* mkdir -p: create every missing directory component. Returns 0 / -1. */
int fs_mkdir_p(const char *path);

/* Read a whole file into buf (cap bytes). Returns size or -1. */
long fs_read_file(const char *path, char *buf, long cap);

/* Create/truncate + write a whole file. Returns 0 / -1. */
int fs_write_file(const char *path, const void *buf, long len);

/* Append len bytes to a file (O_APPEND). Returns 0 / -1. */
int fs_append_file(const char *path, const void *buf, long len);

/* Find the next free "prefixN.ext" in dir, writing it into out (cap bytes).
 * Returns the number chosen, or -1 if dir can't be listed. */
int fs_next_free(const char *dir, const char *prefix, const char *ext,
                 char *out, int cap);

/* itoa into out (>= 16 bytes). */
void fs_itoa(long v, char *out);

/* ends_with: does s end with suffix? */
int fs_endswith(const char *s, const char *suffix);

/* bounded string copy (always NUL-terminates) */
static inline void fs_copystr(char *d, const char *s, int cap) {
    int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
