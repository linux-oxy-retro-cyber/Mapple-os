/* pcspk.c - Driver PC Speaker (buzzer via PIT canal 2 + porta 0x61).
 *
 * Audio real (onda quadrada): beep(freq_hz, ms) e sequencias.
 * Placas HDA/AC97 (DMA+codecs) ficam como roadmap; o speaker e a
 * base honesta e funciona em qualquer PC/QEMU. Saida audivel exige
 * backend de audio no QEMU (ex: -audiodev pa).
 */
#include <stdint.h>

extern uint64_t pit_get_ticks(void);
extern void serial_puts(const char* s);
extern void cpu_hlt(void);

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

static void sleep_ms(uint64_t ms)
{
    uint64_t fim = pit_get_ticks() + (ms * 100 + 999) / 1000;
    if (fim == pit_get_ticks()) {
        fim++;
    }
    while (pit_get_ticks() < fim) {
        cpu_hlt();
    }
}

/* Toca frequencia por ms (0 = silencio/pausa). */
void pcspk_beep(uint32_t hz, uint32_t ms)
{
    if (hz < 20 || hz > 20000) {
        sleep_ms(ms);
        return;
    }
    {
        uint16_t div = (uint16_t)(1193180u / hz);
        uint8_t tmp = inb(0x61);
        outb(0x43, 0xB6); /* canal 2, lo/hi, square wave */
        outb(0x42, (uint8_t)(div & 0xFF));
        outb(0x42, (uint8_t)((div >> 8) & 0xFF));
        outb(0x61, tmp | 0x03); /* liga gate+speaker */
        serial_puts("audio: beep ");
        {
            char b[12];
            int n = 0;
            uint32_t v = hz;
            char tmp2[12];
            int k = 0;
            if (v == 0) {
                tmp2[k++] = '0';
            }
            while (v > 0 && k < 12) {
                tmp2[k++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (k > 0) {
                b[n++] = tmp2[--k];
            }
            b[n] = '\0';
            serial_puts(b);
        }
        serial_puts("Hz\n");
        sleep_ms(ms);
        tmp = inb(0x61);
        outb(0x61, (uint8_t)(tmp & ~0x03)); /* desliga */
    }
}

/* Toca sequencia "freq:ms, freq:ms..." (ex: "440:150,0:50,660:200"). */
void pcspk_play(const char* seq)
{
    while (*seq) {
        uint32_t f = 0, ms = 0;
        while (*seq >= '0' && *seq <= '9') {
            f = f * 10 + (uint32_t)(*seq - '0');
            seq++;
        }
        if (*seq == ':') {
            seq++;
        }
        while (*seq >= '0' && *seq <= '9') {
            ms = ms * 10 + (uint32_t)(*seq - '0');
            seq++;
        }
        if (ms == 0) {
            ms = 150;
        }
        /* f==0 vira pausa dentro do beep */
        pcspk_beep(f, ms);
        if (*seq == ',') {
            seq++;
        } else if (*seq && *seq != ' ') {
            break;
        }
        while (*seq == ' ') {
            seq++;
        }
    }
}
