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
    bool is_modifier = false;
    switch (code) {
    case 0x2A: case 0x36: shift = !released; is_modifier = true; break;
    case 0x1D:            ctrl  = !released; is_modifier = true; break;
    case 0x38:            alt   = !released; is_modifier = true; break;
    case 0x3A: if (!released) caps = !caps;  is_modifier = true; break;
    default:
        break;
    }
    char ascii = 0;
    /* Modifier keys emit NO character: their scancodes (0x2A/0x36/0x1D/
     * 0x38/0x3A) land inside the normal map and would otherwise produce a
     * spurious '~'/'?'/'\\n' before every shifted keypress, corrupting any
     * typed uppercase text or shifted symbol ($, >, |, ...). */
    if (!is_modifier) {
        if (e0_pending) {
            if (code < 128 && e0_vk[code]) ascii = (char)e0_vk[code];
        } else if (code < 128) {
            bool up = shift ^ caps;
            ascii = up ? map_upper[code] : map_lower[code];
        }
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

static u8 kbd_cmd_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(KBD_STAT) & 1) break;
    return inb(KBD_DATA);
}
static void kbd_cmd_write(u8 cmd) {
    for (int i = 0; i < 100000; i++) if (!(inb(KBD_STAT) & 2)) break;
    outb(0x64, cmd);
}
static void kbd_data_write(u8 v) {
    for (int i = 0; i < 100000; i++) if (!(inb(KBD_STAT) & 2)) break;
    outb(KBD_DATA, v);
}

void kbd_init(void) {
    irq_register(32 + 1, kbd_irq);
    /* Re-enable the 8042 keyboard: UEFI firmware can leave it disabled. */
    kbd_cmd_write(0x80);                    /* read command byte (will be 0x20 fix below) */
    kbd_cmd_write(0x20);
    u8 cmdb = kbd_cmd_read();
    cmdb |= 0x01;
    cmdb &= ~0x10;
    kbd_cmd_write(0x60);
    kbd_data_write(cmdb);
    (void)kbd_cmd_read();
    /* Do NOT unmask the 8259 PIC here: once the IOAPIC is online the same
     * IRQ1 is delivered via IOAPIC vector 33, and unmasking the PIC makes
     * QEMU deliver the key twice (once per controller). The PIC-fallback
     * path in apic_init() unmasks IRQ1 explicitly if it stays on the 8259. */
    if (!apic_available()) pic_unmask(1);
    while (inb(KBD_STAT) & 1) (void)inb(KBD_DATA);
}

/* USB-HID keyboard input hook (row 18): route through the same fanout as
 * the PS/2 driver so focused apps and the wm each get their own copy. */
static u16 hid_to_set1(u8 usage) {
    static const u8 t[128] = {
        [0x04]=0x1E,[0x05]=0x30,[0x06]=0x2E,[0x07]=0x20,
        [0x08]=0x12,[0x09]=0x21,[0x0A]=0x22,[0x0B]=0x23,
        [0x0C]=0x17,[0x0D]=0x24,[0x0E]=0x25,[0x0F]=0x26,
        [0x10]=0x32,[0x11]=0x31,[0x12]=0x18,[0x13]=0x19,
        [0x14]=0x10,[0x15]=0x13,[0x16]=0x1F,[0x17]=0x14,
        [0x18]=0x16,[0x19]=0x2F,[0x1A]=0x11,[0x1B]=0x2D,
        [0x1C]=0x15,[0x1D]=0x2C,
        [0x1E]=0x02,[0x1F]=0x03,[0x20]=0x04,[0x21]=0x05,
        [0x22]=0x06,[0x23]=0x07,[0x24]=0x08,[0x25]=0x09,
        [0x26]=0x0A,[0x27]=0x0B,
        [0x28]=0x1C,[0x29]=0x01,[0x2A]=0x0E,[0x2B]=0x0F,
        [0x2C]=0x39,
        [0x2D]=0x0C,[0x2E]=0x0D,
        [0x2F]=0x1A,[0x30]=0x1B,[0x31]=0x2B,
        [0x33]=0x27,[0x34]=0x28,[0x35]=0x29,
        [0x36]=0x33,[0x37]=0x34,[0x38]=0x35,
        [0x39]=0x3A,
        [0x3A]=0x3B,[0x3B]=0x3C,[0x3C]=0x3D,[0x3D]=0x3E,
        [0x3E]=0x3F,[0x3F]=0x40,[0x40]=0x41,[0x41]=0x42,
        [0x42]=0x43,[0x43]=0x44,[0x44]=0x57,[0x45]=0x58,
    };
    return usage < 128 ? t[usage] : 0;
}
static u16 hid_to_set1_ext(u8 usage) {
    static const u8 t[128] = {
        [0x49]=0x52,[0x4A]=0x47,[0x4B]=0x49,[0x4C]=0x53,
        [0x4D]=0x4F,[0x4E]=0x51,[0x4F]=0x4D,[0x50]=0x4B,
        [0x51]=0x50,[0x52]=0x48,
    };
    return usage < 128 ? t[usage] : 0;
}

int kbd_enqueue(u8 usage, u8 ascii, u32 flags) {
    (void)ascii;
    static bool usb_caps;
    bool released = flags & KEY_RELEASE;
    bool shift    = flags & KEY_SHIFT;
    u16 s1 = hid_to_set1(usage);
    bool ext = false;
    if (!s1) { s1 = hid_to_set1_ext(usage); ext = s1 != 0; }
    if (!s1) return -1;
    if (!ext && usage == 0x39 && !released) usb_caps = !usb_caps;
    u8 code = (u8)s1;
    char a = 0;
    if (ext) {
        if (code < 128 && e0_vk[code]) a = (char)e0_vk[code];
    } else if (code < 128) {
        a = (shift ^ usb_caps) ? map_upper[code] : map_lower[code];
    }
    int ev = ((int)code << 8) | (u8)a | (int)flags;
    if (ext) ev |= KEY_EXT;
    sys_input_kbd(ev);
    return 0;
}
