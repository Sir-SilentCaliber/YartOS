/* Yart OS - login screen, first-boot setup wizard, and system menu. */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/string.h>
#include <yart/session.h>
#include <yart/hal.h>
#include <yart/console.h>

/* ---------- light palette for system menu (photo match) ---------- */
#define SM_BG       0xFFE6E2DC
#define SM_PANEL    0xFFD0CCC6
#define SM_TEXT     0xFF222222
#define SM_TEXT_MU  0xFF666666
#define SM_SEP      0xFFBBBBBB
#define SM_ACCENT   0xFFB85C2A
#define SM_ACCENT_H 0xFFD4723A
#define SM_SLIDER   0xFFAAAAAA
#define SM_FIELD_BG 0xFFF0EDE8

/* ---------- state ---------- */
static char l_username[USER_NAME_LEN];
static int  l_username_len;
static char l_password[64];
static int  l_password_len;
static int  l_active_field; /* 0=username, 1=password */
static bool l_error;
static char l_error_msg[64];

static char s_username[USER_NAME_LEN];
static int  s_username_len;
static char s_password[64];
static int  s_password_len;
static char s_confirm[64];
static int  s_confirm_len;
static int  s_active_field;
static char s_error_msg[64];


/* ---------- helpers ---------- */
static void reset_login_fields(void) {
    l_username_len = l_password_len = 0;
    l_username[0] = l_password[0] = 0;
    l_active_field = 0;
    l_error = false;
    l_error_msg[0] = 0;
}

static void reset_setup_fields(void) {
    s_username_len = s_password_len = s_confirm_len = 0;
    s_username[0] = s_password[0] = s_confirm[0] = 0;
    s_active_field = 0;
    s_error_msg[0] = 0;
}

void login_overlay_init(void) {
    reset_login_fields();
    reset_setup_fields();
}

static void dim_screen(void) {
    const u32 MASK = 0x007F7F7FU;
    for (u32 y = 0; y < g_fb.height; y++) {
        u32 *row = &g_fb.pixels[y * g_fb.pitch_px];
        for (u32 x = 0; x < g_fb.width; x++) {
            row[x] = 0xFF000000U | ((row[x] >> 1) & MASK);
        }
    }
}

static void draw_panel_shadow(int px, int py, int pw, int ph, int radius) {
    for (int s = 1; s <= 8; s++) {
        u32 alpha = 100 - s * 10;
        color_t sc = (alpha << 24) | 0;
        draw_rounded_rect(px + s, py + s, pw, ph, radius, sc);
    }
}

static void draw_input_field(int x, int y, int w, int h, const char *label,
                             const char *text, int text_len, bool active, bool password) {
    draw_text(x, y - FONT_H - 4, label, TH_TEXT, 0);
    color_t bg = active ? 0xFF353D4A : 0xFF2A303A;
    draw_rounded_rect(x, y, w, h, 6, bg);
    draw_rounded_rect_outline(x, y, w, h, 6, active ? TH_ACCENT : 0xFF3A4554);
    char display[128];
    int n = text_len < 127 ? text_len : 127;
    if (password) {
        for (int i = 0; i < n; i++) display[i] = '*';
        display[n] = 0;
    } else {
        memcpy(display, text, n);
        display[n] = 0;
    }
    int px_ = x + 12;
    int py_ = y + (h - FONT_H) / 2;
    draw_text(px_, py_, display, TH_TEXT, 0);
    if (active && ((pit_ticks() / 50) & 1)) {
        int cx = px_ + text_len * FONT_W;
        draw_vline(cx, py_ - 2, FONT_H + 4, TH_ACCENT);
    }
}

static void draw_btn(int x, int y, int w, int h, const char *label, bool hover) {
    color_t bg = hover ? TH_ACCENT_DIM : TH_ACCENT;
    draw_rounded_rect(x, y, w, h, 6, bg);
    int tw = text_width(label);
    draw_text(x + (w - tw) / 2, y + (h - FONT_H) / 2, label, TH_TEXT_INV, 0);
}

static bool btn_hit(int x, int y, int w, int h, int mx, int my, bool click) {
    return click && (mx >= x && mx < x + w && my >= y && my < y + h);
}

/* ---------- login screen ---------- */
void draw_login_screen(void) {
    dim_screen();

    int pw = 400, ph = 300;
    int px = ((int)g_fb.width - pw) / 2;
    int py = ((int)g_fb.height - ph) / 2;
    draw_panel_shadow(px, py, pw, ph, 12);

    draw_rounded_rect(px, py, pw, ph, 12, 0xFF2A303A);
    draw_rounded_rect_outline(px, py, pw, ph, 12, 0xFF3A4554);

    int cx = px + 30;
    int cy = py + 30;

    /* title */
    draw_text(cx, cy, "Welcome back", TH_ACCENT, 0); cy += FONT_H + 20;

    /* username */
    draw_input_field(cx, cy, pw - 60, 32, "Username",
                     l_username, l_username_len, l_active_field == 0, false);
    cy += 48;
    /* password */
    draw_input_field(cx, cy, pw - 60, 32, "Password",
                     l_password, l_password_len, l_active_field == 1, true);
    cy += 56;

    int bw = 120, bh = 32;
    int bx = px + (pw - bw) / 2;
    int by = cy;
    int mx, my; cursor_get_pos(&mx, &my);
    bool hover = (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
    draw_btn(bx, by, bw, bh, "Sign In", hover);
    cy += bh + 16;

    if (l_error && l_error_msg[0]) {
        draw_text(cx, cy, l_error_msg, TH_ERR, 0);
    }

    draw_text(px + (pw - text_width("Press Tab to switch fields")) / 2,
              py + ph - 28, "Press Tab to switch fields", TH_TEXT_MUTED, 0);
}

/* ---------- setup wizard ---------- */
void draw_setup_wizard(void) {
    dim_screen();

    int pw = 420, ph = 400;
    int px = ((int)g_fb.width - pw) / 2;
    int py = ((int)g_fb.height - ph) / 2;
    draw_panel_shadow(px, py, pw, ph, 12);

    draw_rounded_rect(px, py, pw, ph, 12, 0xFF2A303A);
    draw_rounded_rect_outline(px, py, pw, ph, 12, 0xFF3A4554);

    int cx = px + 30;
    int cy = py + 24;

    draw_text(cx, cy, "Welcome to Yart OS", TH_ACCENT, 0); cy += FONT_H + 6;
    draw_text(cx, cy, "Let's create your account.", TH_TEXT_DIM, 0); cy += FONT_H + 20;

    draw_input_field(cx, cy, pw - 60, 30, "Username",
                     s_username, s_username_len, s_active_field == 0, false);
    cy += 46;
    draw_input_field(cx, cy, pw - 60, 30, "Password",
                     s_password, s_password_len, s_active_field == 1, true);
    cy += 46;
    draw_input_field(cx, cy, pw - 60, 30, "Confirm Password",
                     s_confirm, s_confirm_len, s_active_field == 2, true);
    cy += 54;

    int bw = 140, bh = 32;
    int bx = px + (pw - bw) / 2;
    int by = cy;
    int mx, my; cursor_get_pos(&mx, &my);
    bool hover = (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
    draw_btn(bx, by, bw, bh, "Get Started", hover);
    cy += bh + 12;

    if (s_error_msg[0]) {
        draw_text(cx, cy, s_error_msg, TH_ERR, 0);
    }
}

/* ---------- system menu helpers ---------- */
static void sm_draw_slider(int x, int y, int w, int *val, const char *icon) {
    draw_text(x, y, icon, SM_TEXT, 0);
    int tx = x + 24;
    int track_y = y + (FONT_H - 6) / 2 + 2;
    draw_rounded_rect(tx, track_y + 4, w, 6, 3, SM_SLIDER);
    int pos = (*val) * w / 100;
    if (pos > w) pos = w;
    draw_rect(tx, track_y + 4, pos, 6, SM_ACCENT);
    int kx = tx + pos - 6;
    if (kx < tx) kx = tx;
    if (kx > (int)(tx + w - 12)) kx = (int)(tx + w - 12);
    draw_rounded_rect(kx, track_y, 12, 14, 6, 0xFFFFFFFF);
    draw_rounded_rect_outline(kx, track_y, 12, 14, 6, SM_SLIDER);
    char num[8];
    snprintf(num, sizeof num, "%d", *val);
    draw_text(tx + w + 8, y, num, SM_TEXT, 0);
}

static void sm_draw_toggle(int x, int y, int w, int h, const char *label, const char *sublabel, bool on, bool hover) {
    color_t bg = on ? SM_ACCENT : (hover ? 0xFFD0CCC6 : SM_PANEL);
    color_t fg = on ? 0xFFFFFFFF : SM_TEXT;
    draw_rounded_rect(x, y, w, h, h / 2, bg);
    if (hover && !on) draw_rounded_rect_outline(x, y, w, h, h / 2, SM_ACCENT);
    int tw = text_width(label);
    int lines = (sublabel && sublabel[0]) ? 2 : 1;
    int text_h = lines * FONT_H;
    int text_y = y + (h - text_h) / 2;
    draw_text(x + (w - tw) / 2, text_y, label, fg, 0);
    if (lines > 1) { int tw2 = text_width(sublabel); draw_text(x + (w - tw2) / 2, text_y + FONT_H, sublabel, fg, 0); }
}

static void sm_draw_icon_circle(int cx_, int cy_, int r, color_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                draw_pixel(cx_ + dx, cy_ + dy, c);
}

static void sm_draw_power_icon(int x, int y, int size) {
    int cx_ = x + size / 2;
    int cy_ = y + size / 2;
    int r = size / 2 - 2;
    color_t c = SM_TEXT;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d = dx * dx + dy * dy;
            if (d <= r * r && d >= (r - 2) * (r - 2)) {
                /* vertical gap at top for power symbol line */
                if (dy < -(r * 7 / 10) && (dx > -2 && dx < 2)) continue;
                draw_pixel(cx_ + dx, cy_ + dy, c);
            }
        }
    }
    /* vertical line at top */
    draw_vline(cx_, cy_ - r, r * 7 / 10, c);
    draw_vline(cx_ + 1, cy_ - r, r * 7 / 10, c);
}

/* ---------- system menu ---------- */
void draw_system_menu(void) {
    dim_screen();

    int pw = 420, ph = 520;
    int px = ((int)g_fb.width - pw) / 2;
    int py = ((int)g_fb.height - ph) / 2;
    draw_panel_shadow(px, py, pw, ph, 16);

    draw_rounded_rect(px, py, pw, ph, 16, SM_BG);
    draw_rounded_rect_outline(px, py, pw, ph, 16, 0xFFC8C4BE);

    int mx, my; cursor_get_pos(&mx, &my);
    
    int cx = px + 24;
    int cy = py + 24;

    /* header: power icon + title */
    sm_draw_power_icon(cx, cy, 28);
    draw_text(cx + 36, cy + 6, "Power Off", SM_TEXT, 0);
    cy += 36;

    /* list items */
    const char *items[] = { "Suspend", "Restart...", "Power Off..." };
    int nitems = 3;
    for (int i = 0; i < nitems; i++) {
        bool hover = (mx >= cx - 8 && mx < cx + pw - 32 && my >= cy && my < cy + 28);
        if (hover) {
            draw_rounded_rect(cx - 8, cy, pw - 32, 28, 6, SM_PANEL);
            
        }
        draw_text(cx, cy + 6, items[i], SM_TEXT, 0);
        cy += 32;
    }

    /* separator */
    draw_hline(cx, cy, pw - 48, SM_SEP);
    cy += 12;

    const char *items2[] = { "Log Out...", "Switch User..." };
    int nitems2 = 2;
    for (int i = 0; i < nitems2; i++) {
        bool hover = (mx >= cx - 8 && mx < cx + pw - 32 && my >= cy && my < cy + 28);
        if (hover) {
            draw_rounded_rect(cx - 8, cy, pw - 32, 28, 6, SM_PANEL);
            
        }
        draw_text(cx, cy + 6, items2[i], SM_TEXT, 0);
        cy += 32;
    }

    cy += 12;

    /* volume */
    sm_draw_slider(cx, cy, pw - 100, &g_session.volume, "[V]");
    cy += 36;
    /* brightness */
    sm_draw_slider(cx, cy, pw - 100, &g_session.brightness, "[*]");
    cy += 40;

    /* separator */
    draw_hline(cx, cy, pw - 48, SM_SEP);
    cy += 16;

    /* toggle grid: 2 columns */
    int col_w = (pw - 60) / 2;
    int gap = 12;
    int toggle_h = 34;
    const char *tlabels[] = { "Wi-Fi", "Bluetooth", "Power Mode",
                              "Night Light", "Dark Style",
                              "Airplane Mode", "Auto Rotate" };
    bool tstates[] = { g_session.wifi_on, g_session.bluetooth_on, false,
                       g_session.night_light_on, g_session.dark_style_on,
                       g_session.airplane_mode_on, g_session.auto_rotate_on };
    int nt = 7;
    int col = 0;
    int tgy = cy;
    for (int i = 0; i < nt; i++) {
        int tx = cx + col * (col_w + gap);
        bool hover = (mx >= tx && mx < tx + col_w && my >= tgy && my < tgy + toggle_h);
        
        const char *subl = NULL;
        if (i == 0 && g_session.wifi_on) subl = "amito2g";
        else if (i == 2) subl = g_session.power_mode;
        sm_draw_toggle(tx, tgy, col_w, toggle_h, tlabels[i], subl, tstates[i], hover);
        col++;
        if (col >= 2) { col = 0; tgy += toggle_h + gap; }
    }
}

/* ---------- input handling ---------- */
bool login_overlay_handle_key(int sc, char ch, u32 mods) {
    (void)mods;

    if (g_session.setup_wizard_visible) {
        char *fields[3] = { s_username, s_password, s_confirm };
        int  *lens[3]   = { &s_username_len, &s_password_len, &s_confirm_len };
        int   maxlens[3] = { USER_NAME_LEN - 1, 63, 63 };

        if (sc == 0x0F) {
            s_active_field = (s_active_field + 1) % 3;
            return true;
        }
        if (ch == '\n' || sc == 0x1C) {
            if (s_username_len == 0) {
                strncpy(s_error_msg, "Please enter a username.", sizeof s_error_msg);
                return true;
            }
            if (s_password_len == 0) {
                strncpy(s_error_msg, "Please enter a password.", sizeof s_error_msg);
                return true;
            }
            if (strcmp(s_password, s_confirm) != 0) {
                strncpy(s_error_msg, "Passwords do not match.", sizeof s_error_msg);
                return true;
            }
            if (session_create_user(s_username, s_password, true) < 0) {
                strncpy(s_error_msg, "Failed to create account.", sizeof s_error_msg);
                return true;
            }
            session_login(0);
            reset_setup_fields();
            return true;
        }
        if (ch == '\b' || ch == 8) {
            if (*lens[s_active_field] > 0) {
                fields[s_active_field][--(*lens[s_active_field])] = 0;
            }
            return true;
        }
        if (ch >= ' ' && ch < 127) {
            int idx = s_active_field;
            if (*lens[idx] < maxlens[idx]) {
                fields[idx][(*lens[idx])++] = ch;
                fields[idx][*lens[idx]] = 0;
            }
            return true;
        }
        return true; /* consume all in wizard */
    }

    if (g_session.login_screen_visible) {
        char *fields[2] = { l_username, l_password };
        int  *lens[2]   = { &l_username_len, &l_password_len };
        int   maxlens[2] = { USER_NAME_LEN - 1, 63 };

        if (sc == 0x0F) {
            l_active_field = (l_active_field + 1) % 2;
            l_error = false;
            return true;
        }
        if (ch == '\n' || sc == 0x1C) {
            if (l_username_len == 0 || l_password_len == 0) {
                l_error = true;
                strncpy(l_error_msg, "Please enter username and password.", sizeof l_error_msg);
                return true;
            }
            if (session_auth(l_username, l_password)) {
                for (int i = 0; i < g_session.user_count; i++) {
                    if (strcmp(g_session.users[i].username, l_username) == 0) {
                        session_login(i);
                        reset_login_fields();
                        return true;
                    }
                }
            }
            l_error = true;
            strncpy(l_error_msg, "Invalid username or password.", sizeof l_error_msg);
            return true;
        }
        if (ch == '\b' || ch == 8) {
            if (*lens[l_active_field] > 0) {
                fields[l_active_field][--(*lens[l_active_field])] = 0;
            }
            return true;
        }
        if (ch >= ' ' && ch < 127) {
            int idx = l_active_field;
            if (*lens[idx] < maxlens[idx]) {
                fields[idx][(*lens[idx])++] = ch;
                fields[idx][*lens[idx]] = 0;
            }
            return true;
        }
        return true; /* consume all in login */
    }

    if (g_session.system_menu_visible) {
        if (sc == 0x01 || ch == 27) {
            g_session.system_menu_visible = false;
            return true;
        }
        return true;
    }

    return false;
}

static bool handle_system_menu_click(int px, int py) {
    int pw = 420, ph = 520;
    int x0 = ((int)g_fb.width - pw) / 2;
    int y0 = ((int)g_fb.height - ph) / 2;
    int cx = x0 + 24;
    int cy = y0 + 24 + 36;

    /* list items: Suspend, Restart, Power Off */
    for (int i = 0; i < 3; i++) {
        if (px >= cx - 8 && px < cx + pw - 32 && py >= cy && py < cy + 28) {
            if (i == 2) {
                /* Power Off - just a toast for now */
                extern void toast(const char *, ...);
                toast("Shutting down... (mock)");
            }
            g_session.system_menu_visible = false;
            return true;
        }
        cy += 32;
    }

    cy += 12; /* after separator */
    /* Log Out, Switch User */
    for (int i = 0; i < 2; i++) {
        if (px >= cx - 8 && px < cx + pw - 32 && py >= cy && py < cy + 28) {
            if (i == 0) session_logout();
            else session_switch_user();
            g_session.system_menu_visible = false;
            return true;
        }
        cy += 32;
    }

    cy += 36 + 40 + 16; /* after sliders + sep */

    /* toggle grid */
    int col_w = (pw - 60) / 2;
    int gap = 12;
    int toggle_h = 34;
    int nt = 7;
    int col = 0;
    int tgy = cy;
    for (int i = 0; i < nt; i++) {
        int tx = cx + col * (col_w + gap);
        if (px >= tx && px < tx + col_w && py >= tgy && py < tgy + toggle_h) {
            switch (i) {
            case 0: g_session.wifi_on = !g_session.wifi_on; break;
            case 1: g_session.bluetooth_on = !g_session.bluetooth_on; break;
            case 2:
                /* cycle power mode */
                if (strcmp(g_session.power_mode, "Balanced") == 0)
                    strncpy(g_session.power_mode, "Performance", sizeof g_session.power_mode);
                else if (strcmp(g_session.power_mode, "Performance") == 0)
                    strncpy(g_session.power_mode, "Power Saver", sizeof g_session.power_mode);
                else
                    strncpy(g_session.power_mode, "Balanced", sizeof g_session.power_mode);
                break;
            case 3: g_session.night_light_on = !g_session.night_light_on; break;
            case 4: g_session.dark_style_on = !g_session.dark_style_on; break;
            case 5: g_session.airplane_mode_on = !g_session.airplane_mode_on; break;
            case 6: g_session.auto_rotate_on = !g_session.auto_rotate_on; break;
            }
            return true;
        }
        col++;
        if (col >= 2) { col = 0; tgy += toggle_h + gap; }
    }

    /* click outside -> close */
    g_session.system_menu_visible = false;
    return true;
}

bool login_overlay_handle_mouse(int px, int py, bool left_click) {
    if (g_session.setup_wizard_visible) {
        int pw = 420, ph = 400;
        int x0 = ((int)g_fb.width - pw) / 2, y0 = ((int)g_fb.height - ph) / 2;
        int cx = x0 + 30;
        int fy = y0 + 24 + FONT_H + 6 + FONT_H + 20;
        for (int i = 0; i < 3; i++) {
            if (px >= cx && px < cx + pw - 60 && py >= fy && py < fy + 30) {
                s_active_field = i;
                return true;
            }
            fy += 46;
        }
        int bw = 140, bh = 32;
        int bx = x0 + (pw - bw) / 2;
        int by = fy + 8;
        if (btn_hit(bx, by, bw, bh, px, py, left_click)) {
            login_overlay_handle_key(0x1C, '\n', 0);
            return true;
        }
        return true; /* consume */
    }

    if (g_session.login_screen_visible) {
        int pw = 400, ph = 300;
        int x0 = ((int)g_fb.width - pw) / 2, y0 = ((int)g_fb.height - ph) / 2;
        int cx = x0 + 30;
        int fy = y0 + 30 + FONT_H + 20;
        for (int i = 0; i < 2; i++) {
            if (px >= cx && px < cx + pw - 60 && py >= fy && py < fy + 32) {
                l_active_field = i;
                l_error = false;
                return true;
            }
            fy += 48;
        }
        int bw = 120, bh = 32;
        int bx = x0 + (pw - bw) / 2;
        int by = fy + 8;
        if (btn_hit(bx, by, bw, bh, px, py, left_click)) {
            login_overlay_handle_key(0x1C, '\n', 0);
            return true;
        }
        return true;
    }

    if (g_session.system_menu_visible) {
        if (!left_click) return true; /* consume hover */
        return handle_system_menu_click(px, py);
    }

    return false;
}

void login_overlay_tick(void) {
    if (!g_session.logged_in) return;
    if (session_is_idle()) {
        session_logout();
    }
}
