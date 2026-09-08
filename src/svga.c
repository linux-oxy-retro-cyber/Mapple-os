/* svga.c - Driver VMware SVGA II (GPU do QEMU com -vga vmware).
 *
 * - Detecta via PCI (15AD:0405), handshake de ID, le VRAM/capacidades.
 * - FIFO com timeouts (nunca trava): RECT_FILL, UPDATE, SYNC.
 * - Cursor por hardware (DEFINE_CURSOR + regs X/Y/ON), com fallback
 *   automatico p/ cursor por software.
 * - Sem SVGA presente: tudo vira no-op e o kernel segue 100% em CPU.
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern void* memset(void* dst, int c, size_t n);
extern void serial_puts(const char* s);
extern void serial_putc(char c);
extern int pci_find(uint16_t vendor, uint16_t device);
extern void pci_enable(int idx);

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass;
    uint32_t bar[6];
} pci_dev_t;
extern const pci_dev_t* pci_get(int idx);

/* Registradores SVGA (indice) */
#define R_ID            0
#define R_ENABLE        1
#define R_FB_START      13
#define R_VRAM_SIZE     15
#define R_FB_SIZE       16
#define R_CAPS          17
#define R_CONFIG_DONE   20
#define R_SYNC          21
#define R_BUSY          22
#define R_CURSOR_ID     24
#define R_CURSOR_X      25
#define R_CURSOR_Y      26
#define R_CURSOR_ON     27

/* IDs negociados (o dispositivo manda; aceita familia 0x9000000X) */
#define SVGA_ID_TRY     0x90000002u
#define SVGA_ID_INVALID 0xFFFFFFFFu

#define CAP_RECT_FILL   0x0002
#define CAP_RECT_COPY   0x0004
#define CAP_CURSOR      0x0020

/* FIFO (indices em dwords a partir da base) */
#define F_MIN     0
#define F_MAX     1
#define F_NEXT    2
#define F_STOP    3

/* Comandos FIFO */
#define C_UPDATE        1
#define C_RECT_FILL     2
#define C_RECT_COPY     3
#define C_DEFINE_CURSOR 19
#define C_DISPLAY_CURSOR 20
#define C_MOVE_CURSOR   21

static uint16_t svga_io = 0;      /* base I/O (indice em +0, valor em +4) */
static uint32_t* fifo = 0;
static uint32_t fifo_bytes = 0;
static int svga_present = 0;
static int fifo_ok = 0;
static int hw_cursor = 0;
static uint32_t vram_size = 0;
static uint32_t fb_size = 0;
static uint32_t caps = 0;
static uint64_t accel_fills = 0;

static inline void outl(uint16_t p, uint32_t v)
{
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p));
}

static inline uint32_t inl(uint16_t p)
{
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

/* ATENCAO: offsets em BYTES (indice +0, valor +1), acessos 32-bit */
static inline void reg_wr(uint32_t idx, uint32_t val)
{
    outl(svga_io, idx);
    outl(svga_io + 1, val);
}

static inline uint32_t reg_rd(uint32_t idx)
{
    outl(svga_io, idx);
    return inl(svga_io + 1);
}

static void hex32(uint32_t v)
{
    static const char* h = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        serial_putc(h[(v >> i) & 0xF]);
    }
}

/* Espera BUSY=0 (timeout ~100ms em loops). */
static int sync_flush(void)
{
    reg_wr(R_SYNC, 1);
    for (int i = 0; i < 100000; i++) {
        if (reg_rd(R_BUSY) == 0) {
            return 0;
        }
    }
    return -1;
}

/* Reserva nbytes no anel FIFO (com timeout). Retorna offset ou -1. */
static int fifo_reserve(uint32_t nbytes)
{
    uint32_t next, stop, max;
    nbytes = (nbytes + 3) & ~3u;
    for (int t = 0; t < 100000; t++) {
        next = fifo[F_NEXT];
        stop = fifo[F_STOP];
        max = fifo[F_MAX];
        if (next >= max || stop >= max) {
            return -1; /* estado invalido */
        }
        {
            uint32_t used = (next >= stop) ? (next - stop) : (max - stop + next);
            uint32_t free = (max - fifo[F_MIN]) - used;
            if (free >= nbytes + (uint32_t)sizeof(uint32_t)) {
                if (next + nbytes > max && next != fifo[F_MIN]) {
                    /* embrulha: espera esvaziar e recomeca */
                    if (next == stop) {
                        fifo[F_NEXT] = fifo[F_MIN];
                        return (int)fifo[F_MIN];
                    }
                    continue;
                }
                return (int)next;
            }
        }
    }
    return -1;
}

static void fifo_commit(uint32_t off, uint32_t nbytes)
{
    fifo[F_NEXT] = off + ((nbytes + 3) & ~3u);
}

static void fifo_put32(uint32_t off, uint32_t v)
{
    uint32_t max = fifo[F_MAX];
    if (off >= max) {
        off = fifo[F_MIN] + (off - max);
    }
    fifo[off / 4] = v;
}

/* Envia comando (id + args). 0 ok, -1 falha. */
static int fifo_cmd(uint32_t id, const uint32_t* args, int nargs)
{
    uint32_t total = (uint32_t)(1 + nargs) * 4;
    int off = fifo_reserve(total);
    uint32_t at;
    int i;
    if (off < 0) {
        return -1;
    }
    at = (uint32_t)off;
    fifo_put32(at, id);
    at += 4;
    for (i = 0; i < nargs; i++) {
        fifo_put32(at, args[i]);
        at += 4;
    }
    fifo_commit((uint32_t)off, total);
    return 0;
}

/* Tamanho de BAR de memoria (sonda padrao PCI). 0 se invalida. */
static uint32_t bar_size(int idx, uint32_t bar)
{
    (void)idx;
    if ((bar & 1) || (bar & ~0xFFFFFFF0u) == 0) {
        return 0;
    }
    return 0; /* sondagem real exige escrita no config; usa 64KB padrao */
}

int svga_init(void)
{
    int idx = pci_find(0x15AD, 0x0405);
    const pci_dev_t* d;
    uint32_t iob, fbb;
    if (idx < 0) {
        serial_puts("svga: ausente (modo CPU)\n");
        return -1;
    }
    d = pci_get(idx);
    pci_enable(idx); /* liga I/O + memoria + bus master */
    iob = d->bar[0] & ~3u;
    fbb = d->bar[1] & ~0xFu;
    if ((d->bar[0] & 1) == 0 || iob == 0) {
        serial_puts("svga: BAR0 invalida\n");
        return -1;
    }
    svga_io = (uint16_t)iob;
    reg_wr(R_ID, SVGA_ID_TRY);
    {
        uint32_t got = reg_rd(R_ID);
        if (got == SVGA_ID_INVALID || got == 0 ||
            (got & 0xFFFFFFF0u) != 0x90000000u) {
            serial_puts("svga: handshake falhou, leu=");
            hex32(got);
            serial_puts("\n");
            svga_io = 0;
            return -1;
        }
        serial_puts("svga: id=");
        hex32(got);
        serial_puts("\n");
    }
    svga_present = 1;
    vram_size = reg_rd(R_VRAM_SIZE);
    fb_size = reg_rd(R_FB_SIZE);
    caps = reg_rd(R_CAPS);

    serial_puts("svga: VMware SVGA II, VRAM=");
    hex32(vram_size);
    serial_puts(" FB=");
    hex32(fb_size);
    serial_puts(" caps=");
    hex32(caps);
    serial_puts("\n");
    serial_puts("svga: fb_bar=");
    hex32(fbb);
    serial_puts("\n");

    /* FIFO: BAR2 se for memoria; senao sem aceleracao */
    {
        uint32_t fb2 = d->bar[2];
        (void)bar_size(idx, fb2);
        if ((fb2 & 1) == 0 && (fb2 & ~0xFu) != 0) {
            uint32_t fphys = fb2 & ~0xFu;
            fifo = (uint32_t*)(uintptr_t)fphys;
            fifo_bytes = 64 * 1024;
            fifo[F_MIN] = 4096;
            fifo[F_MAX] = fifo_bytes;
            fifo[F_NEXT] = fifo[F_MIN];
            fifo[F_STOP] = fifo[F_MIN];
            /* readback: BAR2 e mesmo a FIFO? */
            if (fifo[F_MIN] != 4096 || fifo[F_MAX] != fifo_bytes ||
                fifo[F_NEXT] != 4096 || fifo[F_STOP] != 4096) {
                fifo_ok = 0;
                fifo = 0;
                serial_puts("svga: BAR2 nao e FIFO (CPU)\n");
            } else if (sync_flush() == 0) {
                fifo_ok = 1;
                serial_puts("svga: FIFO ok (64KB)\n");
            } else {
                fifo_ok = 0;
                fifo = 0;
                serial_puts("svga: FIFO falhou (CPU)\n");
            }
        } else {
            serial_puts("svga: sem BAR FIFO (CPU)\n");
        }
    }
    reg_wr(R_CONFIG_DONE, 1);
    return 0;
}

int svga_present_now(void)
{
    return svga_present;
}

int svga_accel_on(void)
{
    return svga_present && fifo_ok && (caps & CAP_RECT_FILL);
}

uint64_t svga_accel_count(void)
{
    return accel_fills;
}

uint32_t svga_vram(void)
{
    return vram_size;
}

uint32_t svga_fb_start(void)
{
    if (!svga_present) {
        return 0;
    }
    return reg_rd(R_FB_START);
}

/* Resolucao maxima anunciada (0,0 se ausente). */
void svga_maxres(uint32_t* w, uint32_t* h)
{
    if (!svga_present) {
        if (w) {
            *w = 0;
        }
        if (h) {
            *h = 0;
        }
        return;
    }
    if (w) {
        *w = reg_rd(4); /* MAX_WIDTH */
    }
    if (h) {
        *h = reg_rd(5); /* MAX_HEIGHT */
    }
}

/* Troca o modo de video em tempo real (VMware SVGA).
 * Atualiza fb_* via ponteiros; retorna 0 ok, -1 falha.
 * Restricoes: 32bpp, cabe no back buffer (w<=1024,h<=768). */
int svga_set_mode(uint32_t w, uint32_t h, uint32_t* fb_addr,
                  uint32_t* pitch, uint32_t* out_w, uint32_t* out_h)
{
    uint32_t mw, mh;
    if (!svga_present) {
        return -1;
    }
    if (w < 640 || h < 400 || w > 1024 || h > 768) {
        return -1;
    }
    svga_maxres(&mw, &mh);
    if ((mw && w > mw) || (mh && h > mh)) {
        return -1;
    }
    reg_wr(1, 0);          /* ENABLE=0: desliga antes de trocar */
    if (sync_flush() != 0) {
        return -1;
    }
    reg_wr(2, w);          /* WIDTH */
    reg_wr(3, h);          /* HEIGHT */
    reg_wr(7, 32);         /* BITS_PER_PIXEL */
    reg_wr(1, 1);          /* ENABLE=1 */
    reg_wr(20, 1);         /* CONFIG_DONE */
    if (sync_flush() != 0) {
        return -1;
    }
    if (fb_addr) {
        *fb_addr = reg_rd(13); /* FB_START (pode mudar!) */
    }
    if (pitch) {
        uint32_t bpl = reg_rd(12); /* BYTES_PER_LINE */
        *pitch = bpl ? bpl : w * 4;
    }
    if (out_w) {
        *out_w = w;
    }
    if (out_h) {
        *out_h = h;
    }
    serial_puts("svga: modo ");
    {
        char b[16];
        int n = 0;
        uint32_t t = w;
        char tmp[12];
        int k = 0;
        if (t == 0) {
            tmp[k++] = '0';
        }
        while (t > 0 && k < 12) {
            tmp[k++] = (char)('0' + t % 10);
            t /= 10;
        }
        while (k > 0 && n < 15) {
            b[n++] = tmp[--k];
        }
        b[n++] = 'x';
        t = h;
        k = 0;
        if (t == 0) {
            tmp[k++] = '0';
        }
        while (t > 0 && k < 12) {
            tmp[k++] = (char)('0' + t % 10);
            t /= 10;
        }
        while (k > 0 && n < 15) {
            b[n++] = tmp[--k];
        }
        b[n] = '\0';
        serial_puts(b);
    }
    serial_puts(" ok\n");
    return 0;
}

/* Preenche retangulo via GPU. 0 ok (acelerado), -1 CPU assume. */
int svga_fill(int x, int y, int w, int h, uint32_t color, uint32_t fbw, uint32_t fbh)
{
    uint32_t args[5];
    if (!svga_accel_on()) {
        return -1;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if ((uint32_t)(x + w) > fbw) {
        w = (int)fbw - x;
    }
    if ((uint32_t)(y + h) > fbh) {
        h = (int)fbh - y;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    args[0] = color;
    args[1] = (uint32_t)x;
    args[2] = (uint32_t)y;
    args[3] = (uint32_t)w;
    args[4] = (uint32_t)h;
    if (fifo_cmd(C_RECT_FILL, args, 5) != 0) {
        return -1;
    }
    /* SYNC imediato: a FIFO e assincrona; sem isso o fill cobriria
     * os pixels desenhados via CPU logo depois (faces, texto). */
    if (sync_flush() != 0) {
        fifo_ok = 0;
        return -1;
    }
    accel_fills++;
    return 0;
}

/* Flush da fila (SYNC). */
int svga_sync(void)
{
    if (!svga_present || !fifo_ok) {
        return -1;
    }
    return sync_flush();
}

/* UPDATE(x,y,w,h): informa regiao alterada p/ o display. */
int svga_update(int x, int y, int w, int h)
{
    uint32_t args[4];
    if (!svga_present || !fifo_ok) {
        return -1;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    args[0] = (uint32_t)x;
    args[1] = (uint32_t)y;
    args[2] = (uint32_t)w;
    args[3] = (uint32_t)h;
    return fifo_cmd(C_UPDATE, args, 4);
}

/* Cursor por hardware: define seta 16x16 (AND 1bpp + XOR 32bpp). */
int svga_cursor_init(void)
{
    /* seta 16x16: 1 = pixel da seta */
    static const uint16_t shape[16] = {
        0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xFF80, 0xF9C0, 0xF0E0, 0xE070, 0xC038, 0x001C, 0x000E, 0x0000
    };
    uint32_t hdr[7];
    int r, c;
    if (!svga_present || !(caps & CAP_CURSOR) || !fifo_ok) {
        return -1;
    }
    hdr[0] = 0;          /* id */
    hdr[1] = 0;          /* hotspot x */
    hdr[2] = 0;          /* hotspot y */
    hdr[3] = 16;         /* w */
    hdr[4] = 16;         /* h */
    hdr[5] = 1;          /* andMaskDepth */
    hdr[6] = 32;         /* xorMaskDepth */
    /* reserva: cabecalho + AND(32B) + XOR(1024B) */
    {
        int off = fifo_reserve(4 + 7 * 4 + 32 + 1024);
        uint32_t at;
        if (off < 0) {
            return -1;
        }
        at = (uint32_t)off;
        fifo_put32(at, C_DEFINE_CURSOR);
        at += 4;
        for (r = 0; r < 7; r++) {
            fifo_put32(at, hdr[r]);
            at += 4;
        }
        /* AND mask: 16 linhas x 2 bytes (MSB = esquerda) */
        for (r = 0; r < 16; r += 2) {
            uint32_t w32 = ((uint32_t)(~shape[r]) << 16) | (~shape[r + 1] & 0xFFFF);
            fifo_put32(at, w32);
            at += 4;
        }
        /* XOR mask: 16x16 ARGB (branco na seta, 0 fora) */
        for (r = 0; r < 16; r++) {
            for (c = 0; c < 16; c += 2) {
                uint32_t p0 = (shape[r] & (0x8000u >> c)) ? 0xFFFFFFFFu : 0x00000000u;
                uint32_t p1 = (shape[r] & (0x8000u >> (c + 1))) ? 0xFFFFFFFFu : 0x00000000u;
                /* empacota 2 pixels de 32 bits em 2 dwords */
                fifo_put32(at, p0);
                at += 4;
                fifo_put32(at, p1);
                at += 4;
            }
        }
        fifo_commit((uint32_t)off, 4 + 7 * 4 + 32 + 1024);
    }
    {
        uint32_t a[2];
        a[0] = 0;
        a[1] = 1;
        if (fifo_cmd(C_DISPLAY_CURSOR, a, 2) != 0) {
            return -1;
        }
    }
    if (sync_flush() != 0) {
        return -1;
    }
    hw_cursor = 1;
    serial_puts("svga: cursor HW ok\n");
    return 0;
}

int svga_cursor_on(void)
{
    return hw_cursor;
}

void svga_cursor_move(int x, int y)
{
    if (!hw_cursor) {
        return;
    }
    reg_wr(R_CURSOR_X, (uint32_t)x);
    reg_wr(R_CURSOR_Y, (uint32_t)y);
}

void svga_cursor_show(int on)
{
    if (!hw_cursor) {
        return;
    }
    reg_wr(R_CURSOR_ON, on ? 1u : 0u);
}
