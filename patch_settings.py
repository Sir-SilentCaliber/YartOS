import re

with open('kernel/gui/app_settings.c', 'r') as f:
    content = f.read()

# 1. Add TAB_USERS to enum
content = content.replace(
    '    TAB_FONTS, TAB_MOUSE, TAB_DISPLAY, TAB_TIME, TAB_POWER,\n    TAB_COUNT',
    '    TAB_FONTS, TAB_MOUSE, TAB_DISPLAY, TAB_TIME, TAB_POWER, TAB_USERS,\n    TAB_COUNT'
)

# 2. Add Users to tab_names
content = content.replace(
    '    "Appearance", "Dock", "Top Bar", "Wallpaper",\n    "Fonts", "Input", "Display", "Time", "Power"\n};',
    '    "Appearance", "Dock", "Top Bar", "Wallpaper",\n    "Fonts", "Input", "Display", "Time", "Power", "Users"\n};'
)

# 3. Add tab_users function before main paint
new_tab = '''
static void tab_users(window_t *w, int x, int y, int W) {
    (void)w;
    draw_text(x, y, "Registered users:", TH_ACCENT, 0); y += FONT_H + 8;
    if (g_session.user_count == 0) {
        draw_text(x, y, "No users configured.", TH_TEXT_DIM, 0);
        return;
    }
    for (int i = 0; i < g_session.user_count; i++) {
        char buf[128];
        snprintf(buf, sizeof buf, "  %s%s  home: %s",
                 g_session.users[i].username,
                 g_session.users[i].is_admin ? " (admin)" : "",
                 g_session.users[i].home);
        draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H + 4;
    }
    y += 8;
    draw_text(x, y, "Users are stored in /etc/users.conf.", TH_TEXT_MUTED, 0);
}

/* ---- main paint ---- */
'''

content = content.replace(
    '/* ---- main paint ---- */',
    new_tab
)

# 4. Add case in switch
content = content.replace(
    '    case TAB_POWER:      tab_power     (w, cx_, cy_, cW); break;\n    default: break;',
    '    case TAB_POWER:      tab_power     (w, cx_, cy_, cW); break;\n    case TAB_USERS:      tab_users     (w, cx_, cy_, cW); break;\n    default: break;'
)

with open('kernel/gui/app_settings.c', 'w') as f:
    f.write(content)

print("app_settings.c patched successfully")
