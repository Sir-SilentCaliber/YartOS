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
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_unmask(u8 irq) {
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}

void pic_eoi(u8 irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
