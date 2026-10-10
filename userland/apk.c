/* /bin/apk — standalone package manager binary (prints to fd 1 -> serial). */
#include "apk.h"
#include "fsutil.h"

static void emit_console(const char *line) {
    write(1, line, strlen(line));
    write(1, "\n", 1);
    klog(line);
    klog("\n");
}

int main_entry(int argc, char **argv, char **envp) {
    (void)envp;
    return apk_main(argc, argv, emit_console);
}
