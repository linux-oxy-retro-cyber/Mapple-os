/* rtc.c - Relogio de tempo real (CMOS/RTC via portas 0x70/0x71).
 *
 * Le data/hora em BCD. Usado pelo shell (date), taskbar e Calendario.
 */
#include <stdint.h>

static inline void outb(uint16_t p, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p));
}

static inline uint8_t inb(uint16_t p)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

static uint8_t rtc_reg(uint8_t reg)
{
    uint8_t v;
    outb(0x70, (uint8_t)(0x80 | reg)); /* 0x80: desliga NMI durante acesso */
    v = inb(0x71);
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); /* BCD -> decimal */
}

void rtc_get(uint8_t* day, uint8_t* mon, uint8_t* year2,
             uint8_t* hh, uint8_t* mm, uint8_t* ss)
{
    if (day) {
        *day = rtc_reg(0x07);
    }
    if (mon) {
        *mon = rtc_reg(0x08);
    }
    if (year2) {
        *year2 = rtc_reg(0x09);
    }
    if (hh) {
        *hh = rtc_reg(0x04);
    }
    if (mm) {
        *mm = rtc_reg(0x02);
    }
    if (ss) {
        *ss = rtc_reg(0x00);
    }
}
