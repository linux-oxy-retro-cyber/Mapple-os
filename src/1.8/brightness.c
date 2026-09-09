/* brightness.c - Controle de brilho do mapple (backlight por software).
 *
 *  - Nivel 10-100 (padrao 100 = sem atenuacao). Vale p/ o framebuffer
 *    grafico; no modo texto VGA e so registrado (paleta do hardware).
 *  - Aplicacao: gui.c chama bright_apply() por pixel no present
 *    (back->front). Se nivel == 100, o caminho rapido (u64) e usado
 *    sem custo; senao cada pixel RGB e escalado por nivel/100.
 *    Formato assumido: XRGB8888 (R=16,G=8,B=0), igual ao compositor.
 *  - Shell: `brilho [10-100]` (alias `brightness`). GUI: Configuracoes
 *    pode chamar bright_set() (mesma variavel).
 *  - Persistencia: nao ha (volta a 100 no boot; roadmap: salvar no FS).
 */
#include <stdint.h>

extern void serial_puts(const char* s);
extern void drv_ready(const char* name, int ok);

static int bright_level = 100;

void bright_init(void)
{
    bright_level = 100;
    drv_ready("backlight", 1);
    serial_puts("backlight: nivel=100 (software, framebuffer)\n");
}

int bright_get(void)
{
    return bright_level;
}

void bright_set(int v)
{
    if (v < 10) {
        v = 10;
    }
    if (v > 100) {
        v = 100;
    }
    bright_level = v;
}

/* Escala um pixel XRGB8888 pelo nivel atual. */
uint32_t bright_apply(uint32_t c)
{
    uint32_t r, g, b;
    if (bright_level >= 100) {
        return c;
    }
    r = (c >> 16) & 0xFF;
    g = (c >> 8) & 0xFF;
    b = c & 0xFF;
    r = (r * (uint32_t)bright_level) / 100;
    g = (g * (uint32_t)bright_level) / 100;
    b = (b * (uint32_t)bright_level) / 100;
    return (r << 16) | (g << 8) | b;
}
