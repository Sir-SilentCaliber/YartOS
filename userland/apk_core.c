/* apk_core.c — YartOS package manager engine.
 *
 * Roadmap #7.  HONEST: this is a NATIVE package manager with Alpine-apk's
 * command surface, NOT a port of apk-tools (which needs musl + openssl +
 * libfetch — a whole Linux userspace; a full port is a separate, much
 * larger project and is independent of this installer).
 *
 * Package format (.ypkg): a tiny archive of NATIVE YartOS binaries:
 *   header  (magic "YPKG", version, nfiles, name[32], ver[32], desc[128],
 *            icon[32], desktop flag)
 *   nfiles entries:  [u32 plen][path][u32 mode][u32 size][bytes]
 *
 * Install flow (`apk add calc`):
 *   1. read /repo/calc.ypkg
 *   2. write each file to its path (fs_mkdir_p parents; +x if mode=exec)
 *   3. if GUI: write /usr/share/applications/<name>.desktop
 *   4. record installed paths in /var/db/ypkg/<name> (for `apk del`)
 *   5. fsync() so it survives reboot
 *
 * The compositor scans the desktop-entry dir and auto-registers any new app
 * in the Super launcher — so `apk add calc` makes the Calculator appear in
 * the app grid exactly like an Ubuntu install.
 */
#include "apk.h"
#include "fsutil.h"

/* ---- remote repository (real networking) --------------------------------
 * apk is NOT local-only anymore.  Packages that aren't in /repo are fetched
 * over the network with a plain HTTP/1.0 GET (built on the kernel's TCP
 * client + optional DNS).  The remote repo serves the SAME .ypkg format as
 * /repo (native YartOS x86_64 binaries).  The QEMU host is reachable from
 * the guest at 10.0.2.2, so this default points at a repo server on the
 * host; change APK_REMOTE_HOST to a hostname/IP to point elsewhere (a
 * hostname is resolved via the kernel DNS client). */
#define APK_REMOTE_HOST "10.0.2.2"
#define APK_REMOTE_PORT 8000
#define APK_REMOTE_DIR  "/repo"

#define YPKG_MAGIC 0x4B505059u   /* "YPKG" little-endian */

/* parse "a.b.c.d" into a host-order u32, or 0 if not a valid literal */
static u32 parse_ip4(const char *s) {
    u32 ip = 0; int part = 0, val = 0, seen = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); seen = 1; }
        else if (*p == '.' || *p == 0) {
            if (!seen || val > 255) return 0;
            ip = (ip << 8) | (u32)val;
            part++; val = 0; seen = 0;
            if (*p == 0) break;
        } else return 0;
    }
    return (part == 4) ? ip : 0;
}

/* Minimal HTTP/1.0 GET.  Returns the response BODY length, or -1. */
static long http_get(const char *host, u16 port, const char *path,
                     char *out, long cap) {
    u32 ip = parse_ip4(host);
    if (!ip) {
        unsigned int d = 0;
        if (dns_resolve(host, &d) != 0 || !d) return -1;
        ip = d;
    }
    long c = tcp_connect(ip, port);
    if (c < 0) return -1;

    char req[640]; int k = 0;
    const char *g = "GET ";            for (; *g; g++) req[k++] = *g;
    for (const char *p = path; *p; p++) req[k++] = *p;
    const char *h1 = " HTTP/1.0\r\nHost: ";
    for (; *h1; h1++) req[k++] = *h1;
    for (const char *p = host; *p; p++) req[k++] = *p;
    const char *h2 = "\r\nConnection: close\r\n\r\n";
    for (; *h2; h2++) req[k++] = *h2;

    if (tcp_send(c, req, k) < 0) { tcp_close(c); return -1; }

    long total = 0;
    int  quiet = 0;
    while (total < cap - 1 && quiet < 200) {      /* ~1 s silence = EOF   */
        long n = tcp_recv(c, out + total, cap - 1 - total);
        if (n > 0) { total += n; quiet = 0; }
        else       { quiet++; sleep(5); }
    }
    tcp_close(c);
    out[total] = 0;

    /* strip the header block */
    char *body = out;
    for (long i = 0; i + 3 < total; i++)
        if (out[i] == '\r' && out[i+1] == '\n' &&
            out[i+2] == '\r' && out[i+3] == '\n') { body = out + i + 4; break; }
    long blen = total - (long)(body - out);
    if (blen < 0) blen = 0;
    for (long i = 0; i < blen; i++) out[i] = body[i];
    out[blen] = 0;
    return blen;
}

/* Download <name>.ypkg from the remote repo into `out`. */
static long apk_remote_get(const char *name, char *out, long cap) {
    char path[160]; int k = 0;
    const char *d = APK_REMOTE_DIR; for (; *d; d++) path[k++] = *d;
    path[k++] = '/';
    for (const char *p = name; *p && k < 150; p++) path[k++] = *p;
    const char *suf = ".ypkg"; while (*suf && k < 159) path[k++] = *suf++;
    path[k] = 0;
    return http_get(APK_REMOTE_HOST, APK_REMOTE_PORT, path, out, cap);
}


typedef struct {
    u32 magic;
    u32 version;
    u32 nfiles;
    char name[32];
    char appver[32];
    char desc[128];
    char icon[32];
    u32 desktop;
} ypkg_header_t;

static u32 rd_u32(const unsigned char *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int read_header(const char *path, ypkg_header_t *h) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char buf[sizeof(ypkg_header_t)];
    long n = read(fd, buf, sizeof buf);
    close(fd);
    if (n != (long)sizeof(ypkg_header_t)) return -1;
    if (rd_u32(buf) != YPKG_MAGIC) return -1;
    h->magic = rd_u32(buf + 0);
    h->version = rd_u32(buf + 4);
    h->nfiles = rd_u32(buf + 8);
    fs_copystr(h->name, (char *)buf + 12, 32);
    fs_copystr(h->appver, (char *)buf + 44, 32);
    fs_copystr(h->desc, (char *)buf + 76, 128);
    fs_copystr(h->icon, (char *)buf + 204, 32);
    h->desktop = rd_u32(buf + 236);
    return 0;
}

/* Installing touches /bin and /usr/share — root-owned paths — so apk must
 * elevate first, exactly like `sudo apk` on Alpine.  YartOS's doas(password)
 * elevates the admin account (the demo account "demo", password "yart"). */
static int apk_elevate(apk_emit_t emit) {
    if (doas("yart") == 0) return 0;
    emit("apk: requires root (doas failed)");
    return -1;
}

static int is_installed(const char *name) {
    char dbpath[160];
    fs_copystr(dbpath, APK_DB_DIR, sizeof dbpath);
    int k = (int)strlen(dbpath);
    dbpath[k++] = '/';
    for (const char *p = name; *p && k < 150; p++) dbpath[k++] = *p;
    dbpath[k] = 0;
    int fd = open(dbpath, O_RDONLY);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

static void emit_row(apk_emit_t emit, const char *name, const char *ver,
                     const char *desc) {
    char line[200];
    fs_copystr(line, name, sizeof line);
    int k = (int)strlen(line);
    line[k++] = ' '; line[k++] = ' ';
    for (const char *p = ver; *p && k < 190; p++) line[k++] = *p;
    line[k++] = ' '; line[k++] = ' ';
    for (const char *p = desc; *p && k < 199; p++) line[k++] = *p;
    line[k] = 0;
    emit(line);
}

/* iterate repo *.ypkg through emit; filter = substring, only_installed=1 */
static int each_pkg(apk_emit_t emit, const char *filter, int only_installed) {
    int fd = open(APK_REPO_DIR, O_RDONLY);
    if (fd < 0) { emit("apk: repo not found (no /repo)"); return -1; }
    int found = 0;
    yart_dirent_t de[32];
    long n;
    while ((n = getdents(fd, de, 32)) > 0) {
        for (long i = 0; i < n; i++) {
            if (de[i].type != 1) continue;
            if (!fs_endswith(de[i].name, ".ypkg")) continue;
            char path[160];
            fs_copystr(path, APK_REPO_DIR, sizeof path);
            int k = (int)strlen(path);
            path[k++] = '/';
            for (const char *p = de[i].name; *p && k < 150; p++) path[k++] = *p;
            path[k] = 0;
            ypkg_header_t h;
            if (read_header(path, &h) != 0) continue;
            int inst = is_installed(h.name);
            if (only_installed && !inst) continue;
            if (filter && filter[0]) {
                int m = 0;
                for (int a = 0; h.name[a]; a++) {
                    int ok = 1;
                    for (int b = 0; filter[b]; b++) {
                        char ca = h.name[a + b];
                        if (!ca) { ok = 0; break; }
                        if (ca >= 'A' && ca <= 'Z') ca += 32;
                        char cb = filter[b];
                        if (cb >= 'A' && cb <= 'Z') cb += 32;
                        if (ca != cb) { ok = 0; break; }
                    }
                    if (ok) { m = 1; break; }
                }
                if (!m) continue;
            }
            emit_row(emit, h.name, h.appver, h.desc);
            found++;
        }
    }
    close(fd);
    if (!found) emit("apk: no packages found");
    return 0;
}

static int find_pkg(const char *name, char *out, int cap) {
    int fd = open(APK_REPO_DIR, O_RDONLY);
    if (fd < 0) return -1;
    yart_dirent_t de[32];
    long n;
    while ((n = getdents(fd, de, 32)) > 0) {
        for (long i = 0; i < n; i++) {
            if (de[i].type != 1) continue;
            char p[160];
            fs_copystr(p, APK_REPO_DIR, sizeof p);
            int k = (int)strlen(p);
            p[k++] = '/';
            for (const char *q = de[i].name; *q && k < 150; q++) p[k++] = *q;
            p[k] = 0;
            ypkg_header_t h;
            if (read_header(p, &h) != 0) continue;
            if (strcmp(h.name, name) == 0) {
                fs_copystr(out, p, cap);
                close(fd);
                return 0;
            }
        }
    }
    close(fd);
    return -1;
}

static int add_pkg(const char *name, apk_emit_t emit) {
    if (apk_elevate(emit) != 0) return -1;
    if (is_installed(name)) { emit("apk: already installed"); return 0; }
    char path[160];
    char *raw = (char *)mmap(16 * 1024 * 1024);
    if (!raw) { emit("apk: out of memory"); return -1; }

    long n = -1;
    if (find_pkg(name, path, sizeof path) == 0) {
        /* local /repo hit */
        n = fs_read_file(path, raw, 16 * 1024 * 1024);
    } else {
        /* not in /repo: fetch it over the network */
        emit("fetching from remote repo...");
        n = apk_remote_get(name, raw, 16 * 1024 * 1024);
        if (n <= 0) {
            munmap((long)raw);
            char line[200];
            fs_copystr(line, "apk: no such package '", sizeof line);
            int k = (int)strlen(line);
            for (const char *p = name; *p && k < 196; p++) line[k++] = *p;
            line[k++] = '\''; line[k] = 0;
            emit(line);
            emit("      (checked /repo and the remote repo)");
            emit("      local packages:");
            each_pkg(emit, "", 0);
            return -1;
        }
        /* stash the download so the install path below is identical */
        char tmp[96];
        fs_copystr(tmp, "/tmp/", sizeof tmp);
        int tk = (int)strlen(tmp);
        for (const char *p = name; *p && tk < 90; p++) tmp[tk++] = *p;
        const char *suf = ".ypkg"; while (*suf && tk < 95) tmp[tk++] = *suf++;
        tmp[tk] = 0;
        if (fs_write_file(tmp, raw, n) != 0) {
            munmap((long)raw); emit("apk: temp write failed"); return -1;
        }
        fs_copystr(path, tmp, sizeof path);
        char dl[80]; fs_copystr(dl, "downloaded ", sizeof dl);
        int dk = (int)strlen(dl);
        fs_itoa(n, dl + dk); dk = (int)strlen(dl);
        fs_copystr(dl + dk, " bytes", (int)(sizeof dl - dk));
        emit(dl);
    }
    if (n <= 0) { munmap((long)raw); emit("apk: read failed"); return -1; }

    ypkg_header_t h;
    if (read_header(path, &h) != 0) { munmap((long)raw); emit("apk: bad package"); return -1; }

    char line[220];
    fs_copystr(line, "installing ", sizeof line);
    int k = (int)strlen(line);
    for (const char *p = h.name; *p && k < 190; p++) line[k++] = *p;
    line[k++] = ' ';
    for (const char *p = h.appver; *p && k < 215; p++) line[k++] = *p;
    line[k] = 0;
    emit(line);

    fs_mkdir_p(APK_DB_DIR);
    char dbpath[160];
    fs_copystr(dbpath, APK_DB_DIR, sizeof dbpath);
    k = (int)strlen(dbpath); dbpath[k++] = '/';
    for (const char *p = h.name; *p && k < 150; p++) dbpath[k++] = *p;
    dbpath[k] = 0;

    char db[4096];
    int dbn = 0;
    db[0] = 0;

    u32 off = sizeof(ypkg_header_t);
    int ok = 1;
    const char *why = "";
    for (u32 f = 0; f < h.nfiles && ok; f++) {
        if (off + 4 > (u32)n) { ok = 0; why = "truncated (plen)"; break; }
        u32 plen = rd_u32((unsigned char *)raw + off); off += 4;
        if (off + plen + 8 > (u32)n) { ok = 0; why = "truncated (path)"; break; }
        char fpath[192];
        fs_copystr(fpath, raw + off, (plen + 1 < 192 ? plen + 1 : 192));
        off += plen;
        u32 mode = rd_u32((unsigned char *)raw + off); off += 4;
        u32 size = rd_u32((unsigned char *)raw + off); off += 4;
        if (off + size > (u32)n) { ok = 0; why = "truncated (data)"; break; }

        char parent[192];
        fs_copystr(parent, fpath, sizeof parent);
        int pl = (int)strlen(parent);
        while (pl > 0 && parent[pl - 1] != '/') pl--;
        if (pl > 0) { parent[pl - 1] = 0; if (parent[0]) fs_mkdir_p(parent); }

        if (fs_write_file(fpath, raw + off, (long)size) != 0) { ok = 0; why = "write failed"; break; }
        if (mode == 1) chmod(fpath, 0755);
        { char wl[200]; fs_copystr(wl, "  wrote ", sizeof wl);
          int wk = (int)strlen(wl);
          for (const char *p = fpath; *p && wk < 196; p++) wl[wk++] = *p;
          wl[wk] = 0; emit(wl); }

        int fl = (int)strlen(fpath);
        if (dbn + fl + 2 < (int)sizeof(db)) {
            for (int i = 0; i < fl; i++) db[dbn++] = fpath[i];
            db[dbn++] = '\n';
        }
        off += size;
    }
    munmap((long)raw);

    if (ok && h.desktop) {
        fs_mkdir_p(APK_APPS_DIR);
        char desktop_path[180];
        fs_copystr(desktop_path, APK_APPS_DIR, sizeof desktop_path);
        int dk = (int)strlen(desktop_path); desktop_path[dk++] = '/';
        for (const char *p = h.name; *p && dk < 170; p++) desktop_path[dk++] = *p;
        const char *suf = ".desktop";
        while (*suf && dk < 179) desktop_path[dk++] = *suf++;
        desktop_path[dk] = 0;

        char df[600];
        int dn = 0;
        const char *pre = "[Desktop Entry]\nName=";
        while (*pre) df[dn++] = *pre++;
        for (const char *p = h.name; *p; p++) df[dn++] = *p;
        const char *pre2 = "\nExec=";
        while (*pre2) df[dn++] = *pre2++;
        int ln = 0; while (ln < dbn && db[ln] != '\n') ln++;
        for (int i = 0; i < ln; i++) df[dn++] = db[i];
        const char *pre3 = "\nIcon=";
        while (*pre3) df[dn++] = *pre3++;
        for (const char *p = h.icon; *p; p++) df[dn++] = *p;
        const char *pre4 = "\nType=Application\n";
        while (*pre4) df[dn++] = *pre4++;
        df[dn] = 0;
        if (fs_write_file(desktop_path, df, dn) != 0) ok = 0;
    }

    if (ok) {
        db[dbn] = 0;
        fs_write_file(dbpath, db, dbn);
        emit("syncing to disk...");
        fsync(0);
        emit("done - now in the launcher (press Super)");
        notify("apk: installed package");
        return 0;
    }
    emit("apk: install FAILED");
    { char wl[160]; fs_copystr(wl, "      reason: ", sizeof wl);
      int wk = (int)strlen(wl);
      for (const char *p = why; *p && wk < 158; p++) wl[wk++] = *p;
      wl[wk] = 0;
      emit(wl); }
    return -1;
}

static int del_pkg(const char *name, apk_emit_t emit) {
    if (apk_elevate(emit) != 0) return -1;
    char dbpath[160];
    fs_copystr(dbpath, APK_DB_DIR, sizeof dbpath);
    int k = (int)strlen(dbpath); dbpath[k++] = '/';
    for (const char *p = name; *p && k < 150; p++) dbpath[k++] = *p;
    dbpath[k] = 0;

    char db[4096];
    long n = fs_read_file(dbpath, db, sizeof db);
    if (n <= 0) {
        char line[200];
        fs_copystr(line, "apk: '", sizeof line);
        k = (int)strlen(line);
        for (const char *p = name; *p && k < 196; p++) line[k++] = *p;
        line[k++] = '\''; line[k] = 0;
        for (const char *p = " not installed"; *p && k < 199; p++) line[k++] = *p;
        line[k] = 0;
        emit(line);
        return -1;
    }

    int lines[256];
    int nl = 0;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (db[i] == '\n' || db[i] == 0) {
            if (i > start) lines[nl++] = start;
            start = i + 1;
            if (db[i] == 0) break;
        }
    }
    for (int i = nl - 1; i >= 0; i--) {
        int s = lines[i], e = s;
        while (db[e] && db[e] != '\n') e++;
        char fpath[192];
        int j = 0;
        for (int q = s; q < e && j < 190; q++) fpath[j++] = db[q];
        fpath[j] = 0;
        unlink(fpath);
    }

    char desktop_path[180];
    fs_copystr(desktop_path, APK_APPS_DIR, sizeof desktop_path);
    int dk = (int)strlen(desktop_path); desktop_path[dk++] = '/';
    for (const char *p = name; *p && dk < 168; p++) desktop_path[dk++] = *p;
    const char *suf = ".desktop";
    while (*suf && dk < 179) desktop_path[dk++] = *suf++;
    desktop_path[dk] = 0;
    unlink(desktop_path);

    unlink(dbpath);
    fsync(0);
    emit("removed");
    notify("apk: package removed");
    return 0;
}

static int info_pkg(const char *name, apk_emit_t emit) {
    char path[160];
    if (find_pkg(name, path, sizeof path) != 0) {
        emit("apk: no such package");
        return -1;
    }
    ypkg_header_t h;
    if (read_header(path, &h) != 0) { emit("apk: bad package"); return -1; }
    char line[220];
    fs_copystr(line, "name:     ", sizeof line);
    int k = (int)strlen(line);
    for (const char *p = h.name; *p && k < 210; p++) line[k++] = *p;
    line[k] = 0; emit(line);
    fs_copystr(line, "version:  ", sizeof line);
    k = (int)strlen(line);
    for (const char *p = h.appver; *p && k < 210; p++) line[k++] = *p;
    line[k] = 0; emit(line);
    fs_copystr(line, "desc:     ", sizeof line);
    k = (int)strlen(line);
    for (const char *p = h.desc; *p && k < 210; p++) line[k++] = *p;
    line[k] = 0; emit(line);
    fs_copystr(line, "installed:", sizeof line);
    k = (int)strlen(line);
    for (const char *p = is_installed(h.name) ? " yes" : " no"; *p && k < 210; p++) line[k++] = *p;
    line[k] = 0; emit(line);
    return 0;
}

/* `apk update`: fetch the remote repo's INDEX file and show it. */
static int update_repo(apk_emit_t emit) {
    char *buf = (char *)mmap(1024 * 1024);
    if (!buf) { emit("apk: out of memory"); return -1; }
    char path[80];
    fs_copystr(path, APK_REMOTE_DIR, sizeof path);
    int k = (int)strlen(path);
    const char *suf = "/INDEX"; while (*suf && k < 79) path[k++] = *suf++;
    path[k] = 0;
    long n = http_get(APK_REMOTE_HOST, APK_REMOTE_PORT, path, buf, 1024 * 1024);
    if (n <= 0) {
        munmap((long)buf);
        emit("apk: update failed - remote repo unreachable");
        emit("      repo: http://" APK_REMOTE_HOST "/repo/");
        return -1;
    }
    /* cache the index and echo it */
    fs_mkdir_p(APK_DB_DIR);
    char db[96];
    fs_copystr(db, APK_DB_DIR, sizeof db);
    int dk = (int)strlen(db);
    const char *suf2 = "/INDEX"; while (*suf2 && dk < 95) db[dk++] = *suf2++;
    db[dk] = 0;
    fs_write_file(db, buf, n);
    int start = 0;
    for (long i = 0; i <= n; i++) {
        if (buf[i] == '\n' || buf[i] == 0) {
            if (i > start) { char sv = buf[i]; buf[i] = 0; emit(buf + start); buf[i] = sv; }
            start = (int)i + 1;
            if (buf[i] == 0) break;
        }
    }
    munmap((long)buf);
    emit("updated");
    return 0;
}

int apk_main(int argc, char **argv, apk_emit_t emit) {
    if (argc < 2) {
        emit("usage: apk <add|del|list|search|info|update> [pkg]");
        emit("  apk add <pkg>    install (local /repo, then remote repo)");
        emit("  apk del <pkg>    remove it");
        emit("  apk list         list the local repository");
        emit("  apk search <s>   find packages");
        emit("  apk info <pkg>   details");
        emit("  apk update       fetch the remote repo index");
        return 1;
    }
    const char *sub = argv[1];
    const char *arg = argc > 2 ? argv[2] : "";

    if (strcmp(sub, "add") == 0) return add_pkg(arg, emit);
    if (strcmp(sub, "del") == 0 || strcmp(sub, "remove") == 0) return del_pkg(arg, emit);
    if (strcmp(sub, "list") == 0) return each_pkg(emit, "", 0);
    if (strcmp(sub, "search") == 0) return each_pkg(emit, arg, 0);
    if (strcmp(sub, "info") == 0) return info_pkg(arg, emit);
    if (strcmp(sub, "installed") == 0) return each_pkg(emit, "", 1);
    if (strcmp(sub, "update") == 0) return update_repo(emit);

    emit("apk: unknown command (add|del|list|search|info|update)");
    return 1;
}
