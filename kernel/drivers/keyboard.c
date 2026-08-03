/* Yart OS - PS/2 keyboard driver (set 1 scancodes)
 *
 * Event layout (32-bit, signed):
 *   bits  0..7   ASCII code (or 0)
 *   bits  8..15  raw scancode
 *   bit   16     release flag
 *   bit   17     shift held at the time of this event
 *   bit   18     ctrl  held at the time of this event
 *   bit   19     alt   held at the time of this event
 */
#include <yart/drivers.h>
#include <yart/hal.h>
#include <yart/io.h>

#define KBD_DATA  0x60
#define KBD_STAT  0x64

/* Events are fanned out to per-task queues by the syscall layer
 * (sys_input_kbd) so the wm AND the focused app each get their own copy. */
extern void sys_input_kbd(int ev);

static bool shift, ctrl, alt, caps;
static bool e0_pending;   /* saw the 0xE0 extended prefix */

static const char map_lower[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0
};
static const char map_upper[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',0
};



bool kbd_ctrl_held(void)  { return ctrl; }
bool kbd_shift_held(void) { return shift; }
bool kbd_alt_held(void)   { return alt; }

/* E0-prefixed set-1 scancodes -> YART virtual keycodes (see drivers.h).
 * Cursor arrows, Home/End, PgUp/PgDn, Ins/Del all come through the 0xE0
 * prefix; without this table they produced ascii 0 and did nothing. */
static const u8 e0_vk[128] = {
    [0x47] = YK_HOME, [0x48] = YK_UP,   [0x49] = YK_PGUP,
    [0x4B] = YK_LEFT, [0x4D] = YK_RIGHT,
    [0x4F] = YK_END,  [0x50] = YK_DOWN, [0x51] = YK_PGDN,
    [0x52] = YK_INS,  [0x53] = YK_DEL,
};

static void kbd_irq(cpu_regs_t *r) {
    (void)r;
    u8 sc = inb(KBD_DATA);
    if (sc == 0xE0) { e0_pending = true; return; }   /* extended prefix */
    bool released = sc & 0x80;
    u8 code = sc & 0x7F;
    /* update modifier state but ALSO push the event so apps that want
     * to track modifiers themselves can do it. */
    switch (code) {
    case 0x2A: case 0x36: shift = !released; break;
    case 0x1D:            ctrl  = !released; break;
    case 0x38:            alt   = !released; break;
    case 0x3A: if (!released) caps = !caps;  break;
    default:
        break;
    }
    char ascii = 0;
    if (e0_pending) {
        if (code < 128 && e0_vk[code]) ascii = (char)e0_vk[code];
    } else if (code < 128) {
        bool up = shift ^ caps;
        ascii = up ? map_upper[code] : map_lower[code];
    }
    int ev = ((int)code << 8) | (u8)ascii;
    if (released)   ev |= KEY_RELEASE;
    if (e0_pending) ev |= KEY_EXT;
    if (shift)      ev |= KEY_SHIFT;
    if (ctrl)       ev |= KEY_CTRL;
    if (alt)        ev |= KEY_ALT;
    sys_input_kbd(ev);
    e0_pending = false;
}

void kbd_init(void) {
    irq_register(32 + 1, kbd_irq);
    pic_unmask(1);
    while (inb(KBD_STAT) & 1) (void)inb(KBD_DATA);
}

/* USB-HID keyboard input hook (row 18): route through the same fanout as
 * the PS/2 driver so focused apps and the wm each get their own copy. */
int kbd_enqueue(u8 scancode, u8 ascii, u32 flags) {
    sys_input_kbd((int)(scancode << 8) | ascii | (int)flags);
    return 0;
}
