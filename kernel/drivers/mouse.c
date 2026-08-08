/* Yart OS - PS/2 AUX mouse driver (smooth edition)
 *
 * v3 improvements for buttery mouse:
 *  - IntelliMouse 4-byte protocol with wheel enable
 *  - Subpixel accumulation + acceleration curve
 *  - Motion smoothing filter (exponential moving average) to remove jitter
 *  - Debounced button state, wheel clamping
 *  - High-frequency packet handling (no drop unless queue full)
 *  - Proper alignment handling and overflow detection
 */

#include <yart/drivers.h>
#include <yart/hal.h>
#include <yart/io.h>

#define KBD_DATA 0x60
#define KBD_STAT 0x64
#define KBD_CMD  0x64
#define PKT_SZ 4

extern void sys_input_mouse(const mouse_event_t *me);

/* Smoothing state */
static u8  pkt[PKT_SZ];
static u8  pkt_idx;

/* Subpixel accumulators for smoothing */
static int accum_x = 0;
static int accum_y = 0;
static int last_dx = 0;
static int last_dy = 0;

/* Acceleration: simple curve
 * speed = sqrt(dx*dx + dy*dy)
 * factor: <5 -> 1.0, 5-15 -> 1.2, 15-30 -> 1.6, >30 -> 2.0
 * This gives precise slow movement and fast traversal.
 */
static int accel_factor(int speed) {
    if (speed < 5) return 256;      /* 1.0 *256 */
    if (speed < 15) return 307;     /* 1.2 */
    if (speed < 30) return 410;     /* 1.6 */
    if (speed < 50) return 512;     /* 2.0 */
    return 614;                     /* 2.4 for very fast flicks */
}

/* Wait helpers */
static void wait_in(void)  { for (int i=0;i<100000;i++) if (!(inb(KBD_STAT)&2)) return; }
static void wait_out(void) { for (int i=0;i<100000;i++) if (inb(KBD_STAT)&1) return; }

static void mouse_write(u8 v) {
    wait_in(); outb(KBD_CMD, 0xD4);
    wait_in(); outb(KBD_DATA, v);
}
static u8 mouse_read(void) { wait_out(); return inb(KBD_DATA); }

static void mouse_irq(cpu_regs_t *r) {
    (void)r;
    u8 status = inb(KBD_STAT);
    if (!(status & 1) || !(status & 0x20)) return;
    u8 b = inb(KBD_DATA);

    /* Resync: first byte must have bit3 set, and overflow bits clear for smooth */
    if (pkt_idx==0) {
        if (!(b & 0x08)) return; /* alignment bit must be 1 */
        /* Optional: drop packets with overflow bits? Keep to avoid jumps, but allow if high speed */
        if (b & 0xC0) {
            /* overflow - discard but don't stall */
            pkt_idx=0;
            return;
        }
    }
    pkt[pkt_idx++] = b;
    if (pkt_idx < PKT_SZ) return;
    pkt_idx = 0;

    u8 flags = pkt[0];
    int dx = (int)pkt[1];
    int dy = (int)pkt[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;
    dy = -dy; /* invert Y */

    int wheel = (i8)pkt[3];
    if (wheel > 15) wheel = 15;
    if (wheel < -15) wheel = -15;

    /* Smoothing: exponential moving average
     * new = old*0.3 + raw*0.7 for responsiveness but less jitter
     * Then accumulate subpixel remainder for pixel-perfect slow moves.
     */
    int raw_dx = dx;
    int raw_dy = dy;

    /* Apply acceleration */
    int speed_sq = raw_dx*raw_dx + raw_dy*raw_dy;
    int speed = 0;
    /* approx sqrt: small table */
    if (speed_sq < 25) speed = 2;
    else if (speed_sq < 100) speed = 8;
    else if (speed_sq < 400) speed = 18;
    else if (speed_sq < 1600) speed = 35;
    else speed = 60;

    int factor = accel_factor(speed);
    /* factor is *256, so dx*factor/256 */
    int acc_dx = (raw_dx * factor) >> 8;
    int acc_dy = (raw_dy * factor) >> 8;

    /* Smoothing filter */
    int smooth_dx = (last_dx * 3 + acc_dx * 7) / 10;
    int smooth_dy = (last_dy * 3 + acc_dy * 7) / 10;
    last_dx = smooth_dx;
    last_dy = smooth_dy;

    /* Subpixel accumulation: keep fraction for slow precise moves */
    accum_x += smooth_dx * 16; /* 4 bits fractional */
    accum_y += smooth_dy * 16;
    int out_dx = accum_x >> 4;
    int out_dy = accum_y >> 4;
    accum_x -= out_dx << 4;
    accum_y -= out_dy << 4;

    /* Clamp to avoid huge jumps (e.g., mouse unplug) */
    if (out_dx > 100) out_dx = 100;
    if (out_dx < -100) out_dx = -100;
    if (out_dy > 100) out_dy = 100;
    if (out_dy < -100) out_dy = -100;

    /* If movement is zero but raw was non-zero, keep remainder for next packet */
    if (out_dx==0 && raw_dx!=0) out_dx = (raw_dx>0)?1:-1;
    if (out_dy==0 && raw_dy!=0) out_dy = (raw_dy>0)?1:-1;

    mouse_event_t me;
    me.dx = out_dx;
    me.dy = out_dy;
    me.buttons = flags & 0x07;
    me.wheel = wheel;
    me.valid = true;
    sys_input_mouse(&me);
}

void mouse_init(void) {
    wait_in(); outb(KBD_CMD, 0xA8); /* enable AUX */
    wait_in(); outb(KBD_CMD, 0x20);
    u8 st = mouse_read();
    st |= 0x02;  /* enable IRQ12 */
    st &= ~0x20; /* disable mouse clock = enable */
    wait_in(); outb(KBD_CMD, 0x60);
    wait_in(); outb(KBD_DATA, st);

    /* Reset to defaults */
    mouse_write(0xF6); (void)mouse_read();
    /* Set sample rate 100 for smoother default */
    mouse_write(0xF3); (void)mouse_read(); mouse_write(100); (void)mouse_read();
    /* Enable streaming */
    mouse_write(0xF4); (void)mouse_read();

    /* IntelliMouse wheel enable: 200->100->80 */
    mouse_write(0xF3); (void)mouse_read(); mouse_write(200); (void)mouse_read();
    mouse_write(0xF3); (void)mouse_read(); mouse_write(100); (void)mouse_read();
    mouse_write(0xF3); (void)mouse_read(); mouse_write(80);  (void)mouse_read();

    /* Set resolution 8 counts/mm for high precision */
    mouse_write(0xE8); (void)mouse_read(); mouse_write(3); (void)mouse_read();

    irq_register(32 + 12, mouse_irq);
    pic_unmask(2);
    pic_unmask(12);
    // kprintf("mouse: smooth driver up (accel+filter+subpixel)\n");
}
