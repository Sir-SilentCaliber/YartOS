/* Yart OS - 8259 PIC */
#include <yart/hal.h>
#include <yart/io.h>

#define PIC1 0x20
#define PIC2 0xA0
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define ICW1_INIT 0x11
#define ICW4_8086 0x01

void pic_remap(int o1, int o2) {
    u8 a1 = inb(PIC1_DATA), a2 = inb(PIC2_DATA);
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DATA, o1);       io_wait();
    outb(PIC2_DATA, o2);       io_wait();
    outb(PIC1_DATA, 1 << 2);   io_wait();   /* slave at IRQ2 */
    outb(PIC2_DATA, 2);        io_wait();
    outb(PIC1_DATA, ICW4_8086);io_wait();
    outb(PIC2_DATA, ICW4_8086);io_wait();
    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void pic_mask(u8 irq) {
    if (apic_active()) return;      /* IOAPIC owns the IRQ now */
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_unmask(u8 irq) {
    if (apic_active()) return;      /* IOAPIC owns the IRQ now */
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}

void pic_eoi(u8 irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* EOI with spurious-interrupt detection.  IRQ7/IRQ15 can be raised by the
 * PIC itself with no device behind them; EOIng those incorrectly wedges the
 * in-service register.  Check the ISR register first. */
void pic_eoi_careful(u8 irq) {
    if (irq == 7) {
        if (!(inb(PIC1_CMD) & 0x80)) return;      /* spurious: no EOI */
        outb(PIC1_CMD, 0x20);
        return;
    }
    if (irq == 15) {
        if (!(inb(PIC2_CMD) & 0x80)) {            /* spurious IRQ15 */
            outb(PIC1_CMD, 0x20);                 /* EOI master only  */
            return;
        }
        outb(PIC2_CMD, 0x20);
        outb(PIC1_CMD, 0x20);
        return;
    }
    pic_eoi(irq);
}

/* Stop the legacy 8259s from delivering anything (used once the APIC path
 * is live; also a clean "shut it down" for power-off). */
void pic_disable_all(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
