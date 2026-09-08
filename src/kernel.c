/* kernel.c - kmain(magic, mbi): GUI (framebuffer+mouse) ou texto. */
#include <stdint.h>
#include <stddef.h>

/* Assembly (boot.s) */
extern void set_idt_entry(uint8_t index, uint64_t handler_address, uint8_t type_attr);
extern void _load_idt(void);
extern void default_isr_stub(void);
extern void pit_isr_stub(void);
extern void keyboard_isr_stub(void);
extern void mouse_isr_stub(void);

/* Drivers */
extern void vga_init(void);
extern void vga_clear_screen(void);
extern void vga_put_char(char c);
extern void vga_print_string(const char* str);
extern void keyboard_init(void);
extern char keyboard_getchar(void);
extern void pic_send_eoi(uint8_t irq);
extern void pit_init(void);
extern void fs_init(void);
extern void ata_probe(void);
extern void pci_scan(void);
extern int svga_init(void);
extern int svga_cursor_init(void);
extern void shell_run(void);
/* GUI */
extern int gui_init(uint32_t magic, uint32_t mbi);
extern void mouse_init(void);
extern void mouse_handler(void);
extern int gui_active;
extern void term_putc(char c);
extern void gui_mark_dirty(void);
extern void gui_poll(void);

/* Vetores apos o remap do PIC (0x20/0x28) */
#define IRQ0_TIMER      0x20
#define IRQ1_KEYBOARD   0x21
#define IRQ12_MOUSE     0x2C

/* Gate de interrupcao 64-bit: P=1, DPL=0, tipo 0xE */
#define IDT_TYPE_INT_GATE 0x8E

void default_interrupt_handler(void)
{
    /* IRQ fantasma/excecao sem handler dedicado: so retorna. */
}

/* --- Serial COM1 (debug/teste: QEMU -serial stdio) --- */
static inline void serial_outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t serial_inb(uint16_t port)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void serial_init(void)
{
    serial_outb(0x3FB, 0x80);
    serial_outb(0x3F8, 0x03); serial_outb(0x3F9, 0x00);
    serial_outb(0x3FB, 0x03);
    serial_outb(0x3FA, 0xC7);
    serial_outb(0x3FC, 0x0B);
}

void serial_putc(char c)
{
    while (!(serial_inb(0x3FD) & 0x20)) {}
    serial_outb(0x3F8, (uint8_t)c);
}

void serial_puts(const char* s)
{
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

/* --- Console: VGA-texto ou isn_terminal + sempre serial --- */
static char* cap_buf = 0;
static size_t cap_cap = 0;
static size_t cap_len = 0;

void console_capture_begin(char* buf, size_t cap)
{
    cap_buf = buf;
    cap_cap = cap;
    cap_len = 0;
}

size_t console_capture_end(void)
{
    size_t n = cap_len;
    cap_buf = 0;
    return n;
}

static void console_serial(char c)
{
    if (c == '\n') {
        while (!(serial_inb(0x3FD) & 0x20)) {}
        serial_outb(0x3F8, (uint8_t)'\r');
    }
    while (!(serial_inb(0x3FD) & 0x20)) {}
    serial_outb(0x3F8, (uint8_t)c);
}

/* ANSI SGR (cores 32-bit): parser minimo no console */
static const uint32_t ansi16[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
};
static int ansi_st = 0; /* 0 normal, 1 ESC, 2 CSI */
static int ansi_params[8];
static int ansi_np = 0;
static int ansi_cur = 0;
static int ansi_has = 0;

extern void vga_set_attr(uint8_t fg, uint8_t bg);
extern void term_set_attr(uint32_t fg, uint32_t bg);
extern void term_attr_reset(void);

static uint8_t rgb_to_vga(uint32_t rgb)
{
    int i, best = 0, bd = -1;
    int r = (int)((rgb >> 16) & 0xFF);
    int g = (int)((rgb >> 8) & 0xFF);
    int b = (int)(rgb & 0xFF);
    static const uint8_t pal[16][3] = {
        {0,0,0},{170,0,0},{0,170,0},{170,85,0},
        {0,0,170},{170,0,170},{0,170,170},{170,170,170},
        {85,85,85},{255,85,85},{85,255,85},{255,255,85},
        {85,85,255},{255,85,255},{85,255,255},{255,255,255}
    };
    for (i = 0; i < 16; i++) {
        int dr = r - pal[i][0];
        int dg = g - pal[i][1];
        int db = b - pal[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (bd < 0 || d < bd) {
            bd = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

static uint32_t con_fg = 0xFFFFFF;
static uint32_t con_bg = 0x000000;

static void ansi_apply(void)
{
    int i = 0;
    if (ansi_np == 0) {
        con_fg = 0xFFFFFF;
        con_bg = 0x000000;
    }
    while (i < ansi_np) {
        int p = ansi_params[i];
        if (p == 0) {
            con_fg = 0xFFFFFF;
            con_bg = 0x000000;
        } else if (p == 1) {
            /* bold: ignora (poderia clarear) */
        } else if (p >= 30 && p <= 37) {
            con_fg = ansi16[p - 30];
        } else if (p >= 90 && p <= 97) {
            con_fg = ansi16[p - 90 + 8];
        } else if (p >= 40 && p <= 47) {
            con_bg = ansi16[p - 40];
        } else if (p >= 100 && p <= 107) {
            con_bg = ansi16[p - 100 + 8];
        } else if ((p == 38 || p == 48) && i + 5 <= ansi_np &&
                   ansi_params[i + 1] == 2) {
            uint32_t c = ((uint32_t)ansi_params[i + 2] << 16) |
                         ((uint32_t)ansi_params[i + 3] << 8) |
                         (uint32_t)ansi_params[i + 4];
            if (p == 38) {
                con_fg = c;
            } else {
                con_bg = c;
            }
            i += 4;
        }
        i++;
    }
    vga_set_attr(rgb_to_vga(con_fg), rgb_to_vga(con_bg));
    term_set_attr(con_fg, con_bg);
}

static int console_esc(char c)
{
    if (ansi_st == 0) {
        if (c == '\x1B') {
            ansi_st = 1;
            return 1;
        }
        return 0;
    }
    if (ansi_st == 1) {
        if (c == '[') {
            ansi_st = 2;
            ansi_np = 0;
            ansi_cur = 0;
            ansi_has = 0;
            return 1;
        }
        ansi_st = 0; /* ESC isolado: ignora */
        return 1;
    }
    /* ansi_st == 2: parametros */
    if (c >= '0' && c <= '9') {
        ansi_cur = ansi_cur * 10 + (c - '0');
        ansi_has = 1;
        return 1;
    }
    if (c == ';') {
        if (ansi_np < 8) {
            ansi_params[ansi_np++] = ansi_has ? ansi_cur : 0;
        }
        ansi_cur = 0;
        ansi_has = 0;
        return 1;
    }
    if (c == 'm') {
        if (ansi_np < 8) {
            ansi_params[ansi_np++] = ansi_has ? ansi_cur : 0;
        }
        ansi_apply();
        ansi_st = 0;
        return 1;
    }
    /* outra sequencia: descarta */
    ansi_st = 0;
    return 1;
}

void console_putc(char c)
{
    if (cap_buf) {
        if (cap_len + 1 < cap_cap) {
            cap_buf[cap_len++] = c;
            cap_buf[cap_len] = '\0';
        }
        return;
    }
    if (console_esc(c)) {
        return; /* sequencia ANSI consumida (nao vai p/ serial) */
    }
    if (gui_active) {
        term_putc(c); /* invalida so o ret do terminal */
    } else {
        vga_put_char(c);
    }
    console_serial(c);
}

void console_puts(const char* s)
{
    while (*s) {
        console_putc(*s++);
    }
}

void console_print_u64(uint64_t v)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) {
        console_putc('0');
        return;
    }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    console_puts(&buf[i]);
}

void console_print_i64(int64_t v)
{
    if (v < 0) {
        console_putc('-');
        uint64_t u = (uint64_t)(-(v + 1)) + 1;
        char buf[21];
        int i = 20;
        buf[i] = '\0';
        if (u == 0) {
            console_putc('0');
            return;
        }
        while (u > 0 && i > 0) {
            buf[--i] = (char)('0' + (u % 10));
            u /= 10;
        }
        console_puts(&buf[i]);
        return;
    }
    console_print_u64((uint64_t)v);
}

void console_print_hex(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        console_putc(hex[(v >> i) & 0xF]);
    }
}

static void setup_idt(void)
{
    for (int i = 0; i < 256; i++) {
        set_idt_entry((uint8_t)i, (uint64_t)default_isr_stub, IDT_TYPE_INT_GATE);
    }
    set_idt_entry(IRQ0_TIMER, (uint64_t)pit_isr_stub, IDT_TYPE_INT_GATE);
    set_idt_entry(IRQ1_KEYBOARD, (uint64_t)keyboard_isr_stub, IDT_TYPE_INT_GATE);
    set_idt_entry(IRQ12_MOUSE, (uint64_t)mouse_isr_stub, IDT_TYPE_INT_GATE);
    _load_idt();
}

uint64_t sys_mem_kb = 0; /* RAM utilizavel (soma do mmap tipo 1) */

static void mem_parse_mmap(uint32_t magic, uint32_t mbi)
{
    if (magic != 0x2BADB002 || mbi == 0) {
        return;
    }
    uint32_t flags = *(volatile uint32_t*)(uintptr_t)mbi;
    if (!(flags & (1u << 6))) {
        return;
    }
    uint32_t len = *(volatile uint32_t*)(uintptr_t)(mbi + 44);
    uint32_t addr = *(volatile uint32_t*)(uintptr_t)(mbi + 48);
    uint32_t off = 0;
    while (off < len) {
        volatile uint8_t* e = (volatile uint8_t*)(uintptr_t)(addr + off);
        uint32_t sz = *(volatile uint32_t*)e;
        uint64_t base = *(volatile uint64_t*)(e + 4);
        uint64_t blen = *(volatile uint64_t*)(e + 12);
        uint32_t type = *(volatile uint32_t*)(e + 20);
        (void)base;
        if (type == 1) {
            sys_mem_kb += blen / 1024;
        }
        off += sz + 4;
    }
}

extern void cxx_smoke(uint32_t magic);
extern void drv_ready(const char* name, int ok);

void kmain(uint32_t magic, uint32_t mbi)
{
    serial_init();
    drv_ready("serial", 1);
    serial_puts("mapple 0.13.0 boot\n");
    /* logo MG no boot (console + serial) */
    console_puts("\x1B[92m#     #  #####\n");
    console_puts("##   ## ##   ##\n");
    console_puts("# # # # ##     mapple 0.13.0\n");
    console_puts("#  #  # ##  ###  kernel em C/C++/asm\n");
    console_puts("#     # ##   ##\n");
    console_puts("#     #  #####\x1B[0m\n");
    cxx_smoke(magic);
    mem_parse_mmap(magic, mbi);

    setup_idt();
    keyboard_init();    /* remap PIC + IRQ1 */
    drv_ready("keyboard", 1);
    drv_ready("pic", 1);
    pit_init();         /* 100 Hz + IRQ0 */
    drv_ready("pit", 1);
    drv_ready("rtc", 1);
    drv_ready("font", 1);
    drv_ready("cc", 1);

    /* tenta modo grafico; sem framebuffer segue em texto */
    if (gui_init(magic, mbi) == 0) {
        serial_puts("kmain: modo grafico\n");
        drv_ready("fbcon", 1);
        drv_ready("gui-wm", 1);
    } else {
        serial_puts("kmain: modo texto\n");
        vga_init();
        vga_clear_screen();
        drv_ready("vga", 1);
    }

    fs_init();
    drv_ready("ramfs", 1);

    __asm__ volatile ("sti");

    ata_probe(); /* IDE (log serial; sem IRQ) */
    drv_ready("ata", 1);
    pci_scan();  /* PCI (log serial) */
    drv_ready("pci", 1);
    {
        extern void hwdetect_init(void);
        hwdetect_init(); /* CPU/ACPI/PCI-classe/legacy (log serial) */
    }
    {
        extern int e1000_init(void);
        extern int pci_find(uint16_t vendor, uint16_t device);
        extern void drv_absent(const char* name);
        if (pci_find(0x8086, 0x100E) < 0) {
            drv_absent("e1000");
            drv_absent("net");
        } else {
            int ok = (e1000_init() == 0);
            drv_ready("e1000", ok);
            drv_ready("net", ok);
        }
    }

    if (gui_active) {
        /* SVGA: deteccao + VRAM direta. FIFO/cursor HW ficam p/ revisao
         * (QEMU 10.2 nao responde aos comandos no formato implementado). */
        {
            extern int pci_find(uint16_t vendor, uint16_t device);
            extern void drv_absent(const char* name);
            if (pci_find(0x15AD, 0x0405) < 0) {
                drv_absent("svga");
            } else {
                drv_ready("svga", svga_init() == 0);
            }
        }
        mouse_init();   /* IRQ12 (precisa do PIT => sti ligado) */
        drv_ready("mouse", 1);
        drv_ready("pcspk", 1);
        extern void gui_term_open(void);
        gui_term_open(); /* janela inicial */
    } else {
        drv_ready("vga", 1);
        drv_ready("pcspk", 1);
    }

    gui_poll(); /* primeiro frame */
    console_puts("Boot OK. isn_terminal pronto.\n");
    shell_run();        /* nao retorna */

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
