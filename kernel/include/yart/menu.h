/*
 * Yart OS - global context menu + toast system.
 *
 * One menu can be visible at a time.  It is opened by right-click and
 * dismissed by clicking anywhere (or selecting an entry).  Toasts stack
 * in the bottom-right corner and auto-dismiss after ~2.5 s.
 */
#pragma once
#include <yart/types.h>

#define MENU_MAX_ITEMS 12
#define MENU_LABEL_LEN 32

typedef struct menu_item {
    char  label[MENU_LABEL_LEN];
    void (*on_click)(void *ud);
    void  *ud;
    bool  separator;
    bool  disabled;
} menu_item_t;

void menu_open(int x, int y, menu_item_t *items, int n);
void menu_close(void);
bool menu_is_open(void);
void menu_render(void);
bool menu_handle_click(int x, int y, bool down);     /* true if consumed */

/* toasts */
void toast(const char *fmt, ...);
void toasts_render(u64 now_ms);
bool toasts_active(void);
