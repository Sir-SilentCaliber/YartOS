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

#define EV_QSZ 256
static volatile int evq[EV_QSZ];
static volatile u32 ev_head, ev_tail;

static bool shift, ctrl, alt, caps;

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

static void enq(int ev) {
    u32 next = (ev_head + 1) % EV_QSZ;
    if (next == ev_tail) return;     /* drop */
    evq[ev_head] = ev;
    ev_head = next;
}

bool kbd_ctrl_held(void)  { return ctrl; }
bool kbd_shift_held(void) { return shift; }
bool kbd_alt_held(void)   { return alt; }

static void kbd_irq(cpu_regs_t *r) {
    (void)r;
    u8 sc = inb(KBD_DATA);
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
    if (code < 128) {
        bool up = shift ^ caps;
        ascii = up ? map_upper[code] : map_lower[code];
    }
    int ev = ((int)code << 8) | (u8)ascii;
    if (released) ev |= (1 << 16);
    if (shift)    ev |= (1 << 17);
    if (ctrl)     ev |= (1 << 18);
    if (alt)      ev |= (1 << 19);
    enq(ev);
}

void kbd_init(void) {
    irq_register(32 + 1, kbd_irq);
    pic_unmask(1);
    while (inb(KBD_STAT) & 1) (void)inb(KBD_DATA);
}

int kbd_poll_event(void) {
    if (ev_head == ev_tail) return 0;
    int ev = evq[ev_tail];
    ev_tail = (ev_tail + 1) % EV_QSZ;
    return ev;
}
