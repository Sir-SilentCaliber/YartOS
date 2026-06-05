/* Yart OS - user/session management. */
#include <yart/session.h>
#include <yart/string.h>
#include <yart/fs.h>
#include <yart/hal.h>
#include <yart/config.h>
#include <yart/console.h>

yart_session_t g_session;

void session_hash_password(const char *password, char *out_hash, int out_len) {
    unsigned long hash = 5381;
    int c;
    const char *p = password;
    while ((c = *p++)) hash = ((hash << 5) + hash) + c;
    static const char hex[] = "0123456789abcdef";
    int max = (out_len - 1) < 16 ? (out_len - 1) : 16;
    for (int i = 0; i < max; i++) {
        out_hash[i] = hex[(hash >> (i * 4)) & 0xF];
    }
    out_hash[max] = 0;
}

void session_init(void) {
    memset(&g_session, 0, sizeof g_session);
    g_session.current_user = -1;
    g_session.logged_in = false;
    g_session.login_screen_visible = true;
    g_session.setup_wizard_visible = false;
    g_session.system_menu_visible = false;
    g_session.idle_limit_ticks = (u64)g_config.power_sleep_after * 100;
    g_session.volume = 60;
    g_session.brightness = 80;
    strncpy(g_session.power_mode, "Balanced", sizeof g_session.power_mode);

    session_load_users();

    if (g_session.user_count == 0) {
        session_create_user("demo", "demo", true);
    }
    session_login(0);

    session_input_activity();
}

void session_load_users(void) {
    g_session.user_count = 0;
    vnode_t *v = vfs_lookup("/etc/users.conf");
    if (!v || v->type != VN_FILE || v->size == 0) return;

    const char *p = (const char *)v->data;
    const char *end = p + v->size;

    while (p < end && g_session.user_count < MAX_USERS) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        if (p >= end) break;

        char line[256];
        int li = 0;
        while (p < end && *p != '\n' && li < 255) line[li++] = *p++;
        line[li] = 0;
        if (p < end) p++;

        if (line[0] == '#') continue;

        char *fields[4] = {0};
        int fi = 0;
        fields[fi++] = line;
        for (int i = 0; i < li && fi < 4; i++) {
            if (line[i] == ':') { line[i] = 0; if (fi < 4) fields[fi++] = &line[i+1]; }
        }

        if (fi < 3) continue;
        yart_user_t *u = &g_session.users[g_session.user_count++];
        strncpy(u->username, fields[0], USER_NAME_LEN - 1);
        strncpy(u->password_hash, fields[1], USER_PASS_LEN - 1);
        strncpy(u->home, fields[2], USER_HOME_LEN - 1);
        u->is_admin = (fi >= 4 && (fields[3][0] == '1' || fields[3][0] == 'y' || fields[3][0] == 'Y' || fields[3][0] == 't' || fields[3][0] == 'T'));
    }
}

int session_save_users(void) {
    char buf[2048];
    int n = 0;
    n += snprintf(buf + n, sizeof buf - n, "# Yart OS users\n");
    for (int i = 0; i < g_session.user_count; i++) {
        yart_user_t *u = &g_session.users[i];
        n += snprintf(buf + n, sizeof buf - n, "%s:%s:%s:%d\n",
                      u->username, u->password_hash, u->home, u->is_admin ? 1 : 0);
    }

    vnode_t *v = vfs_lookup("/etc/users.conf");
    if (!v) {
        if (vfs_mkdir_p("/etc") < 0) return -1;
        vnode_t *parent = vfs_lookup("/etc");
        v = vfs_create(parent, "users.conf", VN_FILE);
        if (!v) return -1;
    }
    vfs_truncate(v, 0);
    return vfs_write(v, buf, 0, n);
}

int session_create_user(const char *username, const char *password, bool admin) {
    if (g_session.user_count >= MAX_USERS) return -1;
    for (int i = 0; i < g_session.user_count; i++) {
        if (strcmp(g_session.users[i].username, username) == 0) return -1;
    }

    yart_user_t *u = &g_session.users[g_session.user_count++];
    strncpy(u->username, username, USER_NAME_LEN - 1);
    session_hash_password(password, u->password_hash, USER_PASS_LEN);

    char home[USER_HOME_LEN];
    snprintf(home, sizeof home, "/home/%s", username);
    vfs_mkdir_p(home);
    vfs_mkdir_p("/home"); /* ensure parent exists */
    strncpy(u->home, home, USER_HOME_LEN - 1);
    u->is_admin = admin;

    session_save_users();
    return 0;
}

bool session_auth(const char *username, const char *password) {
    char hash[USER_PASS_LEN];
    session_hash_password(password, hash, USER_PASS_LEN);

    for (int i = 0; i < g_session.user_count; i++) {
        if (strcmp(g_session.users[i].username, username) == 0 &&
            strcmp(g_session.users[i].password_hash, hash) == 0) {
            return true;
        }
    }
    return false;
}

void session_login(int idx) {
    if (idx < 0 || idx >= g_session.user_count) return;
    g_session.current_user = idx;
    g_session.logged_in = true;
    g_session.login_screen_visible = false;
    g_session.setup_wizard_visible = false;
    g_session.system_menu_visible = false;
    session_input_activity();
}

void session_logout(void) {
    g_session.logged_in = false;
    g_session.current_user = -1;
    g_session.login_screen_visible = true;
    g_session.system_menu_visible = false;
}

void session_switch_user(void) {
    session_logout();
    g_session.login_screen_visible = true;
}

void session_input_activity(void) {
    g_session.last_input_tick = pit_ticks();
}

bool session_is_idle(void) {
    if (g_session.idle_limit_ticks == 0) return false;
    if (!g_session.logged_in) return false;
    return (pit_ticks() - g_session.last_input_tick) > g_session.idle_limit_ticks;
}
