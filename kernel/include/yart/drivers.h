#pragma once
#include <yart/types.h>

/* PS/2 keyboard */
void kbd_init(void);
/* returns 0 if no key. Lower 8 bits = ASCII (or 0), bits 8..15 = scancode,
 * bit 16 = release flag */
int  kbd_poll_event(void);
bool kbd_ctrl_held(void);
bool kbd_shift_held(void);
bool kbd_alt_held(void);

/* Bits in the kbd event word (lower 8 are ascii). */
#define KEY_RELEASE (1<<16)
#define KEY_SHIFT   (1<<17)
#define KEY_CTRL    (1<<18)
#define KEY_ALT     (1<<19)

/* PS/2 mouse */
void mouse_init(void);
typedef struct {
    int dx, dy;
    u8  buttons;       /* bit0=left, bit1=right, bit2=middle */
    bool valid;
} mouse_event_t;
bool mouse_poll(mouse_event_t *out);
