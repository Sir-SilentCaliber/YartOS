/* Yart userland: /bin/hello - the exec() demo binary.
 *
 * init forks a child that execs this file with argv ["/bin/hello",
 * "alpha", "beta"] and envp ["PATH=/bin", "HOME=/home/yart"].  If exec,
 * argv and envp all work, we print every argument + environment entry and
 * exit with status 7, which the parent verifies. */
#include "sys.h"

static void put_dec(long v) {
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    char out[24];
    int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
    klog(out);
}

int main_entry(int argc, char **argv, char **envp) {
    klog("hello: exec'd! argc=");
    put_dec(argc);
    klog("\n");
    for (int i = 0; i < argc; i++) {
        klog("hello: argv[");
        put_dec(i);
        klog("] = ");
        klog(argv[i]);
        klog("\n");
    }
    if (envp) {
        for (int i = 0; envp[i]; i++) {
            klog("hello: env = ");
            klog(envp[i]);
            klog("\n");
        }
    }
    klog("hello: all argv/envp received - exiting with status 7\n");
    return 7;
}
