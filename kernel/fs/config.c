/* Yart OS - /etc/yart.conf loader/saver and pin/unpin helpers. */
#include <yart/config.h>
#include <yart/fs.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/console.h>

yart_config_t g_config;

void config_load_defaults(void) {
    memset(&g_config, 0, sizeof g_config);
    strncpy(g_config.hostname, "yart", CONFIG_STR_LEN-1);
    strncpy(g_config.theme,    "slate-amber", CONFIG_STR_LEN-1);
    g_config.accent = 0xFFE8A87CU;
    g_config.border = 0xFFE8A87CU;
    g_config.corner_radius = 6;

    g_config.dock_auto_hide = false;
    strncpy(g_config.dock_position, "bottom", 7);
    g_config.dock_icon_size = 32;
    g_config.dock_spacing   = 12;

    /* Default pinned apps */
    const char *defaults[] = { "Files", "Term", "Editor", "Calc", "Mon" };
    g_config.dock_pinned_count = sizeof defaults / sizeof defaults[0];
    for (int i = 0; i < g_config.dock_pinned_count; i++)
        strncpy(g_config.dock_pinned[i], defaults[i], CONFIG_STR_LEN-1);

    g_config.topbar_height = 26;
    g_config.topbar_alpha  = 255;

    strncpy(g_config.wallpaper_mode, "gradient", 11);
    strncpy(g_config.wallpaper_path, "/etc/wallpaper.bmp", 127);

    strncpy(g_config.font_system,   "default", CONFIG_STR_LEN-1);
    strncpy(g_config.font_terminal, "default", CONFIG_STR_LEN-1);

    g_config.mouse_accel = 10;
    g_config.keyboard_repeat_delay = 400;
    g_config.keyboard_repeat_rate  = 33;

    g_config.display_fps = 30;
    g_config.display_night_light = 0;

    g_config.time_format24 = true;
    g_config.time_tz_offset = 0;

    g_config.power_dim_after   = 300;
    g_config.power_sleep_after = 900;
}

/* ---- parser ---- */
static int hex_to_int(const char *s) {
    int v = 0;
    while (*s) {
        char c = *s++;
        if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (c - 'A' + 10);
        else break;
    }
    return v;
}

static u32 parse_color(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    return 0xFF000000U | (u32)hex_to_int(s);
}

static int parse_int(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int v = 0, sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

static bool parse_bool(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return (*s == '1' || *s == 't' || *s == 'T' || *s == 'y' || *s == 'Y');
}

static void parse_csv(const char *s, char dst[][CONFIG_STR_LEN], int max, int *out_count) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (!*s) break;
        int i = 0;
        while (*s && *s != ',' && *s != '\n' && i < CONFIG_STR_LEN - 1) {
            if (*s == ' ' || *s == '\t') { s++; continue; }
            dst[n][i++] = *s++;
        }
        dst[n][i] = 0;
        if (i > 0) n++;
    }
    *out_count = n;
}

static void apply(const char *key, const char *val) {
    if      (strcmp(key, "hostname") == 0) strncpy(g_config.hostname, val, CONFIG_STR_LEN-1);
    else if (strcmp(key, "theme") == 0)    strncpy(g_config.theme,    val, CONFIG_STR_LEN-1);
    else if (strcmp(key, "accent") == 0)   g_config.accent = parse_color(val);
    else if (strcmp(key, "border") == 0)   g_config.border = parse_color(val);
    else if (strcmp(key, "corner_radius") == 0) g_config.corner_radius = parse_int(val);
    else if (strcmp(key, "dock.auto_hide") == 0) g_config.dock_auto_hide = parse_bool(val);
    else if (strcmp(key, "dock.position") == 0)  strncpy(g_config.dock_position, val, 7);
    else if (strcmp(key, "dock.icon_size") == 0) g_config.dock_icon_size = parse_int(val);
    else if (strcmp(key, "dock.spacing") == 0)   g_config.dock_spacing = parse_int(val);
    else if (strcmp(key, "dock.pinned") == 0) {
        parse_csv(val, g_config.dock_pinned, CONFIG_MAX_PINNED, &g_config.dock_pinned_count);
    }
    else if (strcmp(key, "topbar.height") == 0) g_config.topbar_height = parse_int(val);
    else if (strcmp(key, "topbar.alpha") == 0)  g_config.topbar_alpha = parse_int(val);
    else if (strcmp(key, "wallpaper.mode") == 0) strncpy(g_config.wallpaper_mode, val, 11);
    else if (strcmp(key, "wallpaper.path") == 0) strncpy(g_config.wallpaper_path, val, 127);
    else if (strcmp(key, "font.system") == 0)    strncpy(g_config.font_system,   val, CONFIG_STR_LEN-1);
    else if (strcmp(key, "font.terminal") == 0)  strncpy(g_config.font_terminal, val, CONFIG_STR_LEN-1);
    else if (strcmp(key, "mouse.accel") == 0)    g_config.mouse_accel = parse_int(val);
    else if (strcmp(key, "keyboard.repeat_delay") == 0) g_config.keyboard_repeat_delay = parse_int(val);
    else if (strcmp(key, "keyboard.repeat_rate") == 0)  g_config.keyboard_repeat_rate  = parse_int(val);
    else if (strcmp(key, "display.fps") == 0)        g_config.display_fps = parse_int(val);
    else if (strcmp(key, "display.night_light") == 0) g_config.display_night_light = parse_int(val);
    else if (strcmp(key, "time.format24") == 0) g_config.time_format24 = parse_bool(val);
    else if (strcmp(key, "time.tz_offset") == 0) g_config.time_tz_offset = parse_int(val);
    else if (strcmp(key, "power.dim_after") == 0)   g_config.power_dim_after = parse_int(val);
    else if (strcmp(key, "power.sleep_after") == 0) g_config.power_sleep_after = parse_int(val);
}

void config_load(const char *path) {
    config_load_defaults();
    vnode_t *v = vfs_lookup(path);
    if (!v || v->type != VN_FILE || v->size == 0) {
        kprintf("config: %s not found, using defaults\n", path);
        return;
    }
    const char *p = (const char *)v->data;
    const char *end = p + v->size;
    char key[CONFIG_STR_LEN], val[256];
    while (p < end) {
        /* skip whitespace + comments */
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && (*p == '#' || *p == ';' || *p == '\n')) {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }
        int ki = 0;
        while (p < end && *p != '=' && *p != '\n' && ki < CONFIG_STR_LEN - 1) {
            if (*p != ' ' && *p != '\t') key[ki++] = *p;
            p++;
        }
        key[ki] = 0;
        if (p < end && *p == '=') p++;
        int vi = 0;
        while (p < end && *p != '\n' && vi < (int)sizeof val - 1) val[vi++] = *p++;
        val[vi] = 0;
        if (p < end) p++;
        if (ki && val[0]) apply(key, val);
    }
    kprintf("config: loaded %s (%d pinned apps)\n", path, g_config.dock_pinned_count);
}

/* ---- saver ---- */
int config_save(const char *path) {
    char buf[2048];
    int n = 0;
    n += snprintf(buf + n, sizeof buf - n, "# /etc/yart.conf - written by Yart\n");
    n += snprintf(buf + n, sizeof buf - n, "hostname=%s\n", g_config.hostname);
    n += snprintf(buf + n, sizeof buf - n, "theme=%s\n", g_config.theme);
    n += snprintf(buf + n, sizeof buf - n, "accent=#%06lX\n", (unsigned long)(g_config.accent & 0xFFFFFFu));
    n += snprintf(buf + n, sizeof buf - n, "border=#%06lX\n", (unsigned long)(g_config.border & 0xFFFFFFu));
    n += snprintf(buf + n, sizeof buf - n, "corner_radius=%d\n", g_config.corner_radius);
    n += snprintf(buf + n, sizeof buf - n, "dock.auto_hide=%d\n", g_config.dock_auto_hide);
    n += snprintf(buf + n, sizeof buf - n, "dock.position=%s\n", g_config.dock_position);
    n += snprintf(buf + n, sizeof buf - n, "dock.icon_size=%d\n", g_config.dock_icon_size);
    n += snprintf(buf + n, sizeof buf - n, "dock.spacing=%d\n", g_config.dock_spacing);
    n += snprintf(buf + n, sizeof buf - n, "dock.pinned=");
    for (int i = 0; i < g_config.dock_pinned_count; i++)
        n += snprintf(buf + n, sizeof buf - n, "%s%s", i ? "," : "", g_config.dock_pinned[i]);
    n += snprintf(buf + n, sizeof buf - n, "\n");
    n += snprintf(buf + n, sizeof buf - n, "topbar.height=%d\n", g_config.topbar_height);
    n += snprintf(buf + n, sizeof buf - n, "wallpaper.mode=%s\n", g_config.wallpaper_mode);
    n += snprintf(buf + n, sizeof buf - n, "wallpaper.path=%s\n", g_config.wallpaper_path);
    n += snprintf(buf + n, sizeof buf - n, "font.system=%s\n", g_config.font_system);
    n += snprintf(buf + n, sizeof buf - n, "font.terminal=%s\n", g_config.font_terminal);
    n += snprintf(buf + n, sizeof buf - n, "display.fps=%d\n", g_config.display_fps);
    n += snprintf(buf + n, sizeof buf - n, "display.night_light=%d\n", g_config.display_night_light);
    n += snprintf(buf + n, sizeof buf - n, "time.format24=%d\n", g_config.time_format24);

    vnode_t *v = vfs_lookup(path);
    if (!v) {
        if (vfs_mkdir_p("/etc") < 0) return -1;
        vnode_t *parent = vfs_lookup("/etc");
        v = vfs_create(parent, "yart.conf", VN_FILE);
        if (!v) return -1;
    }
    vfs_truncate(v, 0);
    return vfs_write(v, buf, 0, n);
}

/* ---- pin helpers ---- */
bool config_is_pinned(const char *name) {
    for (int i = 0; i < g_config.dock_pinned_count; i++)
        if (strcmp(g_config.dock_pinned[i], name) == 0) return true;
    return false;
}

void config_pin(const char *name) {
    if (config_is_pinned(name)) return;
    if (g_config.dock_pinned_count >= CONFIG_MAX_PINNED) return;
    strncpy(g_config.dock_pinned[g_config.dock_pinned_count], name, CONFIG_STR_LEN-1);
    g_config.dock_pinned_count++;
    config_save("/etc/yart.conf");
}

void config_unpin(const char *name) {
    int j = 0;
    for (int i = 0; i < g_config.dock_pinned_count; i++) {
        if (strcmp(g_config.dock_pinned[i], name) == 0) continue;
        if (j != i) memcpy(g_config.dock_pinned[j], g_config.dock_pinned[i], CONFIG_STR_LEN);
        j++;
    }
    g_config.dock_pinned_count = j;
    config_save("/etc/yart.conf");
}
