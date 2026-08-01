#pragma once
#include <yart/types.h>

#define MAX_USERS       8
#define USER_NAME_LEN   32
#define USER_PASS_LEN   96   /* '$'+16-hex-salt+'$'+64-hex-sha256 */
#define USER_HOME_LEN   128

typedef struct {
    char username[USER_NAME_LEN];
    char password_hash[USER_PASS_LEN];
    char home[USER_HOME_LEN];
    bool is_admin;
} yart_user_t;

typedef struct {
    yart_user_t users[MAX_USERS];
    int         user_count;
    int         current_user;      /* -1 = none */
    bool        logged_in;
    u64         last_input_tick;   /* pit_ticks() at last activity */
    u64         idle_limit_ticks;  /* power_sleep_after converted to ticks */
    bool        login_screen_visible;
    bool        setup_wizard_visible;
    bool        system_menu_visible;

    /* quick toggles (runtime state) */
    bool        wifi_on;
    bool        bluetooth_on;
    bool        night_light_on;
    bool        dark_style_on;
    bool        airplane_mode_on;
    bool        auto_rotate_on;
    char        power_mode[16];    /* Balanced | Performance | Power Saver */
    int         volume;            /* 0-100 */
    int         brightness;        /* 0-100 */
} yart_session_t;

extern yart_session_t g_session;

void session_init(void);
void session_load_users(void);
int  session_save_users(void);

int  session_create_user(const char *username, const char *password, bool admin);
bool session_auth(const char *username, const char *password);
void session_login(int idx);
void session_logout(void);
void session_switch_user(void);

void session_input_activity(void);
bool session_is_idle(void);

void session_hash_password(const char *password, const char *salt, char *out_hash, int out_len);
