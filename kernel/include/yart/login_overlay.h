#pragma once

void login_overlay_init(void);

/* called from desktop_render when overlays are visible */
void draw_login_screen(void);
void draw_setup_wizard(void);
void draw_system_menu(void);

/* input handlers for overlays; return true if consumed */
bool login_overlay_handle_key(int sc, char ch, u32 mods);
bool login_overlay_handle_mouse(int px, int py, bool left_click);

/* idle check -> logout */
void login_overlay_tick(void);
