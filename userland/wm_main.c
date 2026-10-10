/* /bin/wm — the ring-3 compositor, as its OWN binary.
 *
 * This is the split that makes the session model work (like a Linux distro):
 * init (pid ~1) is now a supervisor that fork/execs this binary; when the wm
 * exits (Ctrl+Alt+Backspace, a crash, or `logout`), init falls back to a text
 * console and can start a fresh session.  wm_run() is the whole compositor;
 * returning from it (or crashing) ends THIS session, not the machine.
 */
#include "sys.h"

void wm_run(void);

int main_entry(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    wm_run();
    /* wm_run returned: the session ended cleanly.  exit(0) lets init reap us
     * and drop to the text console / restart the session. */
    return 0;
}
