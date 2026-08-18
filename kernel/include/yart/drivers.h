#pragma once
#include <yart/types.h>

/* PS/2 keyboard */
void kbd_init(void);
/* USB-HID keyboard hook: push a key event into the shared input fanout. */
int  kbd_enqueue(u8 scancode, u8 ascii, u32 flags);
bool kbd_ctrl_held(void);
bool kbd_shift_held(void);
bool kbd_alt_held(void);

/* Bits in the kbd event word (lower 8 are ascii or a YK_* code). */
#define KEY_RELEASE (1<<16)
#define KEY_SHIFT   (1<<17)
#define KEY_CTRL    (1<<18)
#define KEY_ALT     (1<<19)
#define KEY_EXT     (1<<20)   /* E0-prefixed extended key (arrows, nav...) */

/* YART virtual keycodes carried in the ASCII field of E0 events. */
#define YK_UP    0xE1
#define YK_DOWN  0xE2
#define YK_LEFT  0xE3
#define YK_RIGHT 0xE4
#define YK_HOME  0xE5
#define YK_END   0xE6
#define YK_PGUP  0xE7
#define YK_PGDN  0xE8
#define YK_DEL   0xE9
#define YK_INS   0xEA

/* PS/2 mouse */
void mouse_init(void);
/* Absolute cursor position tracked by the driver (framebuffer pixels).
 * Ring-3 apps use SYS_MOUSE_POS to sync their local pointer on focus. */
void mouse_get_pos(int *x, int *y);
typedef struct {
    int dx, dy;
    u8  buttons;       /* bit0=left, bit1=right, bit2=middle */
    int wheel;         /* vertical scroll delta (-1 / 0 / +1) */
    bool valid;
} mouse_event_t;
