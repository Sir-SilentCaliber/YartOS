/* Yart OS - PS/2 AUX (mouse) driver.
 *
 * Speaks the IntelliMouse 4-byte protocol (standard 3-byte packets plus a
 * wheel byte).  The wheel is enabled by the 200/100/80 sample-rate trick.
 */
#include <yart/drivers.h>
#include <yart/hal.h>
#include <yart/io.h>

#define KBD_DATA 0x60
#define KBD_STAT 0x64
#define KBD_CMD  0x64

#define PKT_SZ 4

/* Completed packets are fanned out to per-task queues (wm + focused app)
 * by the syscall layer - see sys_input_mouse(). */
extern void sys_input_mouse(const mouse_event_t *me);

static u8  pkt[PKT_SZ];
static u8  pkt_idx;

static void wait_in(void)  { for (int i = 0; i < 100000; i++) if (!(inb(KBD_STAT) & 2)) return; }
static void wait_out(void) { for (int i = 0; i < 100000; i++) if   (inb(KBD_STAT) & 1)  return; }

static void mouse_write(u8 v) {
    wait_in(); outb(KBD_CMD,  0xD4);
    wait_in(); outb(KBD_DATA, v);
}

static u8 mouse_read(void) { wait_out(); return inb(KBD_DATA); }

static void mouse_irq(cpu_regs_t *r) {
    (void)r;
    u8 status = inb(KBD_STAT);
    if (!(status & 1) || !(status & 0x20)) return;
    u8 b = inb(KBD_DATA);
    if (pkt_idx == 0 && !(b & 0x08)) return;   /* alignment bit */
    pkt[pkt_idx++] = b;
    if (pkt_idx == PKT_SZ) {
        pkt_idx = 0;
        u8 flags = pkt[0];
        mouse_event_t me;
        me.dx = pkt[1];
        me.dy = pkt[2];
        if (flags & 0x10) me.dx -= 256;
        if (flags & 0x20) me.dy -= 256;
        me.dy = -me.dy;        /* invert: PS/2 Y goes up */
        me.buttons = flags & 0x07;
        me.wheel   = (i8)pkt[3];   /* wheel byte, -1..+1 typically */
        me.valid = true;
        sys_input_mouse(&me);
    }
}

void mouse_init(void) {
    /* enable AUX */
    wait_in(); outb(KBD_CMD, 0xA8);
    /* enable IRQ12 in compaq status */
    wait_in(); outb(KBD_CMD, 0x20);
    u8 st = mouse_read();
    st |= 0x02;
    st &= ~0x20;
    wait_in(); outb(KBD_CMD, 0x60);
    wait_in(); outb(KBD_DATA, st);
    /* defaults + enable streaming */
    mouse_write(0xF6); (void)mouse_read();
    mouse_write(0xF4); (void)mouse_read();
    /* IntelliMouse wheel enable: 200 -> 100 -> 80 sample rate */
    mouse_write(0xF3); (void)mouse_read(); mouse_write(200); (void)mouse_read();
    mouse_write(0xF3); (void)mouse_read(); mouse_write(100); (void)mouse_read();
    mouse_write(0xF3); (void)mouse_read(); mouse_write(80);  (void)mouse_read();

    irq_register(32 + 12, mouse_irq);
    pic_unmask(2);
    pic_unmask(12);
}
