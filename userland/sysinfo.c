/* /bin/sysinfo — a tiny CLI system-info tool.
 *
 * Ships as an INSTALLABLE package (`apk add sysinfo`) alongside the
 * Calculator, to demonstrate that the package repo holds multiple packages
 * and `apk list` / `apk add` / `apk del` all work across them.
 * Prints uname-style info + uptime to fd 1 (the serial console).
 */
#include "sys.h"

static void emit(const char *s) { write(1, s, strlen(s)); }
static void emit_num(long v) {
    char b[24]; int i = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0 && i < 22) { b[i++] = (char)('0' + v % 10); v /= 10; }
    char out[24]; int k = 0;
    while (i > 0) out[k++] = b[--i];
    out[k] = 0;
    emit(out);
}

int main_entry(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    emit("sysinfo: YartOS 0.8.0-max\n");
    emit("  kernel: x86_64 (ring-3 compositor)\n");
    long ms = time_ms();
    emit("  uptime_ms: "); emit_num(ms); emit("\n");
    long wt = wall_time();
    emit("  wall_time: "); emit_num(wt); emit("\n");
    emit("  cpu: "); emit_num(getcpu()); emit("\n");
    return 0;
}
