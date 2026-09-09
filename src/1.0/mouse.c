/* mouse.c - Driver de mouse PS/2 (porta auxiliar, IRQ12 -> vetor 0x2C).
 *
 * Inicializa o mouse (defaults + reporting) e monta pacotes de 3 bytes.
 * Expoe posicao, botoes e flag "dirty" para o compositor (gui.c).
 */
#include <stdint.h>
#include <stddef.h>

extern void pic_unmask_irq(uint8_t irq);
extern void pic_send_eoi(uint8_t irq);
extern void serial_puts(const char* s);
extern uint64_t pit_get_ticks(void); /* requer sti ligado */

#define MOUSE_IRQ 12

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* Espera: for_write=1 espera buffer de entrada vazio; =0 espera saida cheia. */
static void mouse_wait(int for_write)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t s = inb(0x64);
        if (for_write && !(s & 0x02)) return;
        if (!for_write && (s & 0x01)) return;
    }
}

/* Espera 1 byte de resposta com timeout real (100ms via PIT). */
static int ack_wait(void)
{
    uint64_t fim = pit_get_ticks() + 10;
    while (pit_get_ticks() < fim) {
        if (inb(0x64) & 0x01) {
            return inb(0x60);
        }
    }
    return -1;
}

/* Envia comando e consome o ACK (0xFA). Retorna 0 ok, -1 falha. */
static int mouse_write_cmd(uint8_t cmd)
{
    for (int tries = 0; tries < 3; tries++) {
        mouse_wait(1);
        outb(0x64, 0xD4);   /* "proximo byte vai p/ mouse" */
        mouse_wait(1);
        outb(0x60, cmd);
        int r = ack_wait();
        if (r == 0xFA) {
            return 0;
        }
    }
    return -1;
}

extern void gui_get_res(uint32_t* w, uint32_t* h);

/* Estado publico (lido pelo gui.c) */
volatile int mouse_x = 512;
volatile int mouse_y = 384;
volatile uint8_t mouse_buttons = 0;
volatile int mouse_dirty = 0;
int mouse_present = 0;

static uint8_t pkt[3];
static int pkt_idx = 0;

void mouse_handler(void)
{
    uint8_t b = inb(0x60);

    if (pkt_idx == 0) {
        /* Filtra respostas de protocolo (ACK 0xFA, resend 0xFE,
         * BAT 0xAA/0xFC): tem bit 3 ligado mas nao sao pacotes.
         * Sem isso, um byte perdido teleporta o cursor. */
        if (b == 0xFA || b == 0xFE || b == 0xAA || b == 0xFC) {
            pic_send_eoi(MOUSE_IRQ);
            return;
        }
        /* sincroniza: primeiro byte sempre tem bit 3 ligado */
        if (!(b & 0x08)) {
            pic_send_eoi(MOUSE_IRQ);
            return;
        }
    }
    pkt[pkt_idx++] = b;
    if (pkt_idx < 3) {
        pic_send_eoi(MOUSE_IRQ);
        return;
    }
    pkt_idx = 0;

    mouse_buttons = pkt[0] & 0x07;
    int dx = (int)pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
    int dy = (int)pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);

    if (dx != 0 || dy != 0 || (pkt[0] & 0x07)) {
        uint32_t rw = 1024, rh = 768;
        gui_get_res(&rw, &rh); /* cantos exatos da resolucao atual */
        mouse_x += dx;
        mouse_y -= dy; /* Y do PS/2 cresce p/ cima */
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x > (int)rw - 1) mouse_x = (int)rw - 1;
        if (mouse_y > (int)rh - 1) mouse_y = (int)rh - 1;
        mouse_dirty = 1;
    }
    /* clique tambem suja a tela mesmo sem movimento */
    if (pkt[0] & 0x07) {
        mouse_dirty = 1;
    }

    pic_send_eoi(MOUSE_IRQ);
}

void mouse_init(void)
{
    /* habilita porta auxiliar (tem ACK: consome) */
    mouse_wait(1);
    outb(0x64, 0xA8);
    ack_wait();

    /* habilita IRQ12 no byte de configuracao */
    mouse_wait(1);
    outb(0x64, 0x20);
    int status = ack_wait(); /* agora sim: o byte de status */
    if (status < 0) {
        status = 0x65; /* valor tipico QEMU/SeaBIOS */
    }
    status |= 0x02;   /* IRQ12 */
    status &= ~0x20;  /* habilita mouse */
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, (uint8_t)status);

    mouse_write_cmd(0xF6); /* defaults */
    mouse_write_cmd(0xF4); /* enable data reporting */

    /* Drena qualquer resto com 100ms de silencio garantido. */
    {
        uint64_t fim = pit_get_ticks() + 10;
        while (pit_get_ticks() < fim) {
            if (inb(0x64) & 0x01) {
                (void)inb(0x60);
                fim = pit_get_ticks() + 10;
            }
        }
    }

    pic_unmask_irq(MOUSE_IRQ);
    mouse_present = 1;
    serial_puts("mouse: PS/2 inicializado (IRQ12)\n");
}
