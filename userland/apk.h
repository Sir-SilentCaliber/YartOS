/* apk.h — YartOS package manager (apk-style CLI) interface.
 *
 * Command surface mirrors Alpine's apk (`apk add/del/list/search/info`) so
 * the UX feels like the real thing.  The package FORMAT is YartOS's own
 * .ypkg archive (see apk_core.c): native x86_64 ELF binaries + a manifest.
 * Installing a GUI package drops a the desktop-entry dir entry,
 * which the compositor scans so the app appears in the Super launcher —
 * exactly the "install -> press Super -> it's there" flow.
 */
#pragma once
#include "sys.h"

#define APK_REPO_DIR        "/repo"
#define APK_DB_DIR          "/var/db/ypkg"
#define APK_APPS_DIR        "/usr/share/applications"

typedef void (*apk_emit_t)(const char *line);

/* Run the apk command. Returns exit status (0 = ok, 1 = error). */
int apk_main(int argc, char **argv, apk_emit_t emit);
