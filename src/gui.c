/* gui.c - Gerenciador de janelas + apps do mapple (estilo Win98).
 *
 * - Framebuffer linear 32-bit (VBE via GRUB). Fallback: modo texto.
 * - WM: multi-janelas com foco, z-order, arrasto, minimizar, fechar.
 * - Taskbar: Iniciar + botoes das janelas + relogio com data.
 * - Menu Iniciar com icones; icones no desktop (duplo-clique abre).
 * - Apps: isn_terminal, Notepad, Explorador, Tarefas, Discos.
 * - Frame pacing (max ~50fps) + contador de FPS.
 * - Teclado roteado por foco (terminal x apps).
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, size_t n);
extern char* strcpy(char* dst, const char* src);
extern void* memset(void* dst, int c, size_t n);
extern void serial_puts(const char* s);
extern void serial_putc(char c);
extern uint64_t pit_uptime_sec(void);
extern uint64_t pit_get_ticks(void);
extern uint64_t sys_mem_kb;
extern const uint8_t font8x8[128][8];
extern void vga_clear_screen(void);
extern void vga_write_at(int row, int col, const char* s, uint8_t fg, uint8_t bg);
extern void vga_move_cursor(int row, int col);
extern void vga_taskbar_enable(int on);
extern volatile int mouse_x, mouse_y;
extern volatile uint8_t mouse_buttons;
extern volatile int mouse_dirty;
extern const uint32_t ramfs_count;
extern void fs_stats(uint32_t* nfiles, uint32_t* nbytes);
extern int fs_read(const char* path, uint8_t* buf, size_t cap);
extern int fs_write(const char* path, const uint8_t* data, size_t size);
extern char _end;
extern void cpu_hlt(void);

typedef struct {
    const char* name;
    const uint8_t* data;
    uint32_t size;
} ramfs_entry_t;
extern const ramfs_entry_t ramfs_table[];

int gui_active = 0;
void gui_mark_dirty(void); /* full redraw */
static void gui_draw(void); /* definida adiante */
static int gui_dirty = 1;
static int gui_chrome = 1;

/* --- Framebuffer + double buffer (anti-tearing) --- */
static uint32_t* fb = 0;
static uint32_t fb_w = 0, fb_h = 0, fb_pitch_px = 0;
static uint8_t sh_r = 16, sh_g = 8, sh_b = 0;

/* back buffer: comporta ate 4096 de pitch x 768 linhas (12MB) */
#define BACK_PITCH 4096
#define BACK_H 768
static uint32_t fb_back[BACK_PITCH * BACK_H];
static uint32_t* draw = 0;      /* alvo do desenho: fb_back ou fb */
static uint32_t draw_stride = 0;
static int use_back = 0;
static int svga_direct = 0; /* 1 = desenha direto na VRAM + present via SYNC */

extern int svga_accel_on(void);
extern int svga_fill(int x, int y, int w, int h, uint32_t color, uint32_t fbw, uint32_t fbh);
extern int svga_sync(void);
extern int svga_cursor_on(void);
extern void svga_cursor_move(int x, int y);
static int gui_editor = 0;      /* editor tela-cheia ativo: congela WM */
static int vsync_seen = 0, vsync_reported = 0;

static inline uint8_t vgast_inb(uint16_t p)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

/* Espera o retraco vertical (VGA 0x3DA bit 3) com timeout curto.
 * Timeout de 1 tick (~10ms): suficiente p/ nao travar e nao dominar
 * o frame. Sem vsync na emulacao, avanca mesmo assim. */
static void vsync_wait(void)
{
    uint64_t fim = pit_get_ticks() + 1;
    while (pit_get_ticks() < fim) {
        if (vgast_inb(0x3DA) & 0x08) {
            vsync_seen = 1;
            return;
        }
    }
}

/* apresenta so o retangulo sujo (parcial = rapido) */
static void fb_present_rect(int x0, int y0, int x1, int y1)
{
    extern int svga_update(int x, int y, int w, int h);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int)fb_w) {
        x1 = (int)fb_w;
    }
    if (y1 > (int)fb_h) {
        y1 = (int)fb_h;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (svga_direct) {
        /* avisa o dispositivo da regiao alterada (display refresh) */
        svga_update(x0, y0, x1 - x0, y1 - y0);
        svga_sync();
        return;
    }
    if (!use_back) {
        return;
    }
    vsync_wait();
    for (int y = y0; y < y1; y++) {
        uint32_t* s = fb_back + (uint32_t)y * draw_stride + (uint32_t)x0;
        uint32_t* d = fb + (uint32_t)y * fb_pitch_px + (uint32_t)x0;
        int n = x1 - x0, i = 0;
        if ((((uintptr_t)s | (uintptr_t)d) & 7) == 0) {
            uint64_t* s64 = (uint64_t*)s;
            uint64_t* d64 = (uint64_t*)d;
            int n64 = n / 2;
            for (int k = 0; k < n64; k++) {
                d64[k] = s64[k];
            }
            i = n64 * 2;
        }
        for (; i < n; i++) {
            d[i] = s[i];
        }
    }
    if (!vsync_reported) {
        vsync_reported = 1;
        serial_puts(vsync_seen ? "gui: vsync ok (sem tearing)\n" : "gui: vsync ausente, present direto\n");
    }
}

static uint32_t pack(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << sh_r) | ((uint32_t)g << sh_g) | ((uint32_t)b << sh_b);
}

static const uint8_t vga16[16][3] = {
    {0,0,0},{0,0,170},{0,170,0},{0,170,170},
    {170,0,0},{170,0,170},{170,85,0},{170,170,170},
    {85,85,85},{85,85,255},{85,255,85},{85,255,255},
    {255,85,85},{255,85,255},{255,255,85},{255,255,255}
};

#define C_DESK  0x008080
#define C_TASK  0xC0C0C0
#define C_TITLE 0x000080
#define C_TINACT 0x808080
#define C_WHITE 0xFFFFFF
#define C_BLACK 0x000000
#define C_SHAD  0x808080
#define C_ICONB 0x004040

#define TASK_H 28
#define TITLE_H 20
#define MB_MAGIC_OK 0x2BADB002
#define MENU_H 24
#define DOCK_H 60
#define CORNER_R 7

int gui_init(uint32_t magic, uint32_t mbi)
{
    if (magic != MB_MAGIC_OK || mbi == 0) {
        serial_puts("gui: sem info multiboot, modo texto\n");
        return -1;
    }
    uint32_t flags = *(volatile uint32_t*)(uintptr_t)mbi;
    if (!(flags & (1u << 12))) {
        serial_puts("gui: sem framebuffer no MBI, modo texto\n");
        return -1;
    }
    uint64_t addr = *(volatile uint64_t*)(uintptr_t)(mbi + 88);
    uint32_t pitch = *(volatile uint32_t*)(uintptr_t)(mbi + 96);
    uint32_t w = *(volatile uint32_t*)(uintptr_t)(mbi + 100);
    uint32_t h = *(volatile uint32_t*)(uintptr_t)(mbi + 104);
    uint8_t bpp = *(volatile uint8_t*)(uintptr_t)(mbi + 108);
    uint8_t type = *(volatile uint8_t*)(uintptr_t)(mbi + 109);
    if (type != 1 || bpp != 32 || w < 640 || h < 400 || addr == 0) {
        serial_puts("gui: fb incompativel, modo texto\n");
        return -1;
    }
    /* layout real XRGB8888 (campos do MBI nao confiaveis aqui) */
    sh_r = 16;
    sh_g = 8;
    sh_b = 0;

    fb = (uint32_t*)(uintptr_t)addr;
    fb_w = w;
    fb_h = h;
    fb_pitch_px = pitch / 4;
    mouse_x = (int)(w / 2); /* cursor começa no centro */
    mouse_y = (int)(h / 2);

    /* back buffer se couber; senao desenha direto */
    if (w <= 1024 && h <= BACK_H && pitch / 4 <= BACK_PITCH) {
        use_back = 1;
        draw = fb_back;
        draw_stride = pitch / 4;
        serial_puts("gui: double buffer on\n");
    } else {
        use_back = 0;
        draw = fb;
        draw_stride = pitch / 4;
        serial_puts("gui: double buffer off (fb grande)\n");
    }

    {
        /* identifica a resolucao ativa: WxHx32 */
        char b[24];
        size_t n = 0;
        uint32_t vals[3] = { w, h, 32 };
        serial_puts("gui: video ");
        for (int f = 0; f < 3; f++) {
            uint32_t t = vals[f];
            char tmp[12];
            size_t k = 0;
            if (t == 0) {
                tmp[k++] = '0';
            }
            while (t > 0 && k < sizeof(tmp)) {
                tmp[k++] = (char)('0' + t % 10);
                t /= 10;
            }
            while (k > 0 && n < sizeof(b) - 1) {
                b[n++] = tmp[--k];
            }
            b[n++] = (f < 2) ? 'x' : '\0';
        }
        b[sizeof(b) - 1] = '\0';
        serial_puts(b);
        serial_puts("\n");
    }
    serial_puts("gui: framebuffer ok (wm multi-janelas)\n");
    gui_active = 1;
    gui_mark_dirty();
    return 0;
}

/* --- Primitivas --- */
/* dirty rect: regiao a redesenhar/apresentar (eficiencia) */
static int dirty_on = 0;
static int dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = 0, dirty_y1 = 0;
static int full_draw = 1;

/* clip ativo durante o redraw parcial */
static int clip_on = 0;
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;


void gui_invalidate(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb_w) x1 = (int)fb_w;
    if (y1 > (int)fb_h) y1 = (int)fb_h;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (!dirty_on) {
        dirty_x0 = x0;
        dirty_y0 = y0;
        dirty_x1 = x1;
        dirty_y1 = y1;
        dirty_on = 1;
    } else {
        if (x0 < dirty_x0) dirty_x0 = x0;
        if (y0 < dirty_y0) dirty_y0 = y0;
        if (x1 > dirty_x1) dirty_x1 = x1;
        if (y1 > dirty_y1) dirty_y1 = y1;
    }
    gui_dirty = 1;
}


static int in_clip(int x, int y)
{
    if (!clip_on) {
        return 1;
    }
    return x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1;
}

static void px(int x, int y, uint32_t c)
{
    if ((uint32_t)x < fb_w && (uint32_t)y < fb_h && in_clip(x, y)) {
        draw[(uint32_t)y * draw_stride + (uint32_t)x] = c;
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if ((uint32_t)(x + w) > fb_w) w = (int)fb_w - x;
    if ((uint32_t)(y + h) > fb_h) h = (int)fb_h - y;
    if (clip_on) {
        if (x < clip_x0) {
            w -= (clip_x0 - x);
            x = clip_x0;
        }
        if (y < clip_y0) {
            h -= (clip_y0 - y);
            y = clip_y0;
        }
        if (x + w > clip_x1) {
            w = clip_x1 - x;
        }
        if (y + h > clip_y1) {
            h = clip_y1 - y;
        }
        if (w <= 0 || h <= 0) {
            return;
        }
    }

    for (int j = 0; j < h; j++) {
        uint32_t* row = draw + (uint32_t)(y + j) * draw_stride + (uint32_t)x;
        /* stores de 64 bits quando alinhado */
        uint64_t c2 = ((uint64_t)c << 32) | c;
        int i = 0;
        if (((uintptr_t)row & 7) == 0) {
            uint64_t* r64 = (uint64_t*)row;
            int n64 = w / 2;
            for (int k = 0; k < n64; k++) {
                r64[k] = c2;
            }
            i = n64 * 2;
        }
        for (; i < w; i++) {
            row[i] = c;
        }
    }
}

/* primitivas que desenham DIRETO no front (editor tela-cheia) */
static void fpx(int x, int y, uint32_t c)
{
    if ((uint32_t)x < fb_w && (uint32_t)y < fb_h) {
        fb[(uint32_t)y * fb_pitch_px + (uint32_t)x] = c;
    }
}

static void fglyph(int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t* g = font8x8[(uint8_t)ch & 0x7F];
    for (int r = 0; r < 8; r++) {
        uint8_t bits = g[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80u >> c)) {
                fpx(x + c, y + r, fg);
            } else {
                fpx(x + c, y + r, bg);
            }
        }
    }
}

static void glyph(int x, int y, char ch, uint32_t fg, uint32_t bg, int opaque)
{
    const uint8_t* g = font8x8[(uint8_t)ch & 0x7F];
    for (int r = 0; r < 8; r++) {
        uint8_t bits = g[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80u >> c)) {
                px(x + c, y + r, fg);
            } else if (opaque) {
                px(x + c, y + r, bg);
            }
        }
    }
}

/* mistura src sobre dst (alpha 0-255) no layout XRGB atual */
static uint32_t blend(uint32_t dst, uint32_t src, int alpha)
{
    int inv = 255 - alpha;
    int dr = (int)((dst >> sh_r) & 0xFF), dg = (int)((dst >> sh_g) & 0xFF), db = (int)((dst >> sh_b) & 0xFF);
    int sr = (int)((src >> sh_r) & 0xFF), sg = (int)((src >> sh_g) & 0xFF), sb = (int)((src >> sh_b) & 0xFF);
    uint32_t r = (uint32_t)((dr * inv + sr * alpha) / 255);
    uint32_t g = (uint32_t)((dg * inv + sg * alpha) / 255);
    uint32_t b = (uint32_t)((db * inv + sb * alpha) / 255);
    return (r << sh_r) | (g << sh_g) | (b << sh_b);
}

static void px_blend(int x, int y, uint32_t src, int alpha)
{
    if ((uint32_t)x < fb_w && (uint32_t)y < fb_h && in_clip(x, y)) {
        uint32_t d = draw[(uint32_t)y * draw_stride + (uint32_t)x];
        draw[(uint32_t)y * draw_stride + (uint32_t)x] = blend(d, src, alpha);
    }
}

/* retangulo com cantos arredondados (raio r) */
static void fill_rounded(int x, int y, int w, int h, int r, uint32_t c)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = i, dy = j;
            int inx = 1, iny = 1;
            if (i < r && j < r) {
                int ddx = r - 1 - i, ddy = r - 1 - j;
                inx = iny = (ddx * ddx + ddy * ddy <= r * r);
            } else if (i >= w - r && j < r) {
                int ddx = i - (w - r), ddy = r - 1 - j;
                inx = iny = (ddx * ddx + ddy * ddy <= r * r);
            } else if (i < r && j >= h - r) {
                int ddx = r - 1 - i, ddy = j - (h - r);
                inx = iny = (ddx * ddx + ddy * ddy <= r * r);
            } else if (i >= w - r && j >= h - r) {
                int ddx = i - (w - r), ddy = j - (h - r);
                inx = iny = (ddx * ddx + ddy * ddy <= r * r);
            }
            (void)dx;
            (void)dy;
            if (inx && iny) {
                px(x + i, y + j, c);
            }
        }
    }
}

/* mesma coisa com translucidez */
static void fill_rounded_blend(int x, int y, int w, int h, int r, uint32_t c, int alpha)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int ok = 1;
            if (i < r && j < r) {
                int ddx = r - 1 - i, ddy = r - 1 - j;
                ok = (ddx * ddx + ddy * ddy <= r * r);
            } else if (i >= w - r && j < r) {
                int ddx = i - (w - r), ddy = r - 1 - j;
                ok = (ddx * ddx + ddy * ddy <= r * r);
            } else if (i < r && j >= h - r) {
                int ddx = r - 1 - i, ddy = j - (h - r);
                ok = (ddx * ddx + ddy * ddy <= r * r);
            } else if (i >= w - r && j >= h - r) {
                int ddx = i - (w - r), ddy = j - (h - r);
                ok = (ddx * ddx + ddy * ddy <= r * r);
            }
            if (ok) {
                px_blend(x + i, y + j, c, alpha);
            }
        }
    }
}

/* --- Configuracoes (persistentes em RAM) --- */
static int cfg_wp_theme = 0;   /* 0 oceano, 1 por-do-sol, 2 floresta, 3 mono */
static int cfg_wp_bmp = 0;     /* 0 = tema, 1 = BMP de arquivo */
static int cfg_font = 0;       /* 0=8px, 1=16px, 2=32px */
static int cfg_res = 0;        /* 0=1024x768, 1=800x600, 2=640x480 */
static int term_scale = 1;     /* 1, 2 ou 4 (terminal) */

static const uint8_t wp_themes[4][6] = {
    { 0x3A, 0x6E, 0x8E, 0x00, 0x50, 0x50 }, /* oceano */
    { 0x60, 0x20, 0x50, 0xC0, 0x60, 0x10 }, /* por-do-sol */
    { 0x10, 0x50, 0x20, 0x00, 0x20, 0x10 }, /* floresta */
    { 0x40, 0x40, 0x48, 0x10, 0x10, 0x18 }, /* mono */
};
static const char* wp_names[] = {
    "Oceano", "Por-do-sol", "Floresta", "Mono"
};

/* gradiente vertical (papel de parede), com cache por linha + clip */
static uint32_t grad_cache[768];
static uint32_t grad_cache_h = 0xFFFFFFFFu;

static void wp_invalidate_cache(void)
{
    grad_cache_h = 0xFFFFFFFFu;
    gui_mark_dirty();
}

static void desktop_gradient(void)
{
    int r0 = wp_themes[cfg_wp_theme][0];
    int g0 = wp_themes[cfg_wp_theme][1];
    int b0 = wp_themes[cfg_wp_theme][2];
    int r1 = wp_themes[cfg_wp_theme][3];
    int g1 = wp_themes[cfg_wp_theme][4];
    int b1 = wp_themes[cfg_wp_theme][5];
    uint32_t y0 = 0, y1 = fb_h, x0 = 0, x1 = fb_w;
    uint32_t y;
    if (grad_cache_h != fb_h && fb_h <= 768) {
        for (y = 0; y < fb_h; y++) {
            int r = r0 + (r1 - r0) * (int)y / (int)fb_h;
            int g = g0 + (g1 - g0) * (int)y / (int)fb_h;
            int b = b0 + (b1 - b0) * (int)y / (int)fb_h;
            grad_cache[y] = pack((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        grad_cache_h = fb_h;
    }
    if (clip_on) {
        if ((int)y0 < clip_y0) {
            y0 = (uint32_t)clip_y0;
        }
        if ((int)y1 > clip_y1) {
            y1 = (uint32_t)clip_y1;
        }
        if ((int)x0 < clip_x0) {
            x0 = (uint32_t)clip_x0;
        }
        if ((int)x1 > clip_x1) {
            x1 = (uint32_t)clip_x1;
        }
    }
    for (y = y0; y < y1 && y < fb_h; y++) {
        uint32_t c = (y < 768 && grad_cache_h == fb_h) ? grad_cache[y] :
            pack((uint8_t)(r0 + (r1 - r0) * (int)y / (int)fb_h),
                 (uint8_t)(g0 + (g1 - g0) * (int)y / (int)fb_h),
                 (uint8_t)(b0 + (b1 - b0) * (int)y / (int)fb_h));
        uint32_t* row = draw + y * draw_stride;
        for (uint32_t x = x0; x < x1 && x < fb_w; x++) {
            row[x] = c;
        }
    }
}

/* papel de parede BMP decodificado (256x192) */
#define WP_W 256
#define WP_H 192
static uint8_t wp_px[WP_W * WP_H * 3];
static int wp_bmp_ok = 0;
static char wp_bmp_path[128] = { 0 };
static int wp_cur_bmp = 0;

/* BMPs disponiveis p/ wallpaper */
static char wp_files[4][128];
static int wp_nfiles = 0;

/* visualizador de imagens (ate 512x384 RGB) */
#define IMG_W 512
#define IMG_H 384
static uint8_t img_px[IMG_W * IMG_H * 3];
static int img_w = 0, img_h = 0, img_ok = 0;
static char img_path[128] = { 0 };

/* declaracoes BMP (bmp.c) */
typedef struct {
    const uint8_t* base;
    int w, h;
    int stride;
    int flip;
} bmp_t;
extern int bmp_open(const uint8_t* data, size_t size, bmp_t* out);
extern int bmp_blit(const bmp_t* b, int dx, int dy, int dw, int dh,
                    void (*plot)(int x, int y, uint8_t r, uint8_t g, uint8_t bb));
extern int fs_read(const char* path, uint8_t* buf, size_t cap);
extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern char* strcpy(char* dst, const char* src);

static uint8_t img_filebuf[128 * 1024]; /* leitura de BMPs (ate 128KB) */

static int ends_bmp(const char* name)
{
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' && name[n - 3] == 'b' &&
           name[n - 2] == 'm' && name[n - 1] == 'p';
}

static void wp_collect_cb(const char* name, int is_dir, uint32_t size, void* ctx)
{
    (void)size;
    (void)ctx;
    if (!is_dir && ends_bmp(name) && wp_nfiles < 4) {
        size_t k = 0;
        const char* pre = "/usr/share/wallpapers/";
        while (pre[k] && k < sizeof(wp_files[0]) - 1) {
            wp_files[wp_nfiles][k] = pre[k];
            k++;
        }
        {
            size_t j = 0;
            while (name[j] && k < sizeof(wp_files[0]) - 1) {
                wp_files[wp_nfiles][k++] = name[j++];
            }
        }
        wp_files[wp_nfiles][k] = '\0';
        wp_nfiles++;
    }
}

extern void fs_list(const char* dir, void (*cb)(const char*, int, uint32_t, void*), void* ctx);

/* aplica papel de parede BMP (decodifica p/ 256x192) */
static int wp_apply_bmp(const char* path)
{
    bmp_t b;
    int r = fs_read(path, img_filebuf, sizeof(img_filebuf));
    int x, y;
    if (r <= 0) {
        return -1;
    }
    if (bmp_open(img_filebuf, (size_t)r, &b) != 0) {
        return -1;
    }
    for (y = 0; y < WP_H; y++) {
        int sy = y * b.h / WP_H;
        for (x = 0; x < WP_W; x++) {
            int sx = x * b.w / WP_W;
            const uint8_t* p;
            int yy = sy, xx = sx;
            if (xx < 0) {
                xx = 0;
            }
            if (xx >= b.w) {
                xx = b.w - 1;
            }
            if (yy < 0) {
                yy = 0;
            }
            if (yy >= b.h) {
                yy = b.h - 1;
            }
            if (b.flip) {
                p = b.base + (size_t)(b.h - 1 - yy) * (size_t)b.stride + (size_t)xx * 3;
            } else {
                p = b.base + (size_t)yy * (size_t)b.stride + (size_t)xx * 3;
            }
            wp_px[(y * WP_W + x) * 3 + 0] = p[2];
            wp_px[(y * WP_W + x) * 3 + 1] = p[1];
            wp_px[(y * WP_W + x) * 3 + 2] = p[0];
        }
    }
    wp_bmp_ok = 1;
    strcpy(wp_bmp_path, path);
    cfg_wp_bmp = 1;
    wp_invalidate_cache();
    serial_puts("config: wallpaper ");
    serial_puts(path);
    serial_puts("\n");
    return 0;
}

static void wp_apply_theme(int t)
{
    cfg_wp_theme = t;
    cfg_wp_bmp = 0;
    wp_invalidate_cache();
    serial_puts("config: tema ");
    serial_puts(wp_names[t]);
    serial_puts("\n");
}

/* aplica resolucao (SVGA ao vivo; senao so informa) */
static void res_apply(int idx)
{
    static const uint32_t modes[3][2] = { { 1024, 768 }, { 800, 600 }, { 640, 480 } };
    extern int svga_present_now(void);
    cfg_res = idx;
    if (!svga_present_now()) {
        serial_puts("config: resolucao requer VMware (make run); use make RES=\n");
        return;
    }
    {
        extern int svga_set_mode(uint32_t w, uint32_t h, uint32_t* fb_addr,
                                 uint32_t* pitch, uint32_t* out_w, uint32_t* out_h);
        uint32_t fa = 0, pt = 0, w = 0, h = 0;
        if (svga_set_mode(modes[idx][0], modes[idx][1], &fa, &pt, &w, &h) == 0 && fa) {
            fb = (uint32_t*)(uintptr_t)fa;
            fb_w = w;
            fb_h = h;
            fb_pitch_px = pt / 4;
            draw_stride = fb_pitch_px;
            if (svga_direct) {
                draw = fb; /* framebuffer pode RELOCAR no switch! */
            }
            /* quiesce: deixa o dispositivo assentar antes de desenhar */
            {
                uint64_t t0 = pit_get_ticks() + 30; /* 300ms */
                while (pit_get_ticks() < t0) {
                    cpu_hlt();
                }
            }
            /* re-le o endereco (pode ter mudado tarde) */
            {
                extern uint32_t svga_fb_start(void);
                uint32_t fa2 = svga_fb_start();
                if (fa2 && fa2 != fa) {
                    fb = (uint32_t*)(uintptr_t)fa2;
                    if (svga_direct) {
                        draw = fb;
                    }
                }
            }
            serial_puts("config: fb=");
            {
                char b[16];
                int nn = 0;
                uint32_t vv = fa;
                char tmp[8];
                int kk = 0;
                if (vv == 0) {
                    tmp[kk++] = '0';
                }
                while (vv > 0 && kk < 8) {
                    uint32_t dd = vv % 16;
                    tmp[kk++] = (char)(dd < 10 ? '0' + dd : 'a' + dd - 10);
                    vv /= 16;
                }
                while (kk > 0 && nn < 15) {
                    b[nn++] = tmp[--kk];
                }
                b[nn] = '\0';
                serial_puts(b);
                serial_puts("\n");
            }
            grad_cache_h = 0xFFFFFFFFu;
            if (mouse_x >= (int)w) {
                mouse_x = (int)w - 1;
            }
            if (mouse_y >= (int)h) {
                mouse_y = (int)h - 1;
            }
            serial_puts("config: geo ");
            {
                char b[32];
                int nn = 0;
                uint32_t vv[4];
                vv[0] = w;
                vv[1] = h;
                vv[2] = pt / 4;
                vv[3] = (uint32_t)(draw == fb ? 1 : 0);
                for (int q = 0; q < 4; q++) {
                    uint32_t t = vv[q];
                    char tmp[12];
                    int k = 0;
                    if (t == 0) {
                        tmp[k++] = '0';
                    }
                    while (t > 0 && k < 12) {
                        tmp[k++] = (char)('0' + t % 10);
                        t /= 10;
                    }
                    while (k > 0 && nn < 30) {
                        b[nn++] = tmp[--k];
                    }
                    b[nn++] = (q < 3) ? '/' : '\0';
                }
                b[31] = '\0';
                serial_puts(b);
                serial_puts("\n");
            }
            gui_mark_dirty();
        }
    }
}

static void font_apply(int idx)
{
    static const int sc[3] = { 1, 2, 4 };
    static const char* nm[3] = { "8px", "16px", "32px" };
    term_scale = sc[idx];
    cfg_font = idx;
    gui_mark_dirty();
    serial_puts("config: texto ");
    serial_puts(nm[idx]);
    serial_puts("\n");
}

/* desenha o fundo: BMP esticado ou gradiente do tema */
static void desktop_draw(void)
{
    uint32_t y0 = 0, y1 = fb_h, x0 = 0, x1 = fb_w, y, x;
    if (clip_on) {
        if ((int)y0 < clip_y0) {
            y0 = (uint32_t)clip_y0;
        }
        if ((int)y1 > clip_y1) {
            y1 = (uint32_t)clip_y1;
        }
        if ((int)x0 < clip_x0) {
            x0 = (uint32_t)clip_x0;
        }
        if ((int)x1 > clip_x1) {
            x1 = (uint32_t)clip_x1;
        }
    }
    if (cfg_wp_bmp && wp_bmp_ok) {
        for (y = y0; y < y1 && y < fb_h; y++) {
            uint32_t* row = draw + y * draw_stride;
            uint32_t sy = y * WP_H / fb_h;
            for (x = x0; x < x1 && x < fb_w; x++) {
                uint32_t sx = x * WP_W / fb_w;
                const uint8_t* p = wp_px + (sy * WP_W + sx) * 3;
                row[x] = pack(p[2], p[1], p[0]);
            }
        }
        return;
    }
    desktop_gradient();
}


static void text(int x, int y, const char* s, uint32_t fg, uint32_t bg, int opaque)
{
    while (*s) {
        glyph(x, y, *s, fg, bg, opaque);
        x += 8;
        s++;
    }
}

/* glifo escalado (1x=8px, 2x=16px, 4x=32px): nearest, nitido */
static void glyph_scaled(int x, int y, char ch, uint32_t fg, uint32_t bg, int sc, int opaque)
{
    if (sc <= 1) {
        glyph(x, y, ch, fg, bg, opaque);
        return;
    }
    {
        const uint8_t* g = font8x8[(uint8_t)ch & 0x7F];
        int r, c;
        for (r = 0; r < 8; r++) {
            uint8_t bits = g[r];
            for (c = 0; c < 8; c++) {
                if (bits & (0x80u >> c)) {
                    fill_rect(x + c * sc, y + r * sc, sc, sc, fg);
                } else if (opaque) {
                    fill_rect(x + c * sc, y + r * sc, sc, sc, bg);
                }
            }
        }
    }
}

/* --- Terminal isn_terminal (buffer compartilhado) --- */
#define TERM_COLS 128
#define TERM_LINES 256
static char term_lines[TERM_LINES][TERM_COLS];
static uint32_t term_fg[TERM_LINES][TERM_COLS];
static uint32_t term_bg[TERM_LINES][TERM_COLS];
static uint32_t term_cur_fg = 0xFFFFFF;
static uint32_t term_cur_bg = 0x000000;
static int term_count = 0;
static int term_row = 0;
static int term_col = 0;

void term_set_attr(uint32_t fg, uint32_t bg)
{
    term_cur_fg = fg;
    term_cur_bg = bg;
}

void term_attr_reset(void)
{
    term_cur_fg = 0xFFFFFF;
    term_cur_bg = 0x000000;
}
static int term_cx = 8, term_cy = 28, term_cols = 80, term_rows = 40;

static void term_newline(void)
{
    term_col = 0;
    term_row++;
    if (term_count <= term_row) {
        term_count = term_row + 1;
        if (term_count > TERM_LINES) {
            for (int i = 1; i < TERM_LINES; i++) {
                for (int j = 0; j < TERM_COLS; j++) {
                    term_lines[i - 1][j] = term_lines[i][j];
                    term_fg[i - 1][j] = term_fg[i][j];
                    term_bg[i - 1][j] = term_bg[i][j];
                }
            }
            for (int j = 0; j < TERM_COLS; j++) {
                term_lines[TERM_LINES - 1][j] = '\0';
                term_fg[TERM_LINES - 1][j] = term_cur_fg;
                term_bg[TERM_LINES - 1][j] = term_cur_bg;
            }
            term_row--;
            term_count = TERM_LINES;
        }
    }
}

void term_putc(char c)
{
    if (!gui_active) {
        return;
    }
    if (term_row >= TERM_LINES) {
        term_row = TERM_LINES - 1;
    }
    char* line = term_lines[term_row];
    if (c == '\r') {
        term_col = 0;
    } else if (c == '\n') {
        term_newline();
    } else if (c == '\b') {
        if (term_col > 0) {
            term_col--;
            line[term_col] = '\0';
        }
    } else if (c == '\t') {
        do {
            if (term_col < TERM_COLS - 1) {
                line[term_col] = ' ';
                term_fg[term_row][term_col] = term_cur_fg;
                term_bg[term_row][term_col] = term_cur_bg;
                term_col++;
            }
        } while (term_col % 8);
        line[term_col] = '\0';
    } else if ((uint8_t)c >= 0x20) {
        /* quebra de linha na largura visivel (wrap real, nao corte) */
        int lim = term_cols > 0 ? term_cols : 80;
        if (lim > TERM_COLS - 1) {
            lim = TERM_COLS - 1;
        }
        if (term_col >= lim) {
            term_newline();
            line = term_lines[term_row];
        }
        if (term_col < TERM_COLS - 1) {
            line[term_col] = c;
            term_fg[term_row][term_col] = term_cur_fg;
            term_bg[term_row][term_col] = term_cur_bg;
            term_col++;
            line[term_col] = '\0';
        }
    }
    if (term_count <= term_row) {
        term_count = term_row + 1;
    }
    /* invalida so a area do terminal */
    gui_invalidate(term_cx - 8, term_cy - 8, term_cx + term_cols * 8 + 8,
                   term_cy + term_rows * 8 + 8);
}

void gui_mark_dirty(void)
{
    full_draw = 1;
    gui_dirty = 1;
}

void term_clear(void)
{
    term_count = 0;
    term_row = 0;
    term_col = 0;
    {
        int i, j;
        for (i = 0; i < TERM_LINES; i++) {
            for (j = 0; j < TERM_COLS; j++) {
                term_lines[i][j] = '\0';
                term_fg[i][j] = 0xFFFFFF;
                term_bg[i][j] = 0x000000;
            }
        }
    }
    gui_mark_dirty();
}

void gui_poll(void); /* definida abaixo; usada pela fila de teclas */

/* primitivas p/ apps em C++ (browser): respeitam clip */
void btext(int x, int y, const char* s, uint32_t fg, uint32_t bg)
{
    text(x, y, s, fg, bg, 1);
}

void bfill(int x, int y, int w, int h, uint32_t c)
{
    fill_rect(x, y, w, h, c);
}

/* --- Fila de teclas dos apps (quando o terminal nao tem foco) --- */
#define APPQ 128
static volatile char appq[APPQ];
static volatile uint8_t appq_h = 0, appq_t = 0;

void gui_app_push(char c)
{
    uint8_t n = (uint8_t)(appq_h + 1);
    if (n == appq_t) {
        return;
    }
    appq[appq_h] = c;
    appq_h = n;
}

char gui_app_getkey(void)
{
    for (;;) {
        __asm__ volatile ("cli");
        if (appq_h != appq_t) {
            char c = appq[appq_t];
            appq_t = (uint8_t)(appq_t + 1);
            __asm__ volatile ("sti");
            return c;
        }
        __asm__ volatile ("sti");
        gui_poll();
        cpu_hlt();
    }
}

/* Espera tecla do app, mas aborta (retorna 0) se alive() ficar falso.
 * Evita loop modal preso quando o foco muda (Alt+T/mouse) ou fecha. */
char gui_app_getkey_alive(int (*alive)(void))
{
    for (;;) {
        if (!alive()) {
            return 0;
        }
        __asm__ volatile ("cli");
        if (appq_h != appq_t) {
            char c = appq[appq_t];
            appq_t = (uint8_t)(appq_t + 1);
            __asm__ volatile ("sti");
            return c;
        }
        __asm__ volatile ("sti");
        gui_poll();
        cpu_hlt();
    }
}

/* --- Gerenciador de janelas --- */
typedef enum {
    W_TERM, W_NOTE, W_EXPL, W_TASK, W_DISK, W_CUBE, W_WEB, W_SET, W_IMG,
    W_CALC, W_ABOUT, W_CAL, W_CLOCK, W_SNAKE, W_MUS, W_MTRX
} wtype_t;

typedef struct {
    int used;
    int minimized;
    int maximized;
    wtype_t type;
    int x, y, w, h;
    int sx, sy, sw, sh; /* geometria salva p/ desmaximizar */
    char title[32];
} win_t;

#define MAXW 15
static win_t wins[MAXW];
static int zorder[MAXW];
static int zcount = 0;
static int focused = -1;
static int dragging = -1, drag_ox = 0, drag_oy = 0;
static int start_open = 0;
static uint8_t prev_btn = 0;
static uint64_t last_sec = (uint64_t)-1;
static uint64_t last_frame = 0, fps_frames = 0, fps_t0 = 0;
static int gui_fps = 0;
static uint64_t last_click_t = 0;
static int last_click_id = -1;

static const char* win_titles[] = {
    "isn_terminal", "Notepad", "Explorador", "Tarefas", "Discos", "3ddd", "Navegador",
    "Configuracoes", "Imagem", "Calc", "Sobre", "Calendario", "Relogio", "Snake", "Musica",
    "Matrix"
};

static int win_find(wtype_t t)
{
    for (int i = 0; i < MAXW; i++) {
        if (wins[i].used && wins[i].type == t) {
            return i;
        }
    }
    return -1;
}

static void win_raise(int idx)
{
    for (int i = 0; i < zcount; i++) {
        if (zorder[i] == idx) {
            for (int j = i; j + 1 < zcount; j++) {
                zorder[j] = zorder[j + 1];
            }
            zorder[zcount - 1] = idx;
            break;
        }
    }
}

static void win_focus(int idx)
{
    if (idx >= 0) {
        wins[idx].minimized = 0;
        win_raise(idx);
    }
    focused = idx;
    gui_mark_dirty();
}

static int win_open(wtype_t t)
{
    int idx = win_find(t);
    if (idx < 0) {
        for (int i = 0; i < MAXW; i++) {
            if (!wins[i].used) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        return -1;
    }
    if (!wins[idx].used) {
        wins[idx].used = 1;
        wins[idx].minimized = 0;
        wins[idx].type = t;
        const char* ttl = win_titles[t];
        size_t k = 0;
        while (ttl[k] && k < sizeof(wins[idx].title) - 1) {
            wins[idx].title[k] = ttl[k];
            k++;
        }
        wins[idx].title[k] = '\0';
        int n = zcount;
        wins[idx].x = 150 + n * 36;
        wins[idx].y = 60 + n * 30;
        wins[idx].w = 640;
        wins[idx].h = 460;
        if (t == W_TASK || t == W_DISK) {
            wins[idx].w = 480;
            wins[idx].h = 340;
        }
        zorder[zcount++] = idx;
    }
    win_focus(idx);
    return idx;
}

/* Alt+T: devolve o foco ao terminal (saida de teclado do beco sem saida). */
void gui_focus_term(void)
{
    if (!gui_active) {
        return;
    }
    int idx = win_find(W_TERM);
    if (idx >= 0) {
        win_raise(idx);
        win_focus(idx);
        serial_puts("gui: foco Term\n");
    }
}

static void win_close(int idx)
{
    if (idx < 0 || !wins[idx].used) {
        return;
    }
    wins[idx].used = 0;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i] == idx) {
            for (int j = i; j + 1 < zcount; j++) {
                zorder[j] = zorder[j + 1];
            }
            zcount--;
            break;
        }
    }
    if (focused == idx) {
        focused = zcount ? zorder[zcount - 1] : -1;
    }
    if (dragging == idx) {
        dragging = -1;
    }
    gui_mark_dirty();
}

/* janela sob o ponto (de cima p/ baixo na z-order) */
static int win_at(int mx, int my)
{
    for (int z = zcount - 1; z >= 0; z--) {
        int i = zorder[z];
        if (wins[i].used && !wins[i].minimized &&
            mx >= wins[i].x && mx < wins[i].x + wins[i].w &&
            my >= wins[i].y && my < wins[i].y + wins[i].h) {
            return i;
        }
    }
    return -1;
}

static int in_title(int idx, int mx, int my)
{
    return mx >= wins[idx].x + 3 && mx < wins[idx].x + wins[idx].w - 3 &&
           my >= wins[idx].y + 3 && my < wins[idx].y + 3 + TITLE_H;
}

static int in_close(int idx, int mx, int my)
{
    return mx >= wins[idx].x + 8 && mx < wins[idx].x + 20 &&
           my >= wins[idx].y + 7 && my < wins[idx].y + 19;
}

static int in_min(int idx, int mx, int my)
{
    return mx >= wins[idx].x + 26 && mx < wins[idx].x + 38 &&
           my >= wins[idx].y + 7 && my < wins[idx].y + 19;
}

static int in_max(int idx, int mx, int my)
{
    return mx >= wins[idx].x + 44 && mx < wins[idx].x + 56 &&
           my >= wins[idx].y + 7 && my < wins[idx].y + 19;
}

static void win_toggle_max(int idx)
{
    win_t* wn = &wins[idx];
    if (!wn->maximized) {
        wn->sx = wn->x;
        wn->sy = wn->y;
        wn->sw = wn->w;
        wn->sh = wn->h;
        wn->x = 6;
        wn->y = MENU_H + 6;
        wn->w = (int)fb_w - 12;
        wn->h = (int)fb_h - MENU_H - DOCK_H - 24;
        wn->maximized = 1;
    } else {
        wn->x = wn->sx;
        wn->y = wn->sy;
        wn->w = wn->sw;
        wn->h = wn->sh;
        wn->maximized = 0;
    }
    gui_mark_dirty();
}

/* terminal tem foco de teclado? */
int gui_term_focused(void)
{
    if (!gui_active) {
        return 1;
    }
    if (!gui_chrome) {
        return 1;
    }
    int t = win_find(W_TERM);
    return t >= 0 && !wins[t].minimized && focused == t;
}

/* --- Estado dos apps --- */
#define NOTE_MAX 8192
static char note_path[128] = "/home/guest/notas.txt";
static uint8_t note_buf[NOTE_MAX];
static uint32_t note_len = 0;
static uint32_t note_cur = 0, note_top = 0;
static uint32_t note_view = 0; /* scroll horizontal (coluna inicial) */
static int note_saved_flash = 0;

#define KEY_UP    ((char)0x81)
#define KEY_DOWN  ((char)0x82)
#define KEY_LEFT  ((char)0x83)
#define KEY_RIGHT ((char)0x84)
#define KEY_DEL   ((char)0x85)
#define KEY_HOME  ((char)0x86)
#define KEY_END   ((char)0x87)

#define EXPL_MAX 64
static char expl_path[128] = "/";
static char expl_names[EXPL_MAX][64];
static int expl_isdir[EXPL_MAX];
static uint32_t expl_sizes[EXPL_MAX];
static int expl_n = 0;
static int expl_sel = -1;

typedef struct {
    const char* name;
    int is_dir;
    uint32_t size;
    void* ctx;
} expl_cb_ctx_t;

static void expl_cb(const char* name, int is_dir, uint32_t size, void* ctx)
{
    (void)ctx;
    if (expl_n >= EXPL_MAX) {
        return;
    }
    size_t k = 0;
    while (name[k] && k < sizeof(expl_names[0]) - 1) {
        expl_names[expl_n][k] = name[k];
        k++;
    }
    expl_names[expl_n][k] = '\0';
    expl_isdir[expl_n] = is_dir;
    expl_sizes[expl_n] = size;
    expl_n++;
}

extern void fs_list(const char* dir, void (*cb)(const char*, int, uint32_t, void*), void* ctx);
extern int fs_isdir(const char* path);

static void expl_refresh(void)
{
    expl_n = 0;
    expl_sel = -1;
    fs_list(expl_path, expl_cb, 0);
}

void gui_note_open(const char* path)
{
    size_t k = 0;
    while (path[k] && k < sizeof(note_path) - 1) {
        note_path[k] = path[k];
        k++;
    }
    note_path[k] = '\0';
    int n = fs_read(note_path, note_buf, sizeof(note_buf));
    note_len = (n < 0) ? 0 : (uint32_t)n;
    note_cur = note_len;
    note_top = 0;
    note_view = 0;
    note_saved_flash = 0;
    int idx = win_open(W_NOTE);
    if (idx >= 0) {
        serial_puts("gui: abrir Notepad ");
        serial_puts(note_path);
        serial_puts("\n");
    }
}

void gui_expl_open(const char* path)
{
    size_t k = 0;
    while (path[k] && k < sizeof(expl_path) - 1) {
        expl_path[k] = path[k];
        k++;
    }
    expl_path[k] = '\0';
    expl_refresh();
    if (win_open(W_EXPL) >= 0) {
        serial_puts("gui: abrir Explorador ");
        serial_puts(expl_path);
        serial_puts("\n");
    }
}

void gui_term_open(void)
{
    if (win_open(W_TERM) >= 0) {
        serial_puts("gui: abrir isn_terminal\n");
    }
}

void gui_task_open(void)
{
    if (win_open(W_TASK) >= 0) {
        serial_puts("gui: abrir Tarefas\n");
    }
}

int gui_calc_modal(void)
{
    int idx = win_find(W_CALC);
    return idx >= 0 && !wins[idx].minimized && focused == idx;
}

int gui_snake_modal(void)
{
    int idx = win_find(W_SNAKE);
    return idx >= 0 && !wins[idx].minimized && focused == idx;
}

static char calc_in[64] = { 0 };
static int64_t calc_res = 0;
static int calc_show = 0; /* 0 editando, 1 mostrando resultado */

/* --- Calendario --- */
static int cal_y = 0, cal_m = 0; /* 0 = atual (RTC) */

/* --- Snake --- */
#define SN_W 20
#define SN_H 14
static int sn_x[64], sn_y[64], sn_len = 0, sn_dx = 1, sn_dy = 0;
static int sn_fx = 5, sn_fy = 5, sn_dead = 0, sn_score = 0;
static uint32_t sn_seed = 12345;
static void sn_reset(void);
static void sn_step(void);
static void calc_eval(void)
{
    extern int cc_eval_expr(const char* s, int64_t* out);
    int64_t v = 0;
    if (calc_in[0] && cc_eval_expr(calc_in, &v) == 0) {
        calc_res = v;
        calc_show = 1;
    } else {
        calc_show = 0;
    }
}

void calc_run_loop(void)
{
    for (;;) {
        char c;
        if (!gui_calc_modal()) {
            return;
        }
        c = gui_app_getkey_alive(gui_calc_modal);
        if (!gui_calc_modal()) {
            return;
        }
        if (c == 0x1B) {
            int idx = win_find(W_CALC);
            win_close(idx);
            {
                int t = win_find(W_TERM);
                if (t >= 0) {
                    win_focus(t);
                }
            }
            return;
        } else if (c == '\n' || c == '=') {
            calc_eval();
        } else if (c == '\b') {
            size_t n = strlen(calc_in);
            if (n > 0) {
                calc_in[n - 1] = '\0';
                calc_show = 0;
            }
        } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*' || c == '/' ||
                   c == '%' || c == '(' || c == ')' || c == '.') {
            size_t n = strlen(calc_in);
            if (n + 1 < sizeof(calc_in)) {
                calc_in[n] = c;
                calc_in[n + 1] = '\0';
                calc_show = 0;
            }
        } else if (c == 'c' || c == 'C') {
            calc_in[0] = '\0';
            calc_show = 0;
        }
        gui_dirty = 1;
    }
}

void snake_run_loop(void)
{
    uint64_t next = pit_get_ticks() + 15;
    for (;;) {
        char c = 0;
        if (!gui_snake_modal()) {
            return;
        }
        /* tecla sem bloquear (uma por iteracao) */
        {
            __asm__ volatile ("cli");
            if (appq_h != appq_t) {
                c = appq[appq_t];
                appq_t = (uint8_t)(appq_t + 1);
            }
            __asm__ volatile ("sti");
        }
        if (c == 0x1B || c == 'q') {
            int idx = win_find(W_SNAKE);
            win_close(idx);
            {
                int t = win_find(W_TERM);
                if (t >= 0) {
                    win_focus(t);
                }
            }
            return;
        } else if (c == '\n' && sn_dead) {
            sn_reset();
        } else if (c == (char)0x81 || c == 'w') {
            if (sn_dy == 0) {
                sn_dx = 0;
                sn_dy = -1;
            }
        } else if (c == (char)0x82 || c == 's') {
            if (sn_dy == 0) {
                sn_dx = 0;
                sn_dy = 1;
            }
        } else if (c == (char)0x83 || c == 'a') {
            if (sn_dx == 0) {
                sn_dx = -1;
                sn_dy = 0;
            }
        } else if (c == (char)0x84 || c == 'd') {
            if (sn_dx == 0) {
                sn_dx = 1;
                sn_dy = 0;
            }
        }
        if (!gui_snake_modal()) {
            return;
        }
        if (pit_get_ticks() >= next) {
            next = pit_get_ticks() + 15;
            sn_step();
            {
                int ni = win_find(W_SNAKE);
                if (ni >= 0) {
                    gui_invalidate(wins[ni].x, wins[ni].y,
                                   wins[ni].x + wins[ni].w, wins[ni].y + wins[ni].h);
                }
            }
        }
        gui_poll();
        __asm__ volatile ("hlt");
    }
}

static void app_open_log(wtype_t t, const char* name)
{
    (void)t;
    serial_puts("gui: abrir ");
    serial_puts(name);
    serial_puts("\n");
}

void gui_calc_open(void)
{
    if (win_open(W_CALC) >= 0) {
        app_open_log(W_CALC, "Calc");
    }
}

void gui_about_open(void)
{
    if (win_open(W_ABOUT) >= 0) {
        app_open_log(W_ABOUT, "Sobre");
    }
}

void gui_cal_open(void)
{
    if (win_open(W_CAL) >= 0) {
        extern void rtc_get(uint8_t* day, uint8_t* mon, uint8_t* year2,
                            uint8_t* hh, uint8_t* mm, uint8_t* ss);
        uint8_t m = 0, y = 0;
        rtc_get(0, &m, &y, 0, 0, 0);
        if (cal_y == 0) {
            cal_m = m ? (int)m : 1;
            cal_y = 2000 + (y ? (int)y : 26);
        }
        app_open_log(W_CAL, "Calendario");
    }
}

void gui_clock_open(void)
{
    if (win_open(W_CLOCK) >= 0) {
        app_open_log(W_CLOCK, "Relogio");
    }
}

void gui_snake_open(void)
{
    if (win_open(W_SNAKE) >= 0) {
        sn_reset();
        app_open_log(W_SNAKE, "Snake");
    }
}

void gui_mus_open(void)
{
    if (win_open(W_MUS) >= 0) {
        app_open_log(W_MUS, "Musica");
    }
}

/* --- Matrix (chuva de glifos, estilo cmatrix) --- */
#define MXW 78
#define MXH 44
static char mx_ch[MXH][MXW];
static uint8_t mx_age[MXH][MXW];
static int mx_head[MXW];
static int mx_speed[MXW];
static int mx_len[MXW];
static uint32_t mx_seed = 987654321;

static uint32_t mx_rand(void)
{
    mx_seed = mx_seed * 1103515245u + 12345u;
    return (mx_seed >> 16) & 0x7FFFu;
}

static char mx_pick(void)
{
    static const char* set =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789$#@%&*+-<>";
    return set[mx_rand() % 60];
}

static void mx_reset(void)
{
    int x, y;
    for (y = 0; y < MXH; y++) {
        for (x = 0; x < MXW; x++) {
            mx_ch[y][x] = ' ';
            mx_age[y][x] = 0;
        }
    }
    for (x = 0; x < MXW; x++) {
        mx_head[x] = -(int)(mx_rand() % (MXH * 2));
        mx_speed[x] = 1 + (int)(mx_rand() % 3);
        mx_len[x] = 8 + (int)(mx_rand() % 14);
    }
}

static void mx_step(void)
{
    int x, y, k;
    static int tick = 0;
    tick++;
    for (y = 0; y < MXH; y++) {
        for (x = 0; x < MXW; x++) {
            if (mx_age[y][x] > 0) {
                mx_age[y][x]--;
            }
        }
    }
    for (x = 0; x < MXW; x++) {
        if ((tick % mx_speed[x]) != 0) {
            continue;
        }
        mx_head[x]++;
        if (mx_head[x] - mx_len[x] > MXH) {
            mx_head[x] = -(int)(mx_rand() % 20);
            mx_speed[x] = 1 + (int)(mx_rand() % 3);
            mx_len[x] = 8 + (int)(mx_rand() % 14);
            continue;
        }
        y = mx_head[x];
        if (y >= 0 && y < MXH) {
            mx_ch[y][x] = mx_pick();
            mx_age[y][x] = (uint8_t)(mx_len[x] + 2);
            /* mutacao: troca glifo da cauda as vezes */
            for (k = 1; k < 4 && y - k >= 0; k++) {
                if (mx_age[y - k][x] > 0 && (mx_rand() % 12) == 0) {
                    mx_ch[y - k][x] = mx_pick();
                }
            }
        }
    }
}

int gui_mtrx_modal(void)
{
    int idx = win_find(W_MTRX);
    return idx >= 0 && !wins[idx].minimized && focused == idx;
}

void mtrx_run_loop(void)
{
    uint64_t next = pit_get_ticks() + 6;
    for (;;) {
        char c = 0;
        if (!gui_mtrx_modal()) {
            return;
        }
        {
            __asm__ volatile ("cli");
            if (appq_h != appq_t) {
                c = appq[appq_t];
                appq_t = (uint8_t)(appq_t + 1);
            }
            __asm__ volatile ("sti");
        }
        if (c == 0x1B || c == 'q' || c == 'Q') {
            int idx = win_find(W_MTRX);
            win_close(idx);
            {
                int t = win_find(W_TERM);
                if (t >= 0) {
                    win_focus(t);
                }
            }
            return;
        }
        if (!gui_mtrx_modal()) {
            return;
        }
        if (pit_get_ticks() >= next) {
            next = pit_get_ticks() + 6;
            mx_step();
            {
                int ni = win_find(W_MTRX);
                if (ni >= 0) {
                    gui_invalidate(wins[ni].x, wins[ni].y,
                                   wins[ni].x + wins[ni].w, wins[ni].y + wins[ni].h);
                }
            }
        }
        gui_poll();
        __asm__ volatile ("hlt");
    }
}

void gui_mtrx_open(void)
{
    if (win_open(W_MTRX) >= 0) {
        mx_reset();
        app_open_log(W_MTRX, "Matrix");
    }
}

void gui_set_open(void)
{
    wp_nfiles = 0;
    fs_list("/usr/share/wallpapers", wp_collect_cb, 0);
    if (win_open(W_SET) >= 0) {
        serial_puts("gui: abrir Configuracoes\n");
    }
}

/* --- Calc --- */

void gui_img_open(const char* path)
{
    int r = fs_read(path, img_filebuf, sizeof(img_filebuf));
    bmp_t b;
    if (r <= 0) {
        return;
    }
    if (bmp_open(img_filebuf, (size_t)r, &b) != 0) {
        return;
    }
    if (b.w > IMG_W || b.h > IMG_H) {
        return; /* grande demais p/ o buffer */
    }
    /* copia decodificado p/ img_px (RGB) */
    {
        int x, y;
        for (y = 0; y < b.h; y++) {
            for (x = 0; x < b.w; x++) {
                const uint8_t* p;
                int xx = x, yy = y;
                if (b.flip) {
                    p = b.base + (size_t)(b.h - 1 - yy) * (size_t)b.stride + (size_t)xx * 3;
                } else {
                    p = b.base + (size_t)yy * (size_t)b.stride + (size_t)xx * 3;
                }
                img_px[(y * IMG_W + x) * 3 + 0] = p[2];
                img_px[(y * IMG_W + x) * 3 + 1] = p[1];
                img_px[(y * IMG_W + x) * 3 + 2] = p[0];
            }
        }
    }
    img_w = b.w;
    img_h = b.h;
    img_ok = 1;
    {
        size_t k = 0;
        while (path[k] && k < sizeof(img_path) - 1) {
            img_path[k] = path[k];
            k++;
        }
        img_path[k] = '\0';
    }
    if (win_open(W_IMG) >= 0) {
        serial_puts("gui: abrir Imagem ");
        serial_puts(path);
        serial_puts("\n");
    }
}

/* Alt+Tab visual: overlay com a lista, Tab avanca, confirma por
 * timeout (120 ticks), Enter/clique ou Esc cancela. */
static int switcher_on = 0;
static int switcher_pos = 0; /* posicao na zorder */
static uint64_t switcher_deadline = 0;

int gui_switcher_open(void)
{
    return switcher_on;
}

static int switcher_count(void)
{
    int n = 0, i;
    for (i = 0; i < zcount; i++) {
        int idx = zorder[i];
        if (wins[idx].used && !wins[idx].minimized) {
            n++;
        }
    }
    return n;
}

void gui_switcher_next(void)
{
    int i, cur = -1;
    if (!gui_active || zcount <= 1) {
        return;
    }
    if (!switcher_on) {
        for (i = 0; i < zcount; i++) {
            if (zorder[i] == focused) {
                cur = i;
                break;
            }
        }
        switcher_on = 1;
        switcher_pos = (cur + 1) % zcount;
    } else {
        switcher_pos = (switcher_pos + 1) % zcount;
    }
    /* pula minimizadas */
    for (i = 0; i < zcount; i++) {
        int idx = zorder[switcher_pos];
        if (wins[idx].used && !wins[idx].minimized) {
            break;
        }
        switcher_pos = (switcher_pos + 1) % zcount;
    }
    switcher_deadline = pit_get_ticks() + 120;
    gui_dirty = 1;
    serial_puts("gui: switcher\n");
}

void gui_switcher_confirm(void)
{
    if (!switcher_on) {
        return;
    }
    switcher_on = 0;
    {
        int idx = zorder[switcher_pos % zcount];
        if (wins[idx].used) {
            win_focus(idx);
        }
    }
}

void gui_switcher_cancel(void)
{
    switcher_on = 0;
    gui_dirty = 1;
}

/* Alt+Tab simples (compat): abre o switcher */
void gui_cycle_windows(void)
{
    gui_switcher_next();
}

static void draw_switcher(void)
{
    int n = switcher_count(), i, k = 0;
    int mw = 300, mh, mx, my;
    if (n <= 0) {
        return;
    }
    mh = n * 24 + 16;
    mx = ((int)fb_w - mw) / 2;
    my = ((int)fb_h - mh) / 2;
    fill_rounded_blend(mx, my, mw, mh, 10, pack(30, 30, 40), 220);
    for (i = 0; i < zcount; i++) {
        int idx = zorder[i];
        int iy;
        if (!wins[idx].used || wins[idx].minimized) {
            continue;
        }
        iy = my + 8 + k * 24;
        if (i == switcher_pos) {
            fill_rect(mx + 8, iy, mw - 16, 22, C_TITLE);
        }
        text(mx + 16, iy + 7, wins[idx].title, i == switcher_pos ? C_WHITE : C_BLACK,
             i == switcher_pos ? C_TITLE : C_TASK, 0);
        k++;
    }
}

void gui_disk_open(void)
{
    if (win_open(W_DISK) >= 0) {
        serial_puts("gui: abrir Discos\n");
    }
}

/* xwininfo: despeja geometria das janelas + mouse (debug/teste). */
void gui_winfo(void)
{
    extern void console_puts(const char* s);
    extern void console_print_u64(uint64_t v);
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].used) {
            continue;
        }
        console_puts("win");
        console_print_u64((uint64_t)i);
        console_puts(" ");
        console_puts(wins[i].title);
        console_puts(" @");
        console_print_u64((uint64_t)wins[i].x);
        console_puts(",");
        console_print_u64((uint64_t)wins[i].y);
        console_puts(" ");
        console_print_u64((uint64_t)wins[i].w);
        console_puts("x");
        console_print_u64((uint64_t)wins[i].h);
        console_puts(wins[i].minimized ? " MIN" : "");
        console_puts(focused == i ? " FOC\n" : "\n");
    }
    console_puts("mouse @");
    console_print_u64((uint64_t)(mouse_x < 0 ? 0 : mouse_x));
    console_puts(",");
    console_print_u64((uint64_t)(mouse_y < 0 ? 0 : mouse_y));
    console_puts("\n");
}

/* loop modal do Notepad: roda enquanto a janela esta aberta/focada */
int gui_note_modal(void)
{
    int idx = win_find(W_NOTE);
    return idx >= 0 && !wins[idx].minimized && focused == idx;
}

static uint32_t note_line_start(uint32_t idx)
{
    while (idx > 0 && note_buf[idx - 1] != '\n') idx--;
    return idx;
}

static uint32_t note_line_end(uint32_t idx)
{
    while (idx < note_len && note_buf[idx] != '\n') idx++;
    return idx;
}

void note_run_loop(void)
{
    extern void* memmove(void* dst, const void* src, size_t n);
    for (;;) {
        if (!gui_note_modal()) {
            return;
        }
        char c = gui_app_getkey_alive(gui_note_modal);
        if (!gui_note_modal()) {
            return;
        }
        if (c == 0x18 || c == 0x1B) {
            /* ^X salva+sai, Esc sai */
            if (c == 0x18) {
                fs_write(note_path, note_buf, note_len);
            }
            int idx = win_find(W_NOTE);
            win_close(idx);
            int t = win_find(W_TERM);
            if (t >= 0) {
                win_focus(t);
            }
            return;
        } else if (c == 0x0F) {
            {
                /* DEBUG TEMP: hexdump do buffer */
                extern void serial_putc(char c);
                extern void serial_puts(const char* s);
                const char* h = "0123456789ABCDEF";
                serial_puts("NBUF len=");
                serial_putc((char)('0' + note_len / 10));
                serial_putc((char)('0' + note_len % 10));
                serial_putc(' ');
                for (uint32_t k = 0; k < note_len && k < 90; k++) {
                    serial_putc(h[(note_buf[k] >> 4) & 0xF]);
                    serial_putc(h[note_buf[k] & 0xF]);
                }
                serial_puts("\n");
            }
            fs_write(note_path, note_buf, note_len);
            note_saved_flash = 1;
        } else if (c == '\n') {
            if (note_len < NOTE_MAX - 1) {
                memmove(note_buf + note_cur + 1, note_buf + note_cur, note_len - note_cur);
                note_buf[note_cur++] = '\n';
                note_len++;
            }
        } else if (c == '\b') {
            if (note_cur > 0) {
                memmove(note_buf + note_cur - 1, note_buf + note_cur, note_len - note_cur);
                note_cur--;
                note_len--;
            }
        } else if (c == KEY_DEL) {
            if (note_cur < note_len) {
                memmove(note_buf + note_cur, note_buf + note_cur + 1, note_len - note_cur - 1);
                note_len--;
            }
        } else if (c == KEY_LEFT) {
            if (note_cur > 0) {
                note_cur--;
            }
        } else if (c == KEY_RIGHT) {
            if (note_cur < note_len) {
                note_cur++;
            }
        } else if (c == KEY_HOME) {
            note_cur = note_line_start(note_cur);
        } else if (c == KEY_END) {
            note_cur = note_line_end(note_cur);
        } else if (c == KEY_UP || c == KEY_DOWN) {
            uint32_t ls = note_line_start(note_cur);
            uint32_t col = note_cur - ls;
            if (c == KEY_UP) {
                if (ls > 0) {
                    uint32_t pls = note_line_start(ls - 1);
                    uint32_t pe = note_line_end(pls);
                    note_cur = pls + col;
                    if (note_cur > pe) {
                        note_cur = pe;
                    }
                }
            } else {
                uint32_t le = note_line_end(note_cur);
                if (le < note_len) {
                    uint32_t nls = le + 1;
                    uint32_t ne = note_line_end(nls);
                    note_cur = nls + col;
                    if (note_cur > ne) {
                        note_cur = ne;
                    }
                }
            }
        } else if (c >= 0x20 && c <= 0x7E) {
            if (note_len < NOTE_MAX - 1) {
                memmove(note_buf + note_cur + 1, note_buf + note_cur, note_len - note_cur);
                note_buf[note_cur++] = (uint8_t)c;
                note_len++;
            }
        }
        {
            int ni = win_find(W_NOTE);
            if (ni >= 0) {
                gui_invalidate(wins[ni].x, wins[ni].y,
                               wins[ni].x + wins[ni].w, wins[ni].y + wins[ni].h);
            } else {
                gui_mark_dirty();
            }
        }
    }
}

/* --- Desenho --- */
static const char* mouse_arrow[16] = {
    "X...............",
    "XX..............",
    "XXX.............",
    "XXXX............",
    "XXXXX...........",
    "XXXXXX..........",
    "XXXXXXX.........",
    "XXXXXXXX........",
    "XXXXXXXXX.......",
    "XXXX.XXXX.......",
    "XX...XXXX.......",
    "X.....XXXX......",
    ".......XXXX.....",
    "........XXXX....",
    ".........XXX....",
    "................",
};

static void draw_cursor(int x, int y)
{
    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < 16; c++) {
            if (mouse_arrow[r][c] == 'X') {
                int edge = (c == 0 || mouse_arrow[r][c - 1] != 'X' ||
                            (c < 15 && mouse_arrow[r][c + 1] != 'X') ||
                            (r > 0 && mouse_arrow[r - 1][c] != 'X') ||
                            (r < 15 && mouse_arrow[r + 1][c] != 'X'));
                px(x + c, y + r, edge ? C_BLACK : C_WHITE);
            }
        }
    }
}

static inline void hw_outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t hw_inb(uint16_t port)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void clock_str(char* out)
{
    uint64_t s = pit_uptime_sec();
    uint64_t hh = (s / 3600) % 100, mm = (s / 60) % 60, ss = s % 60;
    out[0] = (char)('0' + hh / 10);
    out[1] = (char)('0' + hh % 10);
    out[2] = ':';
    out[3] = (char)('0' + mm / 10);
    out[4] = (char)('0' + mm % 10);
    out[5] = ':';
    out[6] = (char)('0' + ss / 10);
    out[7] = (char)('0' + ss % 10);
    out[8] = '\0';
}

static void date_str(char* out)
{
    extern void rtc_get(uint8_t* day, uint8_t* mon, uint8_t* year2,
                        uint8_t* hh, uint8_t* mm, uint8_t* ss);
    uint8_t d = 0, m = 0;
    rtc_get(&d, &m, 0, 0, 0, 0);
    out[0] = (char)('0' + d / 10);
    out[1] = (char)('0' + d % 10);
    out[2] = '/';
    out[3] = (char)('0' + m / 10);
    out[4] = (char)('0' + m % 10);
    out[5] = '\0';
}


/* --- 3ddd: cubo 3D em ponto fixo Q10 (sem FPU) --- */
#define FX 1024
static int sin_tab[256];
static int sin_ok = 0;

static void sin_build(void)
{
    int i;
    if (sin_ok) {
        return;
    }
    /* Taylor: sin(x)=x-x^3/3!+x^5/5!-x^7/7!, x em radianos Q10 */
    for (i = 0; i < 256; i++) {
        int64_t x = (int64_t)i * 6434 / 256; /* i * 2pi/256 */
        int64_t x2, r, p;
        if (x > 3217) {
            x -= 6434;
        }
        x2 = x * x / FX;
        r = x;
        p = x;
        p = -p * x2 / FX / (2 * 3);
        r += p;
        p = -p * x2 / FX / (4 * 5);
        r += p;
        p = -p * x2 / FX / (6 * 7);
        r += p;
        sin_tab[i] = (int)r;
    }
    sin_ok = 1;
}

static int fsin(int idx)
{
    return sin_tab[idx & 255];
}

static int fcos(int idx)
{
    return sin_tab[(idx + 64) & 255];
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

/* span horizontal rapido (writes diretos 64-bit, com clip) */
static void hspan(int xs, int xe, int y, uint32_t c)
{
    uint32_t* row;
    uint64_t c2;
    int i, t;
    if (y < 0 || (uint32_t)y >= fb_h) {
        return;
    }
    if (xs > xe) {
        t = xs;
        xs = xe;
        xe = t;
    }
    if (xs < 0) {
        xs = 0;
    }
    if ((uint32_t)xe >= fb_w) {
        xe = (int)fb_w - 1;
    }
    if (clip_on) {
        if (xs < clip_x0) {
            xs = clip_x0;
        }
        if (xe >= clip_x1) {
            xe = clip_x1 - 1;
        }
        if (y < clip_y0 || y >= clip_y1) {
            return;
        }
    }
    if (xe < xs) {
        return;
    }
    row = draw + (uint32_t)y * draw_stride + (uint32_t)xs;
    c2 = ((uint64_t)c << 32) | c;
    i = 0;
    if ((((uintptr_t)row) & 7) == 0) {
        int n = (xe - xs + 1) / 2, k;
        uint64_t* r64 = (uint64_t*)row;
        for (k = 0; k < n; k++) {
            r64[k] = c2;
        }
        i = n * 2;
    }
    {
        int total = xe - xs + 1;
        for (; i < total; i++) {
            row[i] = c;
        }
    }
}

/* triangulo preenchido (cor plana), scanline inteira */
static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c)
{
    int tx, ty;
    /* ordena por y */
    if (y1 < y0) {
        tx = x0; x0 = x1; x1 = tx;
        ty = y0; y0 = y1; y1 = ty;
    }
    if (y2 < y0) {
        tx = x0; x0 = x2; x2 = tx;
        ty = y0; y0 = y2; y2 = ty;
    }
    if (y2 < y1) {
        tx = x1; x1 = x2; x2 = tx;
        ty = y1; y1 = y2; y2 = ty;
    }
    {
        int y;
        int64_t xa, xb, sa, sb;
        /* metade superior (y0..y1) */
        xa = (int64_t)x0 << 8;
        xb = (int64_t)x0 << 8;
        sa = (y1 > y0) ? (((int64_t)(x1 - x0) << 8) / (y1 - y0)) : 0;
        sb = (y2 > y0) ? (((int64_t)(x2 - x0) << 8) / (y2 - y0)) : 0;
        for (y = y0; y <= y1; y++) {
            hspan((int)(xa >> 8), (int)(xb >> 8), y, c);
            xa += sa;
            xb += sb;
        }
        /* metade inferior (y1..y2) */
        xa = ((int64_t)x1 << 8);
        xb = ((int64_t)x0 << 8) + sb * (y1 - y0);
        sa = (y2 > y1) ? (((int64_t)(x2 - x1) << 8) / (y2 - y1)) : 0;
        for (y = y1 + 1; y <= y2; y++) {
            hspan((int)(xa >> 8), (int)(xb >> 8), y, c);
            xa += sa;
            xb += sb;
        }
    }
}

static const int8_t cube_v[8][3] = {
    { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
    { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 }
};
static const uint8_t cube_q[6][4] = {
    { 1, 2, 6, 5 }, { 0, 4, 7, 3 }, { 3, 2, 6, 7 },
    { 0, 1, 5, 4 }, { 4, 5, 6, 7 }, { 0, 3, 2, 1 }
};
static const int8_t cube_n[6][3] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
    { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
};
static const uint8_t cube_col[6][3] = {
    { 230, 60, 60 }, { 230, 140, 40 }, { 60, 200, 80 },
    { 60, 120, 230 }, { 235, 210, 70 }, { 150, 80, 220 }
};
static const int8_t cube_e[12][2] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
};

static int cube_ang = 0;

static void draw_cube_client(int cx, int cy, int cw, int ch)
{
    int i, q;
    int32_t rz[8];
    int sx[8], sy[8];
    int order[6];
    int sa = fsin(cube_ang), ca = fcos(cube_ang);
    int st = fsin(38), ct = fcos(38); /* inclinacao fixa */
    int midx = cx + cw / 2, midy = cy + ch / 2;
    int m = cw < ch ? cw : ch;
    int half = (m / 2 - 12) * 3 / 4;
    int lx = 256, ly = 358, lz = 922; /* luz */
    int qz[6];
#define CUBE_R 560   /* meio-tamanho do cubo (Q10): cabe na projecao */
#define CUBE_D 6144  /* distancia da camera (6.0 em Q10) */

    sin_build();
    fill_rect(cx, cy, cw, ch, pack(12, 12, 20));

    /* rotaciona vertices: Y(ang) depois X(tilt) */
    for (i = 0; i < 8; i++) {
        int32_t x = (int32_t)cube_v[i][0] * CUBE_R;
        int32_t y = (int32_t)cube_v[i][1] * CUBE_R;
        int32_t z = (int32_t)cube_v[i][2] * CUBE_R;
        int32_t x1 = (x * ca + z * sa) / FX;
        int32_t z1 = (-x * sa + z * ca) / FX;
        int32_t y1 = (y * ct - z1 * st) / FX;
        int32_t z2 = (y * st + z1 * ct) / FX;
        rz[i] = z2;
        {
            int32_t den = CUBE_D + z2;
            int32_t pxx = x1 * CUBE_D / den;
            int32_t pyy = y1 * CUBE_D / den;
            sx[i] = midx + (int)(pxx * half / FX);
            sy[i] = midy - (int)(pyy * half / FX);
        }
    }
    /* ordem das faces (painter: longe primeiro) */
    for (q = 0; q < 6; q++) {
        int32_t z = 0;
        int k;
        for (k = 0; k < 4; k++) {
            z += rz[cube_q[q][k]];
        }
        qz[q] = (int)(z / 4);
        order[q] = q;
    }
    {
        int a, b;
        for (a = 0; a < 6; a++) {
            for (b = a + 1; b < 6; b++) {
                if (qz[order[a]] > qz[order[b]]) {
                    int t = order[a];
                    order[a] = order[b];
                    order[b] = t;
                }
            }
        }
    }
    for (q = 0; q < 6; q++) {
        int f = order[q];
        int v0 = cube_q[f][0], v1 = cube_q[f][1], v2 = cube_q[f][2], v3 = cube_q[f][3];
        /* normal rotacionada */
        int32_t nx = (int32_t)cube_n[f][0], ny = (int32_t)cube_n[f][1], nz = (int32_t)cube_n[f][2];
        int32_t nx1 = (nx * ca + nz * sa) / FX;
        int32_t nz1 = (-nx * sa + nz * ca) / FX;
        int32_t ny1 = (ny * ct - nz1 * st) / FX;
        int32_t nz2 = (ny * st + nz1 * ct) / FX;
        int32_t dot = (nx1 * lx + ny1 * ly + nz2 * lz) / FX;
        int bri;
        uint32_t col;
        if (dot < 0) {
            dot = 0;
        }
        bri = 90 + (int)(dot * 165 / FX);
        if (bri > 255) {
            bri = 255;
        }
        col = pack((uint8_t)(cube_col[f][0] * bri / 255),
                   (uint8_t)(cube_col[f][1] * bri / 255),
                   (uint8_t)(cube_col[f][2] * bri / 255));
        fill_tri(sx[v0], sy[v0], sx[v1], sy[v1], sx[v2], sy[v2], col);
        fill_tri(sx[v0], sy[v0], sx[v2], sy[v2], sx[v3], sy[v3], col);
    }
    /* arestas */
    for (i = 0; i < 12; i++) {
        draw_line(sx[cube_e[i][0]], sy[cube_e[i][0]], sx[cube_e[i][1]], sy[cube_e[i][1]], pack(10, 10, 16));
    }
    /* HUD: fps + cpu% */
    {
        extern int cpu_percent(void);
        char hb[24];
        int n = 0;
        int v = gui_fps, k = 0;
        char tmp[8];
        const char* a = "fps ";
        while (*a) {
            hb[n++] = *a++;
        }
        if (v == 0) {
            hb[n++] = '0';
        } else {
            while (v > 0 && k < 8) {
                tmp[k++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (k > 0) {
                hb[n++] = tmp[--k];
            }
        }
        a = " cpu ";
        while (*a) {
            hb[n++] = *a++;
        }
        v = cpu_percent();
        k = 0;
        if (v == 0) {
            hb[n++] = '0';
        } else {
            while (v > 0 && k < 8) {
                tmp[k++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (k > 0) {
                hb[n++] = tmp[--k];
            }
        }
        hb[n++] = '%';
        hb[n] = '\0';
        text(cx + 6, cy + 6, hb, C_WHITE, pack(12, 12, 20), 0);
    }
    {
        /* DEBUG TEMP: vertices projetados */
        extern void serial_putc(char c);
        extern void serial_puts(const char* s);
        static int nn = 0;
        if (nn < 2) {
            nn++;
            serial_puts("cube: mid=");
            serial_putc((char)('0' + midx / 100));
            serial_putc((char)('0' + (midx / 10) % 10));
            serial_putc((char)('0' + midx % 10));
            serial_putc(',');
            serial_putc((char)('0' + midy / 100));
            serial_putc((char)('0' + (midy / 10) % 10));
            serial_putc((char)('0' + midy % 10));
            serial_puts(" half=");
            serial_putc((char)('0' + half / 100));
            serial_putc((char)('0' + (half / 10) % 10));
            serial_putc((char)('0' + half % 10));
            serial_puts(" v0=");
            {
                int v = sx[0];
                if (v < 0) {
                    serial_putc('-');
                    v = -v;
                }
                serial_putc((char)('0' + v / 100));
                serial_putc((char)('0' + (v / 10) % 10));
                serial_putc((char)('0' + v % 10));
            }
            serial_putc(',');
            {
                int v = sy[0];
                if (v < 0) {
                    serial_putc('-');
                    v = -v;
                }
                serial_putc((char)('0' + v / 100));
                serial_putc((char)('0' + (v / 10) % 10));
                serial_putc((char)('0' + v % 10));
            }
            serial_puts("\n");
        }
    }
    cube_ang = (cube_ang + 4) & 255;
}

void gui_cube_open(void)
{
    if (win_open(W_CUBE) >= 0) {
        serial_puts("gui: abrir 3ddd\n");
    }
}

void gui_web_open(const char* url)
{
    extern void browser_open(const char* url);
    if (win_open(W_WEB) >= 0) {
        browser_open(url ? url : "mapple://inicio");
        serial_puts("gui: abrir Navegador ");
        serial_puts(url ? url : "mapple://inicio");
        serial_puts("\n");
    }
}

int gui_web_modal(void)
{
    int idx = win_find(W_WEB);
    return idx >= 0 && !wins[idx].minimized && focused == idx;
}

/* itoa simples p/ telas */
static void u64str(uint64_t v, char* out)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v > 0 && n < 23) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    int i = 0;
    while (n > 0) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
}

static void draw_note_client(int idx, int cx, int cy, int cw, int ch)
{
    int cols = cw / 8;
    int rows = (ch - 12) / 8;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    /* garante cursor visivel */
    uint32_t line = 0;
    for (uint32_t i = 0; i < note_cur; i++) {
        if (note_buf[i] == '\n') line++;
    }
    uint32_t topl = 0;
    for (uint32_t i = 0; i < note_top; i++) {
        if (note_buf[i] == '\n') topl++;
    }
    while (line < topl && note_top > 0) {
        note_top = (note_top > 0 && note_buf[note_top - 1] == '\n') ? note_top - 1 : note_line_start(note_top - 1);
        topl--;
    }
    while (line >= topl + (uint32_t)rows && note_top < note_len) {
        uint32_t le = note_line_end(note_top);
        note_top = (le < note_len) ? le + 1 : note_len;
        topl++;
    }
    /* scroll horizontal: mantem o cursor visivel */
    {
        uint32_t ls0 = note_line_start(note_cur);
        uint32_t col0 = note_cur - ls0;
        if (col0 < note_view) {
            note_view = col0;
        } else if (col0 >= note_view + (uint32_t)cols) {
            note_view = col0 - (uint32_t)cols + 1;
        }
    }
    uint32_t p = note_top;
    for (int r = 0; r < rows; r++) {
        uint32_t lend = note_line_end(p);
        uint32_t q = p + note_view;
        if (q > lend) {
            q = lend;
        }
        int cc = 0;
        while (q < lend && cc < cols) {
            glyph(cx + cc * 8, cy + r * 8, (char)note_buf[q], C_BLACK, C_WHITE, 0);
            q++;
            cc++;
        }
        p = (lend < note_len) ? lend + 1 : note_len;
    }
    /* cursor */
    if (focused == idx) {
        uint32_t ls = note_line_start(note_cur);
        int cr = (int)(line - topl);
        int ccol = (int)(note_cur - ls) - (int)note_view;
        if (cr >= 0 && cr < rows && ccol >= 0 && ccol < cols) {
            fill_rect(cx + ccol * 8, cy + cr * 8, 8, 8, C_BLACK);
        }
    }
    /* barra de status */
    fill_rect(cx, cy + rows * 8, cw, 12, C_TASK);
    if (note_saved_flash) {
        text(cx + 4, cy + rows * 8 + 2, "[salvo]", C_BLACK, C_TASK, 0);
        note_saved_flash = 0;
    } else {
        text(cx + 4, cy + rows * 8 + 2, "^O salvar  ^X fechar", C_BLACK, C_TASK, 0);
    }
}

static void draw_expl_client(int idx, int cx, int cy, int cw, int ch)
{
    (void)idx;
    (void)cw;
    /* barra de caminho */
    fill_rect(cx, cy, cw, 16, C_WHITE);
    text(cx + 4, cy + 4, expl_path, C_BLACK, C_WHITE, 0);
    /* botao Acima */
    fill_rect(cx + cw - 52, cy + 1, 48, 14, C_TASK);
    text(cx + cw - 46, cy + 3, "Acima", C_BLACK, C_TASK, 0);
    int rows = (ch - 20) / 12;
    for (int r = 0; r < rows && r < expl_n; r++) {
        int ry = cy + 20 + r * 12;
        if (r == expl_sel) {
            fill_rect(cx, ry, cw, 12, C_TITLE);
        }
        char nm[72];
        size_t k = 0;
        while (expl_names[r][k] && k < sizeof(nm) - 3) {
            nm[k] = expl_names[r][k];
            k++;
        }
        if (expl_isdir[r]) {
            nm[k++] = '/';
        }
        nm[k] = '\0';
        text(cx + 4, ry + 2, nm, r == expl_sel ? C_WHITE : C_BLACK,
             r == expl_sel ? C_TITLE : C_WHITE, 0);
    }
}

static int set_opt_count(void)
{
    return 4 + wp_nfiles;
}

static int set_opt_cur(void)
{
    if (cfg_wp_bmp) {
        return 4 + wp_cur_bmp;
    }
    return cfg_wp_theme;
}

static const char* set_opt_name(int i, char* tmp)
{
    if (i < 4) {
        return wp_names[i];
    }
    {
        /* nome base do arquivo */
        const char* p = wp_files[i - 4];
        const char* b = p;
        size_t k = 0;
        while (*p) {
            if (*p == '/') {
                b = p + 1;
            }
            p++;
        }
        while (*b && k < 31) {
            tmp[k++] = *b++;
        }
        tmp[k] = '\0';
        return tmp;
    }
}

static void set_opt_next(void)
{
    int n = set_opt_count();
    int cur = (set_opt_cur() + 1) % n;
    if (cur < 4) {
        wp_apply_theme(cur);
    } else {
        wp_cur_bmp = cur - 4;
        wp_apply_bmp(wp_files[wp_cur_bmp]);
    }
}

static void draw_set_client(int cx, int cy, int cw, int ch)
{
    char tmp[32], b[32];
    int y = cy + 6;
    (void)cw;
    (void)ch;
    text(cx + 8, y, "Configuracoes", C_BLACK, C_WHITE, 0);
    y += 20;
    text(cx + 8, y, "Papel de parede:", C_BLACK, C_WHITE, 0);
    text(cx + 200, y, "> ", C_BLACK, C_WHITE, 0);
    text(cx + 220, y, set_opt_name(set_opt_cur(), tmp), C_BLACK, C_WHITE, 0);
    y += 18;
    text(cx + 8, y, "Resolucao:", C_BLACK, C_WHITE, 0);
    text(cx + 200, y, "> ", C_BLACK, C_WHITE, 0);
    {
        static const char* modes[3] = { "1024x768", "800x600", "640x480" };
        text(cx + 220, y, modes[cfg_res], C_BLACK, C_WHITE, 0);
    }
    y += 18;
    text(cx + 8, y, "Texto:", C_BLACK, C_WHITE, 0);
    text(cx + 200, y, "> ", C_BLACK, C_WHITE, 0);
    {
        static const char* fz[3] = { "8px", "16px", "32px" };
        text(cx + 220, y, fz[cfg_font], C_BLACK, C_WHITE, 0);
    }
    y += 22;
    {
        extern int svga_present_now(void);
        extern uint32_t svga_vram(void);
        text(cx + 8, y, svga_present_now() ? "GPU: VMware SVGA II" : "GPU: VBE (CPU)", C_BLACK, C_WHITE, 0);
        y += 14;
        text(cx + 8, y, "Video atual:", C_BLACK, C_WHITE, 0);
        u64str(fb_w, b);
        text(cx + 200, y, b, C_BLACK, C_WHITE, 0);
        text(cx + 240, y, "x", C_BLACK, C_WHITE, 0);
        u64str(fb_h, b);
        text(cx + 255, y, b, C_BLACK, C_WHITE, 0);
        y += 14;
        text(cx + 8, y, "VRAM MB:", C_BLACK, C_WHITE, 0);
        u64str(svga_present_now() ? svga_vram() / (1024 * 1024) : 0, b);
        text(cx + 200, y, b, C_BLACK, C_WHITE, 0);
        y += 14;
        text(cx + 8, y, "Clique numa opcao p/ trocar.", C_BLACK, C_WHITE, 0);
    }
}

/* --- Calc --- */
static const char calc_keys[4][4] = {
    { '7', '8', '9', '/' },
    { '4', '5', '6', '*' },
    { '1', '2', '3', '-' },
    { '0', 'C', '=', '+' },
};

static void draw_calc_client(int cx, int cy, int cw, int ch)
{
    int r, c;
    char disp[64];
    int n = 0;
    (void)ch;
    fill_rect(cx, cy, cw, 26, C_BLACK);
    if (calc_show) {
        int64_t v = calc_res;
        char tmp[24];
        int k = 0, neg = 0;
        uint64_t u;
        disp[n++] = '=';
        if (v < 0) {
            neg = 1;
            u = (uint64_t)(-(v + 1)) + 1;
        } else {
            u = (uint64_t)v;
        }
        if (u == 0) {
            tmp[k++] = '0';
        }
        while (u > 0 && k < 20) {
            tmp[k++] = (char)('0' + u % 10);
            u /= 10;
        }
        if (neg && n < 60) {
            disp[n++] = '-';
        }
        while (k > 0 && n < 60) {
            disp[n++] = tmp[--k];
        }
        disp[n] = '\0';
    } else {
        size_t k = 0;
        while (calc_in[k] && n < 60) {
            disp[n++] = calc_in[k++];
        }
        disp[n] = '\0';
        if (n == 0) {
            disp[n++] = '0';
            disp[n] = '\0';
        }
    }
    text(cx + 8, cy + 9, disp, 0x00FF00, C_BLACK, 0);
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            int bx = cx + 8 + c * ((cw - 16) / 4);
            int by = cy + 32 + r * ((ch - 40) / 4);
            int bw = (cw - 16) / 4 - 4;
            int bh = (ch - 40) / 4 - 4;
            char s[2] = { calc_keys[r][c], '\0' };
            fill_rect(bx, by, bw, bh, C_TASK);
            text(bx + bw / 2 - 4, by + bh / 2 - 4, s, C_BLACK, C_TASK, 0);
        }
    }
}

/* --- Sobre --- */
static void draw_about_client(int cx, int cy, int cw, int ch)
{
    int y = cy + 10;
    (void)ch;
    /* selo MG: pastilha verde + letras 2x */
    {
        int bx = cx + cw - 104, by = cy + 6;
        fill_rect(bx, by, 96, 56, 0x1DB954);
        fill_rect(bx, by, 96, 4, 0x27E465);
        glyph_scaled(bx + 16, by + 12, 'M', 0xFFFFFF, 0x1DB954, 2, 0);
        glyph_scaled(bx + 48, by + 12, 'G', 0xFFFFFF, 0x1DB954, 2, 0);
    }
    text(cx + 8, y, "mapple 0.13.0", C_BLACK, C_WHITE, 0);
    y += 16;
    text(cx + 8, y, "kernel x86_64 em C/C++/asm", C_BLACK, C_WHITE, 0);
    y += 16;
    text(cx + 8, y, "sem dependencias externas", C_BLACK, C_WHITE, 0);
    y += 16;
    text(cx + 8, y, "shell built + 15 apps graficos", C_BLACK, C_WHITE, 0);
    y += 16;
    text(cx + 8, y, "feito a mao, com carinho", C_BLACK, C_WHITE, 0);
}

/* --- Calendario (Zeller) --- */
static int cal_weekday(int d, int m, int y)
{
    int K, J, h;
    if (m < 3) {
        m += 12;
        y--;
    }
    K = y % 100;
    J = y / 100;
    h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7; /* 0=domingo */
}

static int cal_mdays(int m, int y)
{
    static const int8_t t[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int n = t[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        n = 29;
    }
    return n;
}

static void draw_cal_client(int cx, int cy, int cw, int ch)
{
    static const char* mn[12] = { "jan", "fev", "mar", "abr", "mai", "jun",
                                  "jul", "ago", "set", "out", "nov", "dez" };
    char b[16];
    int y = cy + 6, i, d, wd, col, row;
    uint8_t td = 0, tm = 0, ty = 0;
    (void)cw;
    (void)ch;
    {
        extern void rtc_get(uint8_t* day, uint8_t* mon, uint8_t* year2,
                            uint8_t* hh, uint8_t* mm, uint8_t* ss);
        rtc_get(&td, &tm, &ty, 0, 0, 0);
    }
    text(cx + 8, y, "<", C_BLACK, C_WHITE, 0);
    text(cx + 280, y, ">", C_BLACK, C_WHITE, 0);
    {
        size_t n = 0;
        const char* s = mn[cal_m - 1];
        while (*s && n < sizeof(b) - 6) {
            b[n++] = *s++;
        }
        b[n++] = ' ';
        {
            int yy = cal_y, k = 0;
            char tmp[8];
            if (yy == 0) {
                tmp[k++] = '0';
            }
            while (yy > 0 && k < 8) {
                tmp[k++] = (char)('0' + yy % 10);
                yy /= 10;
            }
            while (k > 0) {
                b[n++] = tmp[--k];
            }
        }
        b[n] = '\0';
        text(cx + 100, y, b, C_BLACK, C_WHITE, 0);
    }
    y += 18;
    text(cx + 8, y, "D  S  T  Q  Q  S  S", C_BLACK, C_WHITE, 0);
    y += 14;
    wd = cal_weekday(1, cal_m, cal_y);
    for (d = 1, i = 0; d <= cal_mdays(cal_m, cal_y); d++) {
        col = (wd + d - 1) % 7;
        row = (wd + d - 1) / 7;
        {
            char db[4];
            db[0] = (char)('0' + d / 10);
            db[1] = (char)('0' + d % 10);
            db[2] = '\0';
            if (d == td && cal_m == tm && cal_y == 2000 + ty) {
                fill_rect(cx + 8 + col * 34, y + row * 14, 30, 12, C_TITLE);
                text(cx + 12 + col * 34, y + row * 14 + 2, db[0] == '0' ? db + 1 : db, C_WHITE, C_TITLE, 0);
            } else {
                text(cx + 12 + col * 34, y + row * 14 + 2, db[0] == '0' ? db + 1 : db, C_BLACK, C_WHITE, 0);
            }
        }
        (void)i;
    }
}

/* --- Relogio --- */
static void draw_clock_client(int cx, int cy, int cw, int ch)
{
    char clk[16], dt[8];
    uint8_t d = 0, m = 0, y = 0;
    int i;
    (void)ch;
    {
        extern void rtc_get(uint8_t* day, uint8_t* mon, uint8_t* year2,
                            uint8_t* hh, uint8_t* mm, uint8_t* ss);
        uint8_t hh = 0, mm = 0, ss = 0;
        rtc_get(&d, &m, &y, &hh, &mm, &ss);
        clk[0] = (char)('0' + hh / 10);
        clk[1] = (char)('0' + hh % 10);
        clk[2] = ':';
        clk[3] = (char)('0' + mm / 10);
        clk[4] = (char)('0' + mm % 10);
        clk[5] = ':';
        clk[6] = (char)('0' + ss / 10);
        clk[7] = (char)('0' + ss % 10);
        clk[8] = '\0';
        dt[0] = (char)('0' + d / 10);
        dt[1] = (char)('0' + d % 10);
        dt[2] = '/';
        dt[3] = (char)('0' + m / 10);
        dt[4] = (char)('0' + m % 10);
        dt[5] = '\0';
    }
    {
        int tw = 8 * 8 * 3;
        int tx = cx + (cw - tw) / 2;
        int ty = cy + 30;
        for (i = 0; clk[i]; i++) {
            char s[2] = { clk[i], '\0' };
            int r, c;
            const uint8_t* g = font8x8[(uint8_t)clk[i] & 0x7F];
            for (r = 0; r < 8; r++) {
                for (c = 0; c < 8; c++) {
                    if (g[r] & (0x80u >> c)) {
                        fill_rect(tx + i * 8 * 3 + c * 3, ty + r * 3, 3, 3, C_BLACK);
                    }
                }
            }
            (void)s;
        }
    }
    text(cx + (cw - 5 * 8) / 2, cy + 110, dt, C_BLACK, C_WHITE, 0);
}

/* --- Snake --- */
static void sn_reset(void)
{
    int i;
    sn_len = 4;
    sn_dx = 1;
    sn_dy = 0;
    for (i = 0; i < sn_len; i++) {
        sn_x[i] = 6 - i;
        sn_y[i] = 7;
    }
    sn_dead = 0;
    sn_score = 0;
    sn_seed = (uint32_t)pit_get_ticks() + 12345;
    sn_fx = 14;
    sn_fy = 7;
}

static uint32_t sn_rand(void)
{
    sn_seed = sn_seed * 1103515245u + 12345u;
    return (sn_seed >> 16) & 0x7FFFu;
}

static void sn_food(void)
{
    int i, ok;
    do {
        ok = 1;
        sn_fx = (int)(sn_rand() % SN_W);
        sn_fy = (int)(sn_rand() % SN_H);
        for (i = 0; i < sn_len; i++) {
            if (sn_x[i] == sn_fx && sn_y[i] == sn_fy) {
                ok = 0;
                break;
            }
        }
    } while (!ok);
}

static void sn_step(void)
{
    int i, nx, ny;
    if (sn_dead) {
        return;
    }
    nx = sn_x[0] + sn_dx;
    ny = sn_y[0] + sn_dy;
    if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) {
        sn_dead = 1;
        return;
    }
    for (i = 0; i < sn_len; i++) {
        if (sn_x[i] == nx && sn_y[i] == ny) {
            sn_dead = 1;
            return;
        }
    }
    if (nx == sn_fx && ny == sn_fy) {
        if (sn_len < 60) {
            sn_len++;
        }
        sn_score += 10;
        sn_food();
    }
    for (i = sn_len - 1; i > 0; i--) {
        sn_x[i] = sn_x[i - 1];
        sn_y[i] = sn_y[i - 1];
    }
    sn_x[0] = nx;
    sn_y[0] = ny;
}

static void draw_snake_client(int cx, int cy, int cw, int ch)
{
    int ox = cx + (cw - SN_W * 16) / 2;
    int oy = cy + 24 + (ch - 24 - SN_H * 16) / 2;
    int i;
    char sb[32];
    int n = 0;
    (void)ch;
    fill_rect(cx, cy, cw, ch, C_BLACK);
    for (i = 0; i < sn_len; i++) {
        fill_rect(ox + sn_x[i] * 16 + 1, oy + sn_y[i] * 16 + 1, 14, 14,
                  i == 0 ? 0x00FF00 : 0x009900);
    }
    fill_rect(ox + sn_fx * 16 + 1, oy + sn_fy * 16 + 1, 14, 14, 0xFF0000);
    {
        int v = sn_score, k = 0;
        char tmp[8];
        sb[n++] = 'p';
        sb[n++] = 't';
        sb[n++] = 's';
        sb[n++] = ' ';
        if (v == 0) {
            tmp[k++] = '0';
        }
        while (v > 0 && k < 8) {
            tmp[k++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (k > 0) {
            sb[n++] = tmp[--k];
        }
        if (sn_dead) {
            const char* m = " GAME OVER (enter)";
            while (*m) {
                sb[n++] = *m++;
            }
        }
        sb[n] = '\0';
    }
    text(cx + 8, cy + 6, sb, C_WHITE, C_BLACK, 0);
}

/* --- Musica --- */
static const char* mus_songs[] = {
    "Escala|523:120,587:120,659:120,784:120,880:240",
    "Ode|659:150,659:150,698:150,784:150,784:150,698:150,659:150,587:150,523:150,523:150,587:150,659:150,659:225,587:75,587:300",
    "Parabens|523:150,523:150,587:300,523:300,659:300,587:600",
};
#define NMUS 3

static void draw_mus_client(int cx, int cy, int cw, int ch)
{
    int i;
    (void)cw;
    (void)ch;
    text(cx + 8, cy + 6, "1/2/3 ou clique p/ tocar:", C_BLACK, C_WHITE, 0);
    for (i = 0; i < NMUS; i++) {
        char line[48];
        int n = 0, k = 0;
        line[n++] = (char)('1' + i);
        line[n++] = ' ';
        line[n++] = '-';
        line[n++] = ' ';
        while (mus_songs[i][k] && mus_songs[i][k] != '|' && n < 40) {
            line[n++] = mus_songs[i][k++];
        }
        line[n] = '\0';
        text(cx + 8, cy + 28 + i * 20, line, C_BLACK, C_WHITE, 0);
    }
}

static void draw_mtrx_client(int cx, int cy, int cw, int ch)
{
    int ox = cx + (cw - MXW * 8) / 2;
    int oy = cy + (ch - MXH * 8) / 2;
    int x, y;
    fill_rect(cx, cy, cw, ch, C_BLACK);
    for (y = 0; y < MXH; y++) {
        for (x = 0; x < MXW; x++) {
            uint8_t a = mx_age[y][x];
            uint32_t fg;
            char s[2];
            if (a == 0 || mx_ch[y][x] == ' ') {
                continue;
            }
            /* cabeca branca -> verde vivo -> verde escuro */
            if (a > 20) {
                fg = 0xE0FFE0;
            } else if (a > 12) {
                fg = 0x00FF00;
            } else if (a > 6) {
                fg = 0x00AA00;
            } else {
                fg = 0x005500;
            }
            s[0] = mx_ch[y][x];
            s[1] = '\0';
            text(ox + x * 8, oy + y * 8, s, fg, C_BLACK, 0);
        }
    }
    text(cx + 8, cy + ch - 12, "q/Esc sai", 0x005500, C_BLACK, 0);
}

static void mus_play(int idx)
{
    const char* s;
    if (idx < 0 || idx >= NMUS) {
        return;
    }
    s = mus_songs[idx];
    while (*s && *s != '|') {
        s++;
    }
    if (*s == '|') {
        s++;
    }
    {
        extern void pcspk_play(const char* seq);
        pcspk_play(s);
    }
}

static void draw_task_client(int cx, int cy, int cw, int ch)
{
    (void)cw;
    (void)ch;
    char b[32];
    int y = cy + 6;
    text(cx + 8, y, "Gerenciador de tarefas", C_BLACK, C_WHITE, 0);
    y += 18;
    text(cx + 8, y, "uptime(s):", C_BLACK, C_WHITE, 0);
    u64str(pit_uptime_sec(), b);
    text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
    y += 14;
    text(cx + 8, y, "ticks:", C_BLACK, C_WHITE, 0);
    u64str(pit_get_ticks(), b);
    text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
    y += 14;
    text(cx + 8, y, "fps:", C_BLACK, C_WHITE, 0);
    u64str((uint64_t)gui_fps, b);
    text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
    y += 14;
    text(cx + 8, y, "cpu%:", C_BLACK, C_WHITE, 0);
    {
        extern int cpu_percent(void);
        u64str((uint64_t)cpu_percent(), b);
    }
    text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
    y += 14;
    text(cx + 8, y, "video:", C_BLACK, C_WHITE, 0);
    u64str(fb_w, b);
    text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
    text(cx + 160, y, "x", C_BLACK, C_WHITE, 0);
    u64str(fb_h, b);
    text(cx + 175, y, b, C_BLACK, C_WHITE, 0);
    text(cx + 215, y, "x32", C_BLACK, C_WHITE, 0);
    y += 14;
    {
        extern uint64_t svga_accel_count(void);
        text(cx + 8, y, "accel fills:", C_BLACK, C_WHITE, 0);
        u64str(svga_accel_count(), b);
        text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
        y += 14;
    }
    text(cx + 8, y, "RAM util(KB):", C_BLACK, C_WHITE, 0);
    u64str(sys_mem_kb, b);
    text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
    y += 14;
    text(cx + 8, y, "fim kernel:", C_BLACK, C_WHITE, 0);
    {
        uint64_t v = (uint64_t)&_end;
        char hb[17];
        static const char* hx = "0123456789ABCDEF";
        for (int i = 0; i < 16; i++) {
            hb[i] = hx[(v >> (60 - i * 4)) & 0xF];
        }
        hb[16] = '\0';
        text(cx + 120, y, hb, C_BLACK, C_WHITE, 0);
    }
    y += 14;
    {
        uint32_t nf, nb;
        fs_stats(&nf, &nb);
        text(cx + 8, y, "overlay:", C_BLACK, C_WHITE, 0);
        u64str(nf, b);
        text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
        text(cx + 160, y, "arq", C_BLACK, C_WHITE, 0);
        u64str(nb, b);
        text(cx + 210, y, b, C_BLACK, C_WHITE, 0);
        text(cx + 270, y, "B", C_BLACK, C_WHITE, 0);
    }
    y += 20;
    text(cx + 8, y, "PID  NOME", C_BLACK, C_WHITE, 0);
    y += 14;
    text(cx + 8, y, "0    [idle]", C_BLACK, C_WHITE, 0);
    y += 14;
    text(cx + 8, y, "1    /bin/built (isn_terminal)", C_BLACK, C_WHITE, 0);
}

static void draw_disk_client(int cx, int cy, int cw, int ch)
{
    extern const void* ata_get(int idx);
    (void)cw;
    (void)ch;
    extern int svga_present_now(void);
    extern uint32_t svga_vram(void);
    extern int pci_count_all(void);
    extern uint64_t svga_accel_count(void);
    int y = cy + 6;
    text(cx + 8, y, "Gerenciador de discos", C_BLACK, C_WHITE, 0);
    y += 18;
    {
        char b[32];
        if (svga_present_now()) {
            text(cx + 8, y, "GPU: VMware SVGA II (HW)", C_BLACK, C_WHITE, 0);
            y += 14;
            text(cx + 8, y, "VRAM MB:", C_BLACK, C_WHITE, 0);
            u64str(svga_vram() / (1024 * 1024), b);
            text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
            text(cx + 160, y, "accel fills:", C_BLACK, C_WHITE, 0);
            u64str(svga_accel_count(), b);
            text(cx + 300, y, b, C_BLACK, C_WHITE, 0);
            y += 14;
        } else {
            text(cx + 8, y, "GPU: VBE generico (CPU)", C_BLACK, C_WHITE, 0);
            y += 14;
        }
        text(cx + 8, y, "PCI disp:", C_BLACK, C_WHITE, 0);
        u64str((uint64_t)pci_count_all(), b);
        text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
        y += 14;
    }
    {
        char b[32];
        text(cx + 8, y, "video:", C_BLACK, C_WHITE, 0);
        u64str(fb_w, b);
        text(cx + 120, y, b, C_BLACK, C_WHITE, 0);
        text(cx + 160, y, "x", C_BLACK, C_WHITE, 0);
        u64str(fb_h, b);
        text(cx + 175, y, b, C_BLACK, C_WHITE, 0);
        y += 14;
    }
    static const char* chn[4] = { "ide0 master", "ide0 slave ", "ide1 master", "ide1 slave " };
    for (int i = 0; i < 4; i++) {
        const uint8_t* d = (const uint8_t*)ata_get(i);
        /* layout ata_dev_t: present(int) model[41] sectors(u64) */
        int present = *(const int*)d;
        text(cx + 8, y, chn[i], C_BLACK, C_WHITE, 0);
        if (!present) {
            text(cx + 120, y, "-- vazio --", C_BLACK, C_WHITE, 0);
        } else {
            text(cx + 120, y, present == 2 ? "ATAPI" : "ATA", C_BLACK, C_WHITE, 0);
            text(cx + 170, y, (const char*)(d + 4), C_BLACK, C_WHITE, 0);
            uint64_t sec = *(const uint64_t*)(d + 4 + 44);
            if (sec) {
                char b[32];
                u64str((sec * 512) / (1024 * 1024), b);
                size_t n = strlen(b);
                b[n++] = ' ';
                b[n++] = 'M';
                b[n++] = 'B';
                b[n] = '\0';
                text(cx + 430, y, b, C_BLACK, C_WHITE, 0);
            }
        }
        y += 14;
    }
    y += 8;
    {
        char b[32];
        text(cx + 8, y, "ramfs arquivos:", C_BLACK, C_WHITE, 0);
        u64str(ramfs_count, b);
        text(cx + 200, y, b, C_BLACK, C_WHITE, 0);
        y += 14;
        uint32_t nf, nb;
        fs_stats(&nf, &nb);
        text(cx + 8, y, "overlay:", C_BLACK, C_WHITE, 0);
        u64str(nf, b);
        text(cx + 200, y, b, C_BLACK, C_WHITE, 0);
        y += 14;
        text(cx + 8, y, "RAM util(KB):", C_BLACK, C_WHITE, 0);
        u64str(sys_mem_kb, b);
        text(cx + 200, y, b, C_BLACK, C_WHITE, 0);
    }
}

/* circulo solido p/ semaforo */
static void dot(int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                px(cx + x, cy + y, c);
            }
        }
    }
}

static void draw_window(int idx)
{
    win_t* wn = &wins[idx];
    int x = wn->x, y = wn->y, w = wn->w, h = wn->h;
    int foc = (focused == idx);
    uint32_t tcol = foc ? C_TITLE : C_TINACT;
    /* sombra */
    fill_rounded_blend(x + 5, y + 7, w, h, CORNER_R, C_BLACK, 110);
    /* corpo arredondado */
    fill_rounded(x, y, w, h, CORNER_R, C_TASK);
    /* barra de titulo (topo arredondado aproximado) */
    fill_rect(x + CORNER_R, y + 2, w - 2 * CORNER_R, 3, tcol);
    fill_rect(x + 2, y + 5, w - 4, TITLE_H - 2, tcol);
    text(x + 64, y + 8, wn->title, C_WHITE, tcol, 0);
    /* semaforo: fechar, minimizar, maximizar */
    dot(x + 14, y + 13, 5, pack(255, 90, 90));
    dot(x + 32, y + 13, 5, pack(255, 190, 60));
    dot(x + 50, y + 13, 5, pack(40, 200, 80));

    int cx = x + 4, cy = y + 3 + TITLE_H + 2;
    int cw = w - 8, ch = h - (3 + TITLE_H + 2) - 4;
    fill_rect(cx, cy, cw, ch, C_WHITE);

    if (wn->type == W_TERM) {
        int sc = term_scale;
        int cell = 8 * sc;
        fill_rect(cx, cy, cw, ch, C_BLACK);
        term_cx = cx + 4;
        term_cy = cy + 4;
        term_cols = (cw - 8) / cell;
        term_rows = (ch - 8) / cell;
        if (term_cols > TERM_COLS - 1) term_cols = TERM_COLS - 1;
        if (term_cols < 8) term_cols = 8;
        if (term_rows < 4) term_rows = 4;
        int start = term_row - term_rows + 1;
        if (start < 0) start = 0;
        if (term_count - start > term_rows) start = term_count - term_rows;
        if (start < 0) start = 0;
        for (int r = 0; r < term_rows; r++) {
            int li = start + r;
            if (li < term_count) {
                const char* ln = term_lines[li];
                int cc = 0;
                while (ln[cc] && cc < term_cols) {
                    glyph_scaled(term_cx + cc * cell, term_cy + r * cell, ln[cc],
                          pack((term_fg[li][cc] >> 16) & 0xFF, (term_fg[li][cc] >> 8) & 0xFF, term_fg[li][cc] & 0xFF),
                          pack((term_bg[li][cc] >> 16) & 0xFF, (term_bg[li][cc] >> 8) & 0xFF, term_bg[li][cc] & 0xFF),
                          sc, 0);
                    cc++;
                }
            }
        }
        if (foc) {
            int cr = term_row - start;
            if (cr >= 0 && cr < term_rows && term_col < term_cols) {
                fill_rect(term_cx + term_col * cell, term_cy + cr * cell, cell, cell, C_WHITE);
            }
        }
    } else if (wn->type == W_NOTE) {
        draw_note_client(idx, cx, cy, cw, ch);
    } else if (wn->type == W_EXPL) {
        draw_expl_client(idx, cx, cy, cw, ch);
    } else if (wn->type == W_TASK) {
        draw_task_client(cx, cy, cw, ch);
    } else if (wn->type == W_DISK) {
        draw_disk_client(cx, cy, cw, ch);
    } else if (wn->type == W_CUBE) {
        draw_cube_client(cx, cy, cw, ch);
    } else if (wn->type == W_WEB) {
        extern void browser_draw(int cx, int cy, int cw, int ch, int mx, int my);
        browser_draw(cx, cy, cw, ch, mouse_x, mouse_y);
    } else if (wn->type == W_SET) {
        draw_set_client(cx, cy, cw, ch);
    } else if (wn->type == W_CALC) {
        draw_calc_client(cx, cy, cw, ch);
    } else if (wn->type == W_ABOUT) {
        draw_about_client(cx, cy, cw, ch);
    } else if (wn->type == W_CAL) {
        draw_cal_client(cx, cy, cw, ch);
    } else if (wn->type == W_CLOCK) {
        draw_clock_client(cx, cy, cw, ch);
    } else if (wn->type == W_SNAKE) {
        draw_snake_client(cx, cy, cw, ch);
    } else if (wn->type == W_MUS) {
        draw_mus_client(cx, cy, cw, ch);
    } else if (wn->type == W_MTRX) {
        draw_mtrx_client(cx, cy, cw, ch);
    } else if (wn->type == W_IMG) {
        /* imagem centralizada (1:1 se couber, senao reduzida) */
        int dw = img_w, dh = img_h, ox, oy, x, y;
        fill_rect(cx, cy, cw, ch, pack(20, 20, 28));
        if (img_ok && dw > 0 && dh > 0) {
            if (dw > cw) {
                dh = dh * cw / dw;
                dw = cw;
            }
            if (dh > ch) {
                dw = dw * ch / dh;
                dh = ch;
            }
            ox = cx + (cw - dw) / 2;
            oy = cy + (ch - dh) / 2;
            for (y = 0; y < dh; y++) {
                int sy = y * img_h / dh;
                for (x = 0; x < dw; x++) {
                    int sx = x * img_w / dw;
                    const uint8_t* p = img_px + (sy * IMG_W + sx) * 3;
                    px(ox + x, oy + y, pack(p[0], p[1], p[2]));
                }
            }
        }
        text(cx + 4, cy + ch - 12, img_path, C_WHITE, pack(20, 20, 28), 0);
    }
}

/* icones do desktop */
static const struct {
    const char* label;
    wtype_t app;
} desk_icons[] = {
    { "Terminal", W_TERM },
    { "Notepad", W_NOTE },
    { "Arquivos", W_EXPL },
    { "Web", W_WEB },
};
#define NICONS 4

static void draw_icon(int i, int selected)
{
    int x = 14, y = 14 + i * 84;
    fill_rect(x + 14, y, 32, 32, selected ? C_WHITE : C_ICONB);
    fill_rect(x + 16, y + 2, 28, 28, C_TASK);
    text(x + 22, y + 10, desk_icons[i].label[0] ? (const char[]){ desk_icons[i].label[0], '\0' } : "", C_BLACK, C_TASK, 0);
    text(x, y + 38, desk_icons[i].label, C_WHITE, pack(0, 0x80, 0x80), 0);
}

/* barra superior translucida: logo + app focado + relogio */
static void draw_menubar(void)
{
    for (int x = 0; x < (int)fb_w; x++) {
        for (int y = 0; y < MENU_H; y++) {
            px_blend(x, y, C_TASK, 170);
        }
    }
    fill_rect(0, MENU_H - 1, (int)fb_w, 1, C_SHAD);
    /* logo (circulo) */
    for (int r = -7; r <= 7; r++) {
        for (int c = -7; c <= 7; c++) {
            if (r * r + c * c <= 42) {
                px(16 + c, 12 + r, C_TITLE);
            }
        }
    }
    text(30, 8, "mapple", C_BLACK, C_TASK, 0);
    if (focused >= 0 && wins[focused].used && !wins[focused].minimized) {
        text(110, 8, wins[focused].title, C_BLACK, C_TASK, 0);
    }
    char clk[16], dt[8];
    clock_str(clk);
    date_str(dt);
    char both[32];
    size_t n = 0;
    for (size_t k = 0; dt[k] && n < sizeof(both) - 1; k++) both[n++] = dt[k];
    both[n++] = ' ';
    for (size_t k = 0; clk[k] && n < sizeof(both) - 1; k++) both[n++] = clk[k];
    both[n] = '\0';
    text((int)fb_w - 8 - (int)n * 8, 8, both, C_BLACK, C_TASK, 0);
}

/* dock inferior centralizado e translucido */
#define DOCK_N 8
static const struct {
    const char* label;
    wtype_t app;
    uint32_t color;
} dock_apps[DOCK_N] = {
    { "isn_terminal", W_TERM, 0x101010 },
    { "Notepad", W_NOTE, 0xF0F0F0 },
    { "Explorador", W_EXPL, 0x2A7AD0 },
    { "Tarefas", W_TASK, 0x2AA050 },
    { "Discos", W_DISK, 0xC0C020 },
    { "3ddd", W_CUBE, 0x8020C0 },
    { "Navegador", W_WEB, 0x2070D0 },
    { "Config", W_SET, 0x606060 },
};

static int dock_geom(int* x, int* y, int* w)
{
    int dw = DOCK_N * 56 + 20;
    *x = ((int)fb_w - dw) / 2;
    *y = (int)fb_h - DOCK_H - 8;
    *w = dw;
    return dw;
}

/* retorna indice do icone sob o ponto ou -1 */
static int dock_hit(int mx, int my)
{
    int dx, dy, dw;
    dock_geom(&dx, &dy, &dw);
    if (my < dy + 6 || my >= dy + DOCK_H - 12) {
        return -1;
    }
    for (int i = 0; i < DOCK_N; i++) {
        int ix = dx + 10 + i * 56;
        if (mx >= ix && mx < ix + 48) {
            return i;
        }
    }
    return -1;
}

static int dock_open(wtype_t app)
{
    for (int i = 0; i < MAXW; i++) {
        if (wins[i].used && wins[i].type == app) {
            return i;
        }
    }
    return -1;
}

static void draw_dock(void)
{
    int dx, dy, dw;
    dock_geom(&dx, &dy, &dw);
    fill_rounded_blend(dx, dy, dw, DOCK_H, 12, C_TASK, 150);
    for (int i = 0; i < DOCK_N; i++) {
        int ix = dx + 10 + i * 56;
        int iy = dy + 6;
        int hov = (mouse_x >= ix && mouse_x < ix + 48 &&
                   mouse_y >= iy && mouse_y < iy + 48);
        fill_rounded(ix + 4, iy, 40, 36, 6, dock_apps[i].color);
        /* letra inicial */
        char s[2] = { dock_apps[i].label[0], '\0' };
        uint32_t fg = (i == 1) ? C_BLACK : C_WHITE;
        uint32_t bg = dock_apps[i].color;
        text(ix + 4 + 12, iy + 14, s, fg, bg, 0);
        (void)hov;
        /* bolinha = app aberto */
        if (dock_open(dock_apps[i].app) >= 0) {
            for (int r = -2; r <= 2; r++) {
                for (int c = -2; c <= 2; c++) {
                    if (r * r + c * c <= 4) {
                        px(ix + 24 + c, dy + DOCK_H - 7 + r, C_WHITE);
                    }
                }
            }
        }
    }
}

/* menu suspenso a partir do logo (cima-esquerda) */
static const struct {
    const char* label;
    int action; /* 0 term 1 note 2 expl 3 task 4 disk 5 cube 6 reboot 7 halt */
    uint32_t color;
} menu_items[] = {
    { "isn_terminal", 0, 0x000000 },
    { "Notepad", 1, 0x000080 },
    { "Explorador", 2, 0x808000 },
    { "Tarefas", 3, 0x008000 },
    { "Discos", 4, 0x800000 },
    { "3ddd", 5, 0x8020C0 },
    { "Navegador", 8, 0x2070D0 },
    { "Config", 9, 0x606060 },
    { "Calc", 10, 0x208020 },
    { "Sobre", 11, 0x804020 },
    { "Calendario", 12, 0x802080 },
    { "Relogio", 13, 0x208080 },
    { "Snake", 14, 0x20A020 },
    { "Musica", 15, 0xA02080 },
    { "Matrix", 16, 0x10C010 },
    { "Reiniciar", 6, 0x404040 },
    { "Desligar", 7, 0x400000 },
};
#define NMENU 17
#define MENU_IH 26

static void menu_geom(int* mx, int* my, int* mw, int* mh)
{
    *mw = 210;
    *mh = NMENU * MENU_IH + 10;
    *mx = 4;
    *my = MENU_H + 2;
}

static void draw_menu(void)
{
    int mx, my, mw, mh;
    menu_geom(&mx, &my, &mw, &mh);
    fill_rounded_blend(mx, my, mw, mh, 8, C_TASK, 210);
    fill_rect(mx + 2, my + 2, 26, mh - 4, C_TITLE);
    const char* vert = "mapple";
    for (int i = 0; vert[i]; i++) {
        char s[2] = { vert[i], '\0' };
        text(mx + 9, my + 12 + i * 12, s, C_WHITE, C_TITLE, 0);
    }
    for (int i = 0; i < NMENU; i++) {
        int iy = my + 5 + i * MENU_IH;
        int hov = (mouse_x >= mx + 32 && mouse_x < mx + mw - 4 &&
                   mouse_y >= iy && mouse_y < iy + MENU_IH);
        if (hov) {
            fill_rect(mx + 32, iy, mw - 36, MENU_IH, C_TITLE);
        }
        fill_rect(mx + 38, iy + 5, 16, 16, menu_items[i].color);
        text(mx + 60, iy + 9, menu_items[i].label, hov ? C_WHITE : C_BLACK,
             hov ? C_TITLE : C_TASK, 0);
    }
}

static void gui_reboot(void)
{
    hw_outb(0x64, 0xFE);
    for (;;) {
        cpu_hlt();
    }
}

static void menu_action(int item)
{
    start_open = 0;
    if (item == 0) {
        gui_term_open();
    } else if (item == 1) {
        gui_note_open("/home/guest/notas.txt");
    } else if (item == 2) {
        gui_expl_open("/");
    } else if (item == 3) {
        if (win_open(W_TASK) >= 0) {
            serial_puts("gui: abrir Tarefas\n");
        }
    } else if (item == 4) {
        if (win_open(W_DISK) >= 0) {
            serial_puts("gui: abrir Discos\n");
        }
    } else if (item == 5) {
        gui_cube_open();
    } else if (item == 8) {
        gui_web_open("mapple://inicio");
    } else if (item == 9) {
        gui_set_open();
    } else if (item == 10) {
        gui_calc_open();
    } else if (item == 11) {
        gui_about_open();
    } else if (item == 12) {
        gui_cal_open();
    } else if (item == 13) {
        gui_clock_open();
    } else if (item == 14) {
        gui_snake_open();
    } else if (item == 15) {
        gui_mus_open();
    } else if (item == 16) {
        gui_mtrx_open();
    } else if (item == 6) {
        serial_puts("gui: reiniciar\n");
        gui_reboot();
    } else if (item == 7) {
        serial_puts("gui: desligar (halt)\n");
        for (;;) {
            __asm__ volatile ("cli");
            __asm__ volatile ("hlt");
        }
    }
}

/* duplo-clique: mesmo alvo em <50 ticks */
static int dbl_click(int id)
{
    uint64_t now = pit_get_ticks();
    int isdbl = (id == last_click_id && now - last_click_t < 50);
    last_click_id = id;
    last_click_t = now;
    return isdbl;
}

static void icon_open(int i)
{
    if (desk_icons[i].app == W_TERM) {
        gui_term_open();
    } else if (desk_icons[i].app == W_NOTE) {
        gui_note_open("/home/guest/notas.txt");
    } else if (desk_icons[i].app == W_EXPL) {
        gui_expl_open("/");
    } else if (desk_icons[i].app == W_WEB) {
        gui_web_open("mapple://inicio");
    }
}

static void expl_navigate(const char* sub)
{
    char np[128];
    size_t n = 0;
    size_t pn = strlen(expl_path);
    for (size_t i = 0; i < pn && n < sizeof(np) - 1; i++) {
        np[n++] = expl_path[i];
    }
    if (n == 0 || np[n - 1] != '/') {
        np[n++] = '/';
    }
    for (size_t i = 0; sub[i] && n < sizeof(np) - 1; i++) {
        np[n++] = sub[i];
    }
    np[n] = '\0';
    size_t k = 0;
    while (k < sizeof(expl_path) - 1 && np[k]) {
        expl_path[k] = np[k];
        k++;
    }
    expl_path[k] = '\0';
    expl_refresh();
    gui_mark_dirty();
}

static void expl_up(void)
{
    size_t n = strlen(expl_path);
    if (n <= 1) {
        return;
    }
    size_t cut = n - 1;
    while (cut > 0 && expl_path[cut] != '/') cut--;
    if (cut == 0) {
        expl_path[1] = '\0';
    } else {
        expl_path[cut] = '\0';
    }
    expl_refresh();
    gui_mark_dirty();
}

static int expl_row_at(int idx, int mx, int my)
{
    int cx = wins[idx].x + 4, cy = wins[idx].y + 3 + TITLE_H + 2;
    int cw = wins[idx].w - 8;
    (void)cw;
    if (mx < cx || mx >= cx + cw) {
        return -2;
    }
    /* botao Acima */
    if (mx >= cx + cw - 52 && mx < cx + cw - 4 && my >= cy + 1 && my < cy + 15) {
        return -3;
    }
    int r = (my - (cy + 20)) / 12;
    if (r < 0 || r >= expl_n) {
        return -1;
    }
    return r;
}

static int has_txt_ext(const char* name)
{
    size_t n = strlen(name);
    if (n < 4) {
        return 0;
    }
    const char* e = name + n - 4;
    return (e[0] == '.' && ((e[1] == 't' && e[2] == 'x' && e[3] == 't') ||
                            (e[1] == '.' && e[2] == 'c') ||
                            (e[1] == '.' && e[2] == 'h')));
}

/* Entrada de mouse: foco, arrasto, botoes, menu, icones, explorer. */
static void gui_mouse(void)
{
    int mx = mouse_x, my = mouse_y;
    uint8_t b = mouse_buttons;
    int pressed = (b & 1) && !(prev_btn & 1);

    if (dragging >= 0) {
        if (b & 1) {
            int nx = mx - drag_ox;
            int ny = my - drag_oy;
            if (nx < -(wins[dragging].w - 60)) {
                nx = -(wins[dragging].w - 60);
            }
            if (ny < 0) {
                ny = 0;
            }
            if (nx > (int)fb_w - 40) {
                nx = (int)fb_w - 40;
            }
            if (ny > (int)fb_h - 60) {
                ny = (int)fb_h - 60;
            }
            if (nx != wins[dragging].x || ny != wins[dragging].y) {
                /* area antiga + nova */
                gui_invalidate(wins[dragging].x - 8, wins[dragging].y - 8,
                               wins[dragging].x + wins[dragging].w + 8,
                               wins[dragging].y + wins[dragging].h + 8);
                wins[dragging].x = nx;
                wins[dragging].y = ny;
                gui_invalidate(nx - 8, ny - 8, nx + wins[dragging].w + 8,
                               ny + wins[dragging].h + 8);
            }
        } else {
            dragging = -1;
        }
        prev_btn = b;
        return;
    }

    if (!pressed) {
        prev_btn = b;
        return;
    }

    int task_y = (int)fb_h - TASK_H;

    /* menu aberto: itens primeiro */
    if (start_open) {
        int mx0, my0, mw0, mh0;
        menu_geom(&mx0, &my0, &mw0, &mh0);
        if (mx >= mx0 + 32 && mx < mx0 + mw0 - 4 && my >= my0 + 5 && my < my0 + mh0 - 5) {
            int item = (my - (my0 + 5)) / MENU_IH;
            if (item >= 0 && item < NMENU) {
                menu_action(item);
                prev_btn = b;
                return;
            }
        }
        start_open = 0;
    }

    /* logo na barra superior */
    if (mx >= 2 && mx < 30 && my >= 2 && my < MENU_H - 2) {
        if (dbl_click(1000)) {
            start_open = 0;
        } else {
            start_open = 1;
            serial_puts("gui: menu iniciar\n");
        }
        prev_btn = b;
        return;
    }

    /* dock: abre/foca/minimiza apps */
    {
        int di = dock_hit(mx, my);
        if (di >= 0) {
            int wi = win_find(dock_apps[di].app);
            if (wi < 0) {
                if (dock_apps[di].app == W_TERM) {
                    gui_term_open();
                } else if (dock_apps[di].app == W_NOTE) {
                    gui_note_open("/home/guest/notas.txt");
                } else if (dock_apps[di].app == W_EXPL) {
                    gui_expl_open("/");
                } else if (dock_apps[di].app == W_TASK) {
                    gui_task_open();
                } else if (dock_apps[di].app == W_DISK) {
                    gui_disk_open();
                } else if (dock_apps[di].app == W_CUBE) {
                    gui_cube_open();
                } else if (dock_apps[di].app == W_WEB) {
                    gui_web_open("mapple://inicio");
                } else if (dock_apps[di].app == W_SET) {
                    gui_set_open();
                }
            } else if (wins[wi].minimized) {
                win_focus(wi);
            } else if (focused == wi) {
                wins[wi].minimized = 1;
                focused = -1;
                gui_mark_dirty();
            } else {
                win_focus(wi);
            }
            prev_btn = b;
            return;
        }
    }

    /* icones do desktop (area sem janela) */
    {
        int hit = win_at(mx, my);
        if (hit < 0 && my < task_y) {
            for (int i = 0; i < NICONS; i++) {
                int ix = 14, iy = 14 + i * 84;
                if (mx >= ix && mx < ix + 76 && my >= iy && my < iy + 64) {
                    if (dbl_click(2000 + i)) {
                        icon_open(i);
                    }
                    prev_btn = b;
                    return;
                }
            }
            /* clique no desktop: desfoca */
            focused = -1;
            gui_mark_dirty();
            prev_btn = b;
            return;
        }
    }

    /* janelas */
    {
        int hit = win_at(mx, my);
        if (hit >= 0) {
            if (focused != hit) {
                win_focus(hit);
            }
            if (in_close(hit, mx, my)) {
                serial_puts("gui: fechar janela\n");
                win_close(hit);
                prev_btn = b;
                return;
            }
            if (in_min(hit, mx, my)) {
                wins[hit].minimized = 1;
                focused = -1;
                gui_mark_dirty();
                prev_btn = b;
                return;
            }
            if (in_max(hit, mx, my)) {
                win_toggle_max(hit);
                win_focus(hit);
                prev_btn = b;
                return;
            }
            if (in_title(hit, mx, my)) {
                dragging = hit;
                drag_ox = mx - wins[hit].x;
                drag_oy = my - wins[hit].y;
                serial_puts("gui: arrastar janela\n");
                prev_btn = b;
                return;
            }
            /* configuracoes: linhas clicaveis (titulo em cy+6,
             * opcoes a cada 18px a partir de cy+26) */
            if (wins[hit].type == W_SET) {
                int cy = wins[hit].y + 3 + TITLE_H + 2;
                int r = (my >= cy + 26) ? (my - (cy + 26)) / 18 : -1;
                if (r == 0) {
                    set_opt_next();
                } else if (r == 1) {
                    res_apply((cfg_res + 1) % 3);
                } else if (r == 2) {
                    font_apply((cfg_font + 1) % 3);
                }
                prev_btn = b;
                return;
            }
            /* navegador: links e barra */
            if (wins[hit].type == W_WEB) {
                extern int browser_click(int lx, int ly, int cw, int ch);
                int cx = wins[hit].x + 4;
                int cy = wins[hit].y + 3 + TITLE_H + 2;
                int cw = wins[hit].w - 8;
                int ch = wins[hit].h - (3 + TITLE_H + 2) - 4;
                browser_click(mx - cx, my - cy, cw, ch);
                prev_btn = b;
                return;
            }
            /* calc: botoes */
            if (wins[hit].type == W_CALC) {
                int cx = wins[hit].x + 4;
                int cy = wins[hit].y + 3 + TITLE_H + 2;
                int cw = wins[hit].w - 8;
                int ch = wins[hit].h - (3 + TITLE_H + 2) - 4;
                int r, c;
                for (r = 0; r < 4; r++) {
                    for (c = 0; c < 4; c++) {
                        int bx = cx + 8 + c * ((cw - 16) / 4);
                        int by = cy + 32 + r * ((ch - 40) / 4);
                        int bw = (cw - 16) / 4 - 4;
                        int bh = (ch - 40) / 4 - 4;
                        if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                            char k = calc_keys[r][c];
                            if (k == 'C') {
                                calc_in[0] = '\0';
                                calc_show = 0;
                            } else if (k == '=') {
                                calc_eval();
                            } else {
                                size_t n = strlen(calc_in);
                                if (n + 1 < sizeof(calc_in)) {
                                    calc_in[n] = k;
                                    calc_in[n + 1] = '\0';
                                    calc_show = 0;
                                }
                            }
                            gui_dirty = 1;
                            prev_btn = b;
                            return;
                        }
                    }
                }
            }
            /* calendario: < > */
            if (wins[hit].type == W_CAL) {
                int cx = wins[hit].x + 4;
                int cy = wins[hit].y + 3 + TITLE_H + 2;
                if (my >= cy + 6 && my < cy + 22) {
                    if (mx >= cx + 8 && mx < cx + 24) {
                        cal_m--;
                        if (cal_m < 1) {
                            cal_m = 12;
                            cal_y--;
                        }
                        gui_dirty = 1;
                        prev_btn = b;
                        return;
                    }
                    if (mx >= cx + 280 && mx < cx + 296) {
                        cal_m++;
                        if (cal_m > 12) {
                            cal_m = 1;
                            cal_y++;
                        }
                        gui_dirty = 1;
                        prev_btn = b;
                        return;
                    }
                }
            }
            /* musica: linhas */
            if (wins[hit].type == W_MUS) {
                int cx = wins[hit].x + 4;
                int cy = wins[hit].y + 3 + TITLE_H + 2;
                int r = (my - (cy + 28)) / 20;
                if (r >= 0 && r < NMUS && mx >= cx && mx < cx + wins[hit].w - 8) {
                    mus_play(r);
                    prev_btn = b;
                    return;
                }
            }
            /* explorer: linhas e botao Acima */
            if (wins[hit].type == W_EXPL) {
                int r = expl_row_at(hit, mx, my);
                if (r == -3) {
                    expl_up();
                } else if (r >= 0) {
                    expl_sel = r;
                    gui_mark_dirty();
                    if (dbl_click(3000 + r)) {
                        if (expl_isdir[r]) {
                            expl_navigate(expl_names[r]);
                        } else if (has_txt_ext(expl_names[r])) {
                            char fp[192];
                            size_t n = 0;
                            size_t pn = strlen(expl_path);
                            for (size_t k = 0; k < pn && n < sizeof(fp) - 1; k++) {
                                fp[n++] = expl_path[k];
                            }
                            if (n == 0 || fp[n - 1] != '/') {
                                fp[n++] = '/';
                            }
                            for (size_t k = 0; expl_names[r][k] && n < sizeof(fp) - 1; k++) {
                                fp[n++] = expl_names[r][k];
                            }
                            fp[n] = '\0';
                            gui_note_open(fp);
                        }
                    }
                }
            }
        }
    }
    prev_btn = b;
}

static void gui_draw(void)
{
    int px0, py0, px1, py1;
    if (svga_direct) {
        vsync_wait(); /* desenha direto na VRAM: sincroniza antes */
    }
    /* define clip a partir do dirty rect (ou tela cheia) */
    if (full_draw || !dirty_on) {
        clip_on = 0;
        px0 = 0;
        py0 = 0;
        px1 = (int)fb_w;
        py1 = (int)fb_h;
    } else {
        clip_on = 1;
        clip_x0 = dirty_x0;
        clip_y0 = dirty_y0;
        clip_x1 = dirty_x1;
        clip_y1 = dirty_y1;
        px0 = dirty_x0;
        py0 = dirty_y0;
        px1 = dirty_x1;
        py1 = dirty_y1;
    }
    desktop_draw();

    if (gui_chrome) {
        for (int i = 0; i < NICONS; i++) {
            draw_icon(i, 0);
        }
        for (int z = 0; z < zcount; z++) {
            int i = zorder[z];
            if (wins[i].used && !wins[i].minimized) {
                draw_window(i);
            }
        }
        if (start_open) {
            draw_menu();
        }
        if (switcher_on) {
            draw_switcher();
        }
        draw_menubar();
        draw_dock();
    } else {
        int sc = term_scale;
        int cell = 8 * sc;
        term_cx = 8;
        term_cy = 8;
        term_cols = ((int)fb_w - 16) / cell;
        term_rows = ((int)fb_h - 16) / cell;
        if (term_cols > TERM_COLS - 1) term_cols = TERM_COLS - 1;
        if (term_cols < 8) term_cols = 8;
        if (term_rows < 4) term_rows = 4;
        int start = term_row - term_rows + 1;
        if (start < 0) start = 0;
        if (term_count - start > term_rows) start = term_count - term_rows;
        if (start < 0) start = 0;
        for (int r = 0; r < term_rows; r++) {
            int li = start + r;
            if (li < term_count) {
                const char* ln = term_lines[li];
                int cc = 0;
                while (ln[cc] && cc < term_cols) {
                    glyph_scaled(term_cx + cc * cell, term_cy + r * cell, ln[cc],
                          pack((term_fg[li][cc] >> 16) & 0xFF, (term_fg[li][cc] >> 8) & 0xFF, term_fg[li][cc] & 0xFF),
                          pack((term_bg[li][cc] >> 16) & 0xFF, (term_bg[li][cc] >> 8) & 0xFF, term_bg[li][cc] & 0xFF),
                          sc, 0);
                    cc++;
                }
            }
        }
        {
            int cr = term_row - start;
            if (cr >= 0 && cr < term_rows && term_col < term_cols) {
                fill_rect(term_cx + term_col * cell, term_cy + cr * cell, cell, cell, C_WHITE);
            }
        }
    }

    /* cursor HW desabilitado por enquanto (DEFINE_CURSOR nao confere
     * neste QEMU); software sempre. */
    draw_cursor(mouse_x, mouse_y);
    clip_on = 0;
    gui_dirty = 0;
    dirty_on = 0;
    full_draw = 0;
    fb_present_rect(px0, py0, px1, py1);
}

/* Hotkeys globais vindos pela fila de apps (ex: Alt+Tab com o
 * terminal desfocado, quando o shell nao esta consumindo teclas). */
/* remove a primeira ocorrencia de c da fila; 1 se achou */
static int appq_take(char c)
{
    uint8_t t = appq_t;
    while (t != appq_h) {
        if (appq[t] == c) {
            uint8_t u = t;
            uint8_t nx = (uint8_t)(u + 1);
            while (nx != appq_h) {
                appq[u] = appq[nx];
                u = nx;
                nx = (uint8_t)(nx + 1);
            }
            appq_h = (uint8_t)(appq_h - 1);
            return 1;
        }
        t = (uint8_t)(t + 1);
    }
    return 0;
}

static void app_hotkeys(void)
{
    if (appq_take((char)0x88)) {
        gui_cycle_windows();
        gui_mark_dirty();
        return;
    }
    if (switcher_on) {
        if (appq_take('\t')) {
            gui_switcher_next();
            return;
        }
        if (appq_take('\n')) {
            gui_switcher_confirm();
            return;
        }
        if (appq_take(0x1B)) {
            gui_switcher_cancel();
            return;
        }
    }
}

/* Frame pacing: no maximo ~50fps; conta FPS real. */
void gui_poll(void)
{
    if (!gui_active) {
        return;
    }
    if (gui_editor) {
        return; /* editor tela-cheia comanda o front */
    }
    app_hotkeys();
    /* migra p/ GPU quando o acelerador aparece (uma vez) */
    if (!svga_direct && svga_accel_on()) {
        svga_direct = 1;
        draw = fb;
        draw_stride = fb_pitch_px;
        gui_mark_dirty();
        serial_puts("gui: backend GPU direto (fills+cursor)\n");
    }
    {
        static int last_cx = -100, last_cy = -100;
        if (mouse_dirty) {
            mouse_dirty = 0;
            /* cursor antigo + novo */
            gui_invalidate(last_cx - 1, last_cy - 1, last_cx + 17, last_cy + 17);
            gui_invalidate(mouse_x - 1, mouse_y - 1, mouse_x + 17, mouse_y + 17);
            last_cx = mouse_x;
            last_cy = mouse_y;
            if (start_open) {
                int mx0, my0, mw0, mh0;
                menu_geom(&mx0, &my0, &mw0, &mh0);
                gui_invalidate(mx0, my0, mx0 + mw0, my0 + mh0);
            }
            if (svga_cursor_on()) {
                svga_cursor_move(mouse_x, mouse_y);
            }
        }
    }
    uint64_t now = pit_get_ticks();
    uint64_t s = pit_uptime_sec();
    {
        extern int cpu_update(void);
        cpu_update(); /* % CPU 1x/seg */
    }
    if (s != last_sec) {
        last_sec = s;
        /* relogio + apps live */
        gui_invalidate((int)fb_w - 140, 0, (int)fb_w, MENU_H);
        {
            int ti = win_find(W_TASK);
            if (ti >= 0 && !wins[ti].minimized) {
                gui_invalidate(wins[ti].x, wins[ti].y, wins[ti].x + wins[ti].w,
                               wins[ti].y + wins[ti].h);
            }
            ti = win_find(W_DISK);
            if (ti >= 0 && !wins[ti].minimized) {
                gui_invalidate(wins[ti].x, wins[ti].y, wins[ti].x + wins[ti].w,
                               wins[ti].y + wins[ti].h);
            }
        }
    }
    /* 3ddd aberto e visivel: anima todo frame (so o ret da janela) */
    {
        int ci = win_find(W_CUBE);
        if (ci >= 0 && !wins[ci].minimized) {
            gui_invalidate(wins[ci].x - 8, wins[ci].y - 8,
                           wins[ci].x + wins[ci].w + 8,
                           wins[ci].y + wins[ci].h + 8);
        }
    }
    /* switcher: confirma por timeout */
    if (switcher_on && pit_get_ticks() >= switcher_deadline) {
        gui_switcher_confirm();
    }
    if (!gui_dirty) {
        return;
    }
    if (now - last_frame < 2) {
        return; /* estabilizador: coalesce p/ ~50fps */
    }
    last_frame = now;
    fps_frames++;
    if (now - fps_t0 >= 100) {
        gui_fps = (int)(fps_frames * 100 / (now - fps_t0));
        fps_frames = 0;
        fps_t0 = now;
    }
    gui_mouse();
    gui_draw();
}

void gui_set_chrome(int on)
{
    gui_chrome = on ? 1 : 0;
    gui_mark_dirty();
}

int gui_get_chrome(void)
{
    return gui_chrome;
}

int gui_get_fps(void)
{
    return gui_fps;
}

void gui_get_res(uint32_t* w, uint32_t* h)
{
    if (w) *w = fb_w;
    if (h) *h = fb_h;
}

/* --- Tela de texto p/ o editor --- */
static int ts_cur_r = 0, ts_cur_c = 0;

void ts_taskbar(int on)
{
    if (!gui_active) {
        vga_taskbar_enable(on);
    }
}

void gui_editor_enter(void)
{
    gui_editor = 1;
}

void gui_editor_exit(void)
{
    gui_editor = 0;
    gui_mark_dirty();
}

void ts_clear(void)
{
    if (!gui_active) {
        vga_clear_screen();
        return;
    }
    /* editor desenha direto no front (imediato, sem WM) */
    uint32_t c = pack(0, 0, 0);
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t* row = fb + y * fb_pitch_px;
        for (uint32_t x = 0; x < fb_w; x++) {
            row[x] = c;
        }
    }
    ts_cur_r = 0;
    ts_cur_c = 0;
}

void ts_write_at(int row, int col, const char* s, uint8_t fg, uint8_t bg)
{
    if (!gui_active) {
        vga_write_at(row, col, s, fg, bg);
        return;
    }
    uint32_t f = pack(vga16[fg & 15][0], vga16[fg & 15][1], vga16[fg & 15][2]);
    uint32_t b = pack(vga16[bg & 15][0], vga16[bg & 15][1], vga16[bg & 15][2]);
    int x = col * 8, y = row * 8;
    while (*s && x + 8 <= (int)fb_w && y + 8 <= (int)fb_h) {
        fglyph(x, y, *s, f, b);
        x += 8;
        s++;
    }
}

void ts_move(int row, int col)
{
    if (!gui_active) {
        vga_move_cursor(row, col);
        return;
    }
    ts_cur_r = row;
    ts_cur_c = col;
    uint32_t c = pack(255, 255, 255);
    for (int r = 0; r < 8; r++) {
        for (int cc = 0; cc < 8; cc++) {
            fpx(col * 8 + cc, row * 8 + r, c);
        }
    }
}
