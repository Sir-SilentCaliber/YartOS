/* Yart OS - CMOS RTC reader */
#include <yart/hal.h>
#include <yart/io.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static u8 bcd2bin(u8 v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_read(rtc_time_t *t) {
    while (cmos_read(0x0A) & 0x80) { /* update in progress */ }
    u8 sec = cmos_read(0x00);
    u8 min = cmos_read(0x02);
    u8 hr  = cmos_read(0x04);
    u8 day = cmos_read(0x07);
    u8 mon = cmos_read(0x08);
    u8 yr  = cmos_read(0x09);
    u8 cen = cmos_read(0x32);
    u8 status = cmos_read(0x0B);
    if (!(status & 0x04)) {
        sec = bcd2bin(sec); min = bcd2bin(min);
        hr  = bcd2bin(hr & 0x7F) | (hr & 0x80);
        day = bcd2bin(day); mon = bcd2bin(mon);
        yr  = bcd2bin(yr);  cen = bcd2bin(cen);
    }
    t->second = sec; t->minute = min; t->hour = hr & 0x7F;
    t->day = day; t->month = mon;
    t->year = (cen ? cen * 100 : 2000) + yr;
}
