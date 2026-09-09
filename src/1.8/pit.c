/* pit.c - Driver do PIT (Programmable Interval Timer), canal 0 -> IRQ0.
 *
 * Programa o PIT em 100 Hz. Cada tick incrementa um contador de 64 bits,
 * base do comando `uptime` do shell. Handler chamado pelo pit_isr_stub (boot.s).
 */
#include <stdint.h>

#define PIT_CH0     0x40
#define PIT_CMD     0x43
#define PIT_HZ      1193180u
#define PIT_FREQ    100u        /* ticks por segundo */

extern void pic_unmask_irq(uint8_t irq);
extern void pic_send_eoi(uint8_t irq);

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static volatile uint64_t pit_ticks = 0;

/* Medidor de CPU: TSC calibrado + tempo em hlt */
static uint64_t tsc_per_ms = 3000; /* chute inicial; calibrado no init */
static volatile uint64_t idle_tsc = 0;
static volatile int cpu_pct = 0;

static inline uint64_t rdtsc_now(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* hlt contabilizado: cerca o hlt com TSC (chamado com IF como estiver) */
void cpu_hlt(void)
{
    uint64_t t0 = rdtsc_now();
    __asm__ volatile ("hlt");
    idle_tsc += rdtsc_now() - t0;
}

void pit_handler(void)
{
    pit_ticks++;
    pic_send_eoi(0);
}

void pit_init(void)
{
    uint16_t div = (uint16_t)(PIT_HZ / PIT_FREQ);

    outb(PIT_CMD, 0x36);                    /* canal 0, lo/hi, square wave */
    outb(PIT_CH0, (uint8_t)(div & 0xFF));
    outb(PIT_CH0, (uint8_t)((div >> 8) & 0xFF));

    pic_unmask_irq(0);                      /* libera IRQ0 */
}

uint64_t pit_get_ticks(void); /* definida abaixo */

/* Calibra TSC (chamar com interrupcoes ligadas). */
void tsc_calibrate(void)
{
    uint64_t t0 = pit_ticks;
    uint64_t r0 = rdtsc_now();
    while (pit_ticks < t0 + 10) {
        __asm__ volatile ("hlt");
    }
    {
        uint64_t dt = rdtsc_now() - r0;
        if (dt > 1000) {
            tsc_per_ms = dt / 100; /* 10 ticks = 100ms */
        }
    }
}

/* Atualiza % CPU 1x/segundo; retorna 0-100. */
int cpu_update(void)
{
    static uint64_t last_t = 0;
    static uint64_t last_idle = 0;
    uint64_t now = pit_get_ticks();
    if (now - last_t < 100) {
        return cpu_pct;
    }
    {
        uint64_t dt = now - last_t;
        uint64_t di = idle_tsc - last_idle;
        uint64_t total = dt * 10 * tsc_per_ms; /* ticks->ms->ciclos TSC */
        int idle = 0;
        if (total > 0) {
            idle = (int)((di * 100) / total);
            if (idle > 100) {
                idle = 100;
            }
        }
        cpu_pct = 100 - idle;
        if (cpu_pct < 0) {
            cpu_pct = 0;
        }
        if (cpu_pct > 100) {
            cpu_pct = 100;
        }
        last_t = now;
        last_idle = idle_tsc;
    }
    return cpu_pct;
}

int cpu_percent(void)
{
    return cpu_pct;
}

uint64_t pit_get_ticks(void)
{
    return pit_ticks;
}

uint64_t pit_uptime_sec(void)
{
    return pit_ticks / PIT_FREQ;
}
