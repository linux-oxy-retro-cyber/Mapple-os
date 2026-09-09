/* bmp.c - Leitor BMP 24-bit sem compressao + blit com escala.
 *
 * Base do visualizador de imagens e dos wallpapers a partir de
 * arquivo. Suporta BITMAPINFOHEADER (40B), bottom-up e top-down.
 */
#include <stdint.h>
#include <stddef.h>

extern void console_putc(char c);
extern void console_puts(const char* s);

typedef struct {
    const uint8_t* base; /* inicio dos pixels */
    int w, h;            /* h sempre positivo */
    int stride;          /* bytes por linha (com padding p/ multiplo de 4) */
    int flip;            /* 1 = bottom-up (origem embaixo) */
} bmp_t;

static uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Abre BMP da memoria. 0 ok, -1 erro (assinatura/tipo/tamanho). */
int bmp_open(const uint8_t* data, size_t size, bmp_t* out)
{
    uint32_t off, ihsize, comp;
    int32_t w, h;
    uint16_t bpp;
    if (!data || !out || size < 54) {
        return -1;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        return -1;
    }
    off = rd32(data + 10);
    ihsize = rd32(data + 14);
    if (ihsize != 40) {
        return -1;
    }
    w = (int32_t)rd32(data + 18);
    h = (int32_t)rd32(data + 22);
    if (rd16(data + 26) != 1) {
        return -1;
    }
    bpp = rd16(data + 28);
    comp = rd32(data + 30);
    if (bpp != 24 || comp != 0) {
        return -1; /* so 24-bit sem compressao */
    }
    if (w <= 0 || w > 4096 || h == 0 || h < -4096 || h > 4096) {
        return -1;
    }
    out->w = w;
    out->stride = ((w * 3 + 3) & ~3);
    if (h < 0) {
        out->h = -h;
        out->flip = 0; /* top-down */
    } else {
        out->h = h;
        out->flip = 1; /* bottom-up: linha 0 do BMP e a base */
    }
    if (off + (size_t)out->stride * (size_t)out->h > size) {
        return -1;
    }
    out->base = data + off;
    return 0;
}

/* Pixel (x,y) com origem no topo, em RGB. */
static void bmp_pixel(const bmp_t* b, int x, int y,
                      uint8_t* r, uint8_t* g, uint8_t* bb)
{
    const uint8_t* p;
    if (x < 0) {
        x = 0;
    }
    if (x >= b->w) {
        x = b->w - 1;
    }
    if (y < 0) {
        y = 0;
    }
    if (y >= b->h) {
        y = b->h - 1;
    }
    if (b->flip) {
        p = b->base + (size_t)(b->h - 1 - y) * (size_t)b->stride + (size_t)x * 3;
    } else {
        p = b->base + (size_t)y * (size_t)b->stride + (size_t)x * 3;
    }
    *bb = p[0];
    *g = p[1];
    *r = p[2];
}

/* Desenha escalado (nearest) via plot(x,y,r,g,b). Retorna 0 ok. */
int bmp_blit(const bmp_t* b, int dx, int dy, int dw, int dh,
             void (*plot)(int x, int y, uint8_t r, uint8_t g, uint8_t bb))
{
    int y;
    if (!b || !plot || dw <= 0 || dh <= 0) {
        return -1;
    }
    for (y = 0; y < dh; y++) {
        int sy = y * b->h / dh;
        int x;
        for (x = 0; x < dw; x++) {
            int sx = x * b->w / dw;
            uint8_t r, g, bb;
            bmp_pixel(b, sx, sy, &r, &g, &bb);
            plot(dx + x, dy + y, r, g, bb);
        }
    }
    return 0;
}

/* Info curta no console. */
void bmp_info(const bmp_t* b)
{
    extern void console_print_u64(uint64_t v);
    console_puts("bmp ");
    console_print_u64((uint64_t)b->w);
    console_putc('x');
    console_print_u64((uint64_t)b->h);
    console_puts("x24\n");
}
