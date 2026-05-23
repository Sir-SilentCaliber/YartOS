/* Yart OS - the single source of truth for "what apps exist".
 * The dock and the app drawer both index into this table. */
#include <yart/apps.h>
#include <yart/string.h>

extern void open_files(const char *p);
extern void open_terminal(void);
extern void open_editor(const char *p);
extern void open_clock(void);
extern void open_sysinfo(void);
extern void open_settings(void);
extern void open_calc(void);
extern void open_sysmon(void);

static void l_files(void)    { open_files("/home/yart"); }
static void l_term(void)     { open_terminal(); }
static void l_editor(void)   { open_editor(0); }
static void l_clock(void)    { open_clock(); }
static void l_info(void)     { open_sysinfo(); }
static void l_settings(void) { open_settings(); }
static void l_calc(void)     { open_calc(); }
static void l_mon(void)      { open_sysmon(); }
static void l_root(void)     { open_files("/"); }

const yart_app_t yart_app_catalog[] = {
    { "Files",    ICON_FILES,   l_files    },
    { "Term",     ICON_TERM,    l_term     },
    { "Editor",   ICON_EDITOR,  l_editor   },
    { "Calc",     ICON_CALC,    l_calc     },
    { "Mon",      ICON_MONITOR, l_mon      },
    { "Clock",    ICON_CLOCK,   l_clock    },
    { "Info",     ICON_INFO,    l_info     },
    { "Settings", ICON_CONFIG,  l_settings },
    { "Root",     ICON_FOLDER,  l_root     },
};
const int yart_app_catalog_count = sizeof yart_app_catalog / sizeof yart_app_catalog[0];

const yart_app_t *app_find(const char *name) {
    for (int i = 0; i < yart_app_catalog_count; i++)
        if (strcmp(yart_app_catalog[i].name, name) == 0)
            return &yart_app_catalog[i];
    return 0;
}
