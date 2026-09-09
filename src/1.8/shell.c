/* built.c (shell.c) - Shell padrao do mapple: /bin/built.
 *
 * Multi-call estilo busybox: applets resolvidos via PATH /bin:/usr/bin.
 * FS gravavel com cwd (cd/rm/touch/mkdir), redirect `>`/`>>`,
 * historico, tema Win98 (desktop teal + taskbar + relogio).
 */
#include <stdint.h>
#include <stddef.h>

/* libc propria */
extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, size_t n);
extern char* strcpy(char* dst, const char* src);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void* memset(void* dst, int c, size_t n);
extern int atoi(const char* s);
extern size_t utoa64(uint64_t v, char* buf, size_t cap);

/* Console (kernel.c): VGA + serial */
extern void console_putc(char c);
extern void console_puts(const char* s);
extern void console_print_u64(uint64_t v);
extern void console_print_i64(int64_t v);
extern void console_print_hex(uint64_t v);
extern void console_capture_begin(char* buf, size_t cap);
extern size_t console_capture_end(void);

/* Drivers / kernel */
extern char keyboard_getchar(void);
extern char keyboard_getraw(void);
extern void gui_cycle_windows(void);
extern void pcspk_beep(uint32_t hz, uint32_t ms);
extern void pcspk_play(const char* seq);
extern void audio_beep(uint32_t hz, uint32_t ms);
extern void audio_play(const char* seq);
extern void audio_info(void);
extern int audio_get_volume(void);
extern int audio_is_muted(void);
extern void audio_set_volume(int v);
extern void audio_set_mute(int m);
extern void usb_list(void);
extern int usb_mount(void);
extern int bright_get(void);
extern void bright_set(int v);
extern int vfs_mount(const char* fstype, const char* point);
extern int vfs_umount(const char* point);
extern void vfs_list_mounts(void (*cb)(const char*, const char*, void*),
                            void* ctx);
extern void ping_cmd(const char* args);
extern void netcfg_cmd(const char* args);
extern int gui_switcher_open(void);
extern void gui_switcher_next(void);
extern void gui_switcher_confirm(void);
extern void gui_switcher_cancel(void);
extern void cpu_hlt(void);
extern void vga_set_color(uint8_t fg, uint8_t bg);
extern void term_clear(void);
extern void* memmove(void* dst, const void* src, size_t n);
static void sh_readline(char* line, size_t cap, int nested);

/* teclas especiais (ver keyboard.c) */
#define KEY_UP    ((char)0x81)
#define KEY_DOWN  ((char)0x82)
#define KEY_LEFT  ((char)0x83)
#define KEY_RIGHT ((char)0x84)
#define KEY_DEL   ((char)0x85)
#define KEY_HOME  ((char)0x86)
#define KEY_END   ((char)0x87)
#define KEY_SWITCH ((char)0x88)
extern int gui_active;
extern void gui_set_chrome(int on);
extern void gui_poll(void);
extern void gui_note_open(const char* path);
extern void gui_expl_open(const char* path);
extern void gui_term_open(void);
extern int gui_note_modal(void);
extern void note_run_loop(void);
extern void vga_clear_screen(void);
extern void vga_set_color(uint8_t fg, uint8_t bg);
extern void vga_write_at(int row, int col, const char* s, uint8_t fg, uint8_t bg);
extern void vga_fill_row(int row, uint8_t fg, uint8_t bg);
extern void vga_move_cursor(int row, int col);
extern void vga_taskbar_enable(int on);
extern void vga_draw_taskbar(const char* left, const char* right);
extern uint64_t pit_uptime_sec(void);
extern uint64_t pit_get_ticks(void);
extern char _end;

/* FS */
extern void fs_resolve(const char* cwd, const char* in, char* out, size_t cap);
extern int fs_isdir(const char* path);
extern int fs_exists(const char* path);
extern int fs_read(const char* path, uint8_t* buf, size_t cap);
extern int fs_write(const char* path, const uint8_t* data, size_t size);
extern int fs_touch(const char* path);
extern int fs_mkdir(const char* path);
extern int fs_rm(const char* path);
extern void fs_list(const char* dir, void (*cb)(const char*, int, uint32_t, void*), void* ctx);

/* Editor + compilador */
extern void edit_file(const char* path);
extern int cc_run_file(const char* path);
extern int cc_eval_expr(const char* s, int64_t* out);

#define SHELL_MAX_LINE 128
#define HIST_MAX 16

static char sh_cwd[128] = "/";
static char sh_hist[HIST_MAX][SHELL_MAX_LINE];
static uint32_t sh_hcount = 0;
static int sh_theme98 = 1;
static int sh_exit_flag = 0;

int shell_win98(void)
{
    return sh_theme98;
}

/* Tabela de applets: nome -> diretorio do PATH. */
typedef struct {
    const char* name;
    const char* dir;
} applet_t;

static const applet_t applets[] = {
    { "sh", "/bin" }, { "help", "/bin" }, { "echo", "/bin" },
    { "ls", "/bin" }, { "cat", "/bin" }, { "cd", "/bin" },
    { "pwd", "/bin" }, { "rm", "/bin" }, { "mkdir", "/bin" },
    { "touch", "/bin" }, { "cp", "/bin" }, { "mv", "/bin" },
    { "clear", "/bin" }, { "uname", "/bin" }, { "whoami", "/bin" },
    { "sleep", "/bin" }, { "reboot", "/bin" }, { "halt", "/bin" },
    { "edit", "/bin" }, { "busybox", "/bin" }, { "exit", "/bin" },
    { "isn_terminal", "/bin" }, { "notepad", "/bin" }, { "explorer", "/bin" },
    { "taskmgr", "/bin" }, { "disks", "/bin" }, { "winfo", "/bin" },
    { "3ddd", "/bin" }, { "fps", "/usr/bin" }, { "browser", "/bin" },
    { "beep", "/bin" }, { "play", "/bin" }, { "ping", "/bin" },
    { "netcfg", "/bin" }, { "img", "/bin" }, { "config", "/bin" },
    { "drivers", "/bin" }, { "calc", "/bin" }, { "sobre", "/bin" },
    { "cal", "/bin" }, { "relogio", "/bin" }, { "snake", "/bin" },
    { "musica", "/bin" },
    { "ver", "/usr/bin" }, { "uptime", "/usr/bin" }, { "mem", "/usr/bin" },
    { "color", "/usr/bin" }, { "head", "/usr/bin" }, { "wc", "/usr/bin" },
    { "grep", "/usr/bin" }, { "which", "/usr/bin" }, { "man", "/usr/bin" },
    { "env", "/usr/bin" }, { "history", "/usr/bin" }, { "date", "/usr/bin" },
    { "calc", "/usr/bin" }, { "hexdump", "/usr/bin" }, { "cc", "/usr/bin" },
    { "ps", "/usr/bin" }, { "theme", "/usr/bin" },
    { "cpuinfo", "/usr/bin" }, { "reboot", "/usr/bin" },
    { "fetch", "/usr/bin" }, { "matrix", "/usr/bin" },
    { "usb", "/bin" }, { "usbmount", "/bin" }, { "vol", "/bin" },
    { "mute", "/bin" }, { "audioinfo", "/bin" }, { "brilho", "/bin" },
    { "brightness", "/bin" },
    { "foon", "/bin" }, { "pong", "/bin" }, { "breakout", "/bin" },
    { "tetris", "/bin" }, { "torus", "/bin" }, { "terreno", "/bin" },
    { "tunel", "/bin" },
    { "mount", "/bin" }, { "umount", "/bin" }, { "df", "/usr/bin" },
    { "install", "/usr/bin" }, { "atatest", "/bin" },
};
#define APPLET_COUNT (sizeof(applets) / sizeof(applets[0]))

/* decimal ate 2^32-1 (para na primeira nao-digito); ok=0 se vazio */
static uint32_t sh_parse_u32(const char* s, int* ok)
{
    uint64_t v = 0;
    *ok = 0;
    if (!*s) {
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFull) {
            return 0;
        }
        s++;
    }
    *ok = 1;
    return (uint32_t)v;
}

static const applet_t* applet_find(const char* name)
{
    for (size_t i = 0; i < APPLET_COUNT; i++) {
        if (strcmp(applets[i].name, name) == 0) {
            return &applets[i];
        }
    }
    return 0;
}

/* relogio HH:MM:SS a partir do uptime */
static void sh_clock(char* out)
{
    uint64_t s = pit_uptime_sec();
    uint64_t hh = (s / 3600) % 100;
    uint64_t mm = (s / 60) % 60;
    uint64_t ss = s % 60;
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

static void sh_taskbar(void)
{
    if (!sh_theme98 || gui_active) {
        return; /* GUI desenha a propria taskbar */
    }
    char right[80];
    size_t n = 0;
    size_t cn = strlen(sh_cwd);
    if (cn > 50) cn = 50;
    for (size_t i = 0; i < cn; i++) right[n++] = sh_cwd[i];
    right[n++] = ' ';
    right[n++] = ' ';
    char clk[16];
    sh_clock(clk);
    for (size_t i = 0; clk[i] && n < sizeof(right) - 1; i++) {
        right[n++] = clk[i];
    }
    right[n] = '\0';
    vga_draw_taskbar(" Iniciar", right);
}

static void sh_desktop(void)
{
    if (gui_active) {
        return; /* compositor desenha o desktop */
    }
    if (!sh_theme98) {
        vga_set_color(0x07, 0x00);
        vga_taskbar_enable(0);
        vga_clear_screen();
        return;
    }
    /* desktop teal + janela de boas-vindas + taskbar */
    vga_set_color(0x0F, 0x03);
    vga_taskbar_enable(1);
    vga_clear_screen();
    int x0 = 18, w = 44, y0 = 6, h = 11;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            char s[2] = { ' ', '\0' };
            vga_write_at(y0 + r, x0 + c, s, 0x00, 0x07);
        }
    }
    vga_write_at(y0, x0, " built 0.5.0 - bem-vindo                   ", 0x0F, 0x01);
    vga_write_at(y0 + 2, x0 + 2, "Bem-vindo ao built!", 0x00, 0x07);
    vga_write_at(y0 + 3, x0 + 2, "Shell padrao: /bin/built", 0x00, 0x07);
    vga_write_at(y0 + 4, x0 + 2, "Digite 'help' para listar os", 0x00, 0x07);
    vga_write_at(y0 + 5, x0 + 2, "78 comandos do sistema.", 0x00, 0x07);
    vga_write_at(y0 + 7, x0 + 2, "PATH=/bin:/usr/bin", 0x00, 0x07);
    vga_write_at(y0 + 8, x0 + 2, "/bin /usr /etc /home /root", 0x00, 0x07);
    vga_move_cursor(y0 + h + 1, 0);
    sh_taskbar();
}

static inline void sh_outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t sh_inb(uint16_t port)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void sh_reboot(void)
{
    console_puts("Reiniciando...\n");
    sh_outb(0x64, 0xFE);
    for (;;) {
        cpu_hlt();
    }
}

/* Data via driver RTC (rtc.c) */
extern void rtc_get(uint8_t* day, uint8_t* mon, uint8_t* year2,
                    uint8_t* hh, uint8_t* mm, uint8_t* ss);

static void cmd_date(void)
{
    char b[8];
    uint8_t d, m, y, hh, mm, ss;
    rtc_get(&d, &m, &y, &hh, &mm, &ss);
    console_puts("RTC ");
    utoa64(d, b, sizeof(b));
    if (strlen(b) < 2) console_putc('0');
    console_puts(b);
    console_putc('/');
    utoa64(m, b, sizeof(b));
    if (strlen(b) < 2) console_putc('0');
    console_puts(b);
    console_puts("/20");
    utoa64(y, b, sizeof(b));
    if (strlen(b) < 2) console_putc('0');
    console_puts(b);
    console_putc(' ');
    utoa64(hh, b, sizeof(b));
    if (strlen(b) < 2) console_putc('0');
    console_puts(b);
    console_putc(':');
    utoa64(mm, b, sizeof(b));
    if (strlen(b) < 2) console_putc('0');
    console_puts(b);
    console_putc(':');
    utoa64(ss, b, sizeof(b));
    if (strlen(b) < 2) console_putc('0');
    console_puts(b);
    console_puts("  (uptime ");
    console_print_u64(pit_uptime_sec());
    console_puts("s)\n");
}

static void cmd_mem(void)
{
    extern uint32_t hw_ext_mem_kb(void);
    console_puts("kernel: base 0x100000, fim 0x");
    console_print_hex((uint64_t)&_end);
    console_puts("\nVGA texto: 0xB8000 (80x25)\n");
    console_puts("CMOS estendida: ");
    console_print_u64(hw_ext_mem_kb());
    console_puts(" KB acima de 1MB\n");
    console_puts("PIT: 100 Hz | overlay: 32x2KB | heap: (roadmap pmm)\n");
}

static void sh_mount_cb(const char* point, const char* fstype, void* ctx)
{
    (void)ctx;
    console_puts(fstype);
    console_puts(" em ");
    console_puts(point);
    console_putc('\n');
}

void sh_mount_list(void)
{
    vfs_list_mounts(sh_mount_cb, 0);
}

void sh_df(void)
{
    uint32_t nf = 0, nb = 0;
    extern void fs_stats(uint32_t* nfiles, uint32_t* nbytes);
    extern uint64_t sys_mem_kb;
    fs_stats(&nf, &nb);
    console_puts("overlay: ");
    console_print_u64(nf);
    console_puts("/32 arqs, ");
    console_print_u64(nb);
    console_puts("/65536 B\n");
    console_puts("mem: ");
    console_print_u64(sys_mem_kb ? sys_mem_kb : 0);
    console_puts(" KB (e820)\n");
    console_puts("montagens:\n");
    sh_mount_list();
}

static void cmd_help(void)
{
    /* help com categorias coloridas (man <cmd> detalha). */
    console_puts("\x1B[92mmapple 0.13.0\x1B[0m - shell built, ");
    console_print_u64((uint64_t)APPLET_COUNT);
    console_puts(" comandos. \x1B[90mman <cmd>\x1B[0m detalha.\n");
    console_puts("\x1B[96m sistema:\x1B[0m ver uname uptime mem cpuinfo fetch ");
    console_puts("ps env date drivers\n");
    console_puts("\x1B[96m arquivos:\x1B[0m ls cat cd pwd touch rm mkdir cp mv ");
    console_puts("head wc grep hexdump mount umount df install\n");
    console_puts("\x1B[96m texto/dev:\x1B[0m echo edit calc cc sleep color whoami ");
    console_puts("which history man\n");
    console_puts("\x1B[96m midia/rede:\x1B[0m beep play img ping netcfg\n");
    console_puts("\x1B[96m usb:\x1B[0m usb usbmount\n");
    console_puts("\x1B[96m audio:\x1B[0m vol mute audioinfo\n");
    console_puts("\x1B[96m video:\x1B[0m brilho brightness\n");
    console_puts("\x1B[96m apps graficos:\x1B[0m notepad explorer taskmgr disks ");
    console_puts("isn_terminal winfo\n");
    console_puts("  3ddd browser config calc sobre cal relogio snake musica matrix\n");
    console_puts("  foon pong breakout tetris torus terreno tunel\n");
    console_puts("\x1B[96m sessao:\x1B[0m clear busybox sh theme exit reboot halt\n");
    console_puts("\x1B[96m teclas:\x1B[0m Alt+Tab troca janela | Alt+T volta ao ");
    console_puts("terminal | Esc fecha app\n");
    console_puts("\x1B[96m ex:\x1B[0m echo ola > /home/guest/f.txt | fetch | matrix\n");
}

static void cmd_man(const char* args)
{
    if (!*args) {
        console_puts("uso: man <comando>\n");
        return;
    }
    if (strcmp(args, "ls") == 0) {
        console_puts("ls [dir] - lista diretorio (padrao: atual)\n");
    } else if (strcmp(args, "cat") == 0) {
        console_puts("cat <arq> - imprime arquivo\n");
    } else if (strcmp(args, "edit") == 0) {
        console_puts("edit <arq> - editor tela-cheia: ^O salva ^X sai Esc cancela\n");
    } else if (strcmp(args, "cc") == 0) {
        console_puts("cc <prog.c> - compila subconjunto C: int, +-*/%, print(), return\n");
    } else if (strcmp(args, "cd") == 0) {
        console_puts("cd <dir> - troca diretorio (. .. / caminhos relativos ok)\n");
    } else if (strcmp(args, "busybox") == 0) {
        console_puts("busybox [applet] - multi-call: sem args lista applets\n");
    } else if (strcmp(args, "fetch") == 0) {
        console_puts("fetch - resumo do sistema com a logo MG (estilo fastfetch)\n");
    } else if (strcmp(args, "matrix") == 0) {
        console_puts("matrix - chuva de glifos (estilo cmatrix): q/Esc sai\n");
    } else if (strcmp(args, "cpuinfo") == 0) {
        console_puts("cpuinfo - vendor, modelo e flags da CPU (via CPUID)\n");
    } else if (strcmp(args, "drivers") == 0) {
        console_puts("drivers - lista drivers: [OK] detectado [ - ] ausente\n");
    } else if (strcmp(args, "reboot") == 0) {
        console_puts("reboot - reinicia via 8042 (fallback: triple fault)\n");
    } else if (strcmp(args, "calc") == 0) {
        console_puts("calc - calculadora (usa o compilador cc): 2+3*4 =\n");
    } else if (strcmp(args, "snake") == 0) {
        console_puts("snake - setas/WASD movem, Enter reinicia, q/Esc sai\n");
    } else if (strcmp(args, "foon") == 0) {
        console_puts("foon - raycaster estilo Doom: WASD/setas, Esp atira, q sai\n");
    } else if (strcmp(args, "pong") == 0) {
        console_puts("pong - W/S ou setas, 5 pontos vencem, q/Esc sai\n");
    } else if (strcmp(args, "breakout") == 0) {
        console_puts("breakout - A/D ou setas, 3 vidas, q/Esc sai\n");
    } else if (strcmp(args, "tetris") == 0) {
        console_puts("tetris - setas movem, cima gira, Esp cai, q/Esc sai\n");
    } else if (strcmp(args, "torus") == 0) {
        console_puts("torus - demo 3D (96 quads, testa fill_tri/painter)\n");
    } else if (strcmp(args, "terreno") == 0) {
        console_puts("terreno - demo 3D (heightfield wireframe, testa linhas)\n");
    } else if (strcmp(args, "tunel") == 0) {
        console_puts("tunel - demo 3D (aneis em perspectiva, testa fill-rate)\n");
    } else if (strcmp(args, "rm") == 0) {
        console_puts("rm [-r] [-f] <arq...> - apaga; -r recursivo, -f sem erro\n");
    } else if (strcmp(args, "mount") == 0) {
        console_puts("mount [-t dev|proc|sys] <ponto> - monta dispositivo virtual\n");
    } else if (strcmp(args, "umount") == 0) {
        console_puts("umount <ponto> - desmonta\n");
    } else if (strcmp(args, "df") == 0) {
        console_puts("df - uso do overlay + montagens + memoria\n");
    } else if (strcmp(args, "install") == 0) {
        console_puts("install - instala o mapple num HD (APAGA o disco!)\n");
    } else if (strcmp(args, "usb") == 0) {
        console_puts("usb - lista controladoras/portas USB (uhci/ohci/ehci/xhci)\n");
    } else if (strcmp(args, "usbmount") == 0) {
        console_puts("usbmount - cria /mnt/usb se houver pendrive (stub)\n");
    } else if (strcmp(args, "vol") == 0) {
        console_puts("vol [0-100] - mostra/ajusta o volume do mixer\n");
    } else if (strcmp(args, "mute") == 0) {
        console_puts("mute [on|off] - mostra/alterna o mudo\n");
    } else if (strcmp(args, "audioinfo") == 0) {
        console_puts("audioinfo - mixer + AC97/HDA detectados\n");
    } else if (strcmp(args, "brilho") == 0) {
        console_puts("brilho [10-100] - mostra/ajusta o brilho (alias: brightness)\n");
    } else if (strcmp(args, "notepad") == 0) {
        console_puts("notepad [arq] - editor grafico: ^X salva+sai Esc sai\n");
    } else if (strcmp(args, "help") == 0) {
        console_puts("help - esta lista; man <cmd> detalha cada comando\n");
    } else {
        console_puts("sem pagina para '");
        console_puts(args);
        console_puts("'. Tente 'help'.\n");
    }
}

/* buffer de arquivo (8KB) */
static uint8_t fbuf[8192];

static void cmd_head(const char* path)
{
    int n = fs_read(path, fbuf, sizeof(fbuf) - 1);
    if (n < 0) {
        console_puts("head: arquivo nao encontrado\n");
        return;
    }
    int lines = 0;
    for (int i = 0; i < n && lines < 10; i++) {
        console_putc((char)fbuf[i]);
        if (fbuf[i] == '\n') lines++;
    }
}

static void cmd_wc(const char* path)
{
    int n = fs_read(path, fbuf, sizeof(fbuf) - 1);
    if (n < 0) {
        console_puts("wc: arquivo nao encontrado\n");
        return;
    }
    uint64_t lines = 0, words = 0;
    int inword = 0;
    for (int i = 0; i < n; i++) {
        if (fbuf[i] == '\n') lines++;
        if (fbuf[i] == ' ' || fbuf[i] == '\t' || fbuf[i] == '\n') {
            inword = 0;
        } else if (!inword) {
            inword = 1;
            words++;
        }
    }
    console_print_u64(lines);
    console_putc(' ');
    console_print_u64(words);
    console_putc(' ');
    console_print_u64((uint64_t)n);
    console_putc('\n');
}

static void cmd_grep(const char* args)
{
    /* grep <padrao> <arquivo> */
    const char* sp = args;
    while (*sp && *sp != ' ') sp++;
    if (!*sp) {
        console_puts("uso: grep <padrao> <arquivo>\n");
        return;
    }
    char pat[64], file[128];
    size_t i = 0;
    while (args < sp && i < sizeof(pat) - 1) pat[i++] = *args++;
    pat[i] = '\0';
    args++;
    i = 0;
    while (*args && *args != ' ' && i < sizeof(file) - 1) file[i++] = *args++;
    file[i] = '\0';
    char ap[128];
    fs_resolve(sh_cwd, file, ap, sizeof(ap));
    int n = fs_read(ap, fbuf, sizeof(fbuf) - 1);
    if (n < 0) {
        console_puts("grep: arquivo nao encontrado\n");
        return;
    }
    fbuf[n] = '\0';
    size_t pn = strlen(pat);
    int pos = 0;
    while (pos < n) {
        int eol = pos;
        while (eol < n && fbuf[eol] != '\n') eol++;
        int found = 0;
        for (int k = pos; k + (int)pn <= eol && !found; k++) {
            size_t t = 0;
            while (t < pn && fbuf[k + t] == (uint8_t)pat[t]) t++;
            if (t == pn) found = 1;
        }
        if (found) {
            for (int k = pos; k <= eol && k < n; k++) {
                console_putc((char)fbuf[k]);
            }
            if (eol >= n || fbuf[eol] != '\n') console_putc('\n');
        }
        pos = eol + 1;
    }
}

static void cmd_hexdump(const char* path)
{
    int n = fs_read(path, fbuf, sizeof(fbuf));
    if (n < 0) {
        console_puts("hexdump: arquivo nao encontrado\n");
        return;
    }
    static const char* h = "0123456789abcdef";
    for (int off = 0; off < n; off += 16) {
        char b[16];
        utoa64((uint64_t)off, b, sizeof(b));
        console_puts(b);
        console_puts(": ");
        for (int k = 0; k < 16; k++) {
            if (off + k < n) {
                console_putc(h[(fbuf[off + k] >> 4) & 0xF]);
                console_putc(h[fbuf[off + k] & 0xF]);
            } else {
                console_puts("  ");
            }
            console_putc(k == 7 ? '-' : ' ');
        }
        console_puts(" |");
        for (int k = 0; k < 16 && off + k < n; k++) {
            char c = (char)fbuf[off + k];
            console_putc((c >= 0x20 && c <= 0x7E) ? c : '.');
        }
        console_puts("|\n");
    }
}

typedef struct {
    int dummy;
} ls_ctx_t;

static void ls_print_cb(const char* name, int is_dir, uint32_t size, void* ctx)
{
    (void)ctx;
    console_puts("  ");
    if (is_dir) {
        console_puts("\x1B[34m");
    }
    console_puts(name);
    if (is_dir) {
        console_puts("/\x1B[0m\n");
    } else {
        console_puts("  (");
        console_print_u64(size);
        console_puts(" bytes)\n");
    }
}

static void cmd_ls(const char* args)
{
    char ap[128];
    fs_resolve(sh_cwd, *args ? args : ".", ap, sizeof(ap));
    if (!fs_isdir(ap)) {
        /* arquivo exato? */
        int n = fs_read(ap, fbuf, 0);
        if (n == -1 && fs_exists(ap)) {
            console_puts("  ");
            console_puts(ap);
            console_putc('\n');
            return;
        }
        console_puts("ls: nao encontrado: ");
        console_puts(args);
        console_putc('\n');
        return;
    }
    ls_ctx_t ctx;
    fs_list(ap, ls_print_cb, &ctx);
    /* applets instalados (links -> busybox) em verde */
    if (strcmp(ap, "/bin") == 0 || strcmp(ap, "/usr/bin") == 0) {
        for (size_t i = 0; i < APPLET_COUNT; i++) {
            if (strcmp(applets[i].dir, ap) == 0) {
                console_puts("  \x1B[32m");
                console_puts(applets[i].name);
                console_puts("\x1B[0m  -> busybox\n");
            }
        }
    }
}

static void cmd_which(const char* args)
{
    if (!*args) {
        console_puts("uso: which <comando>\n");
        return;
    }
    /* nome base (ignora diretorio digitado) */
    const char* base = args;
    for (const char* p = args; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    const applet_t* a = applet_find(base);
    if (a) {
        console_puts(a->dir);
        console_putc('/');
        console_puts(a->name);
        console_putc('\n');
    } else {
        console_puts("which: nao encontrado: ");
        console_puts(args);
        console_putc('\n');
    }
}

/* resolve comando (com ou sem /caminho) -> applet; copia args ja separados */
static const applet_t* cmd_resolve(char* line, char** args)
{
    char* cmd = line;
    char* sp = line;
    while (*sp && *sp != ' ') sp++;
    if (*sp) {
        *sp = '\0';
        *args = sp + 1;
        while (**args == ' ') (*args)++;
    } else {
        *args = sp;
    }
    const char* base = cmd;
    for (const char* p = cmd; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    if (!*base) {
        return 0;
    }
    /* copia o nome base para o inicio (permite /bin/ls) */
    if (base != cmd) {
        size_t i = 0;
        while (base[i] && i < SHELL_MAX_LINE - 1) {
            cmd[i] = base[i];
            i++;
        }
        cmd[i] = '\0';
    }
    return applet_find(cmd);
}

static void sh_exec(char* line);

static void cmd_busybox(const char* args)
{
    if (!*args) {
            console_puts("BusyBox v0.13.0 (mapple) multi-call binary.\n");
        console_puts("Uso: busybox <applet> [args] | applets:\n  ");
        for (size_t i = 0; i < APPLET_COUNT; i++) {
            console_puts(applets[i].name);
            console_putc(i + 1 < APPLET_COUNT ? ' ' : '\n');
        }
        return;
    }
    if ((strncmp(args, "busybox", 7) == 0) && (args[7] == '\0' || args[7] == ' ')) {
        console_puts("busybox: nao aninhado\n");
        return;
    }
    char nb[SHELL_MAX_LINE];
    size_t i = 0;
    while (args[i] && i < SHELL_MAX_LINE - 1) {
        nb[i] = args[i];
        i++;
    }
    nb[i] = '\0';
    sh_exec(nb);
}

static void sh_exec(char* line)
{
    /* redirect `>` / `>>` */
    char* redir = 0;
    int append = 0;
    for (char* p = line; *p; p++) {
        if (*p == '>') {
            redir = p;
            if (*(p + 1) == '>') {
                append = 1;
            }
            break;
        }
    }
    char capbuf[4096];
    char* redpath = 0;
    if (redir) {
        *redir = '\0';
        redpath = redir + (append ? 2 : 1);
        while (*redpath == ' ') redpath++;
        char* e = redpath + strlen(redpath);
        while (e > redpath && *(e - 1) == ' ') {
            e--;
            *e = '\0';
        }
        /* aparar espacos do comando */
        char* c = line + strlen(line);
        while (c > line && *(c - 1) == ' ') {
            c--;
            *c = '\0';
        }
        console_capture_begin(capbuf, sizeof(capbuf));
    }

    char* args;
    const applet_t* a = cmd_resolve(line, &args);
    int rc_valid = 1;

    if (!a) {
        if (line[0]) {
            console_puts("built: comando desconhecido: ");
            console_puts(line);
            console_puts(" (tente 'help')\n");
        }
    } else if (strcmp(line, "help") == 0) {
        cmd_help();
    } else if (strcmp(line, "ver") == 0) {
        console_puts("built 0.13.0 em mapple 0.13.0 (x86_64, gui+wm)\n");
    } else if (strcmp(line, "cpuinfo") == 0) {
        extern const char* hw_cpu_vendor(void);
        extern const char* hw_cpu_brand(void);
        extern void hw_cpu_ids(uint32_t* fam, uint32_t* mod, uint32_t* step);
        extern int hw_cpu_has(uint32_t leaf, char reg, uint32_t bit);
        uint32_t f = 0, m = 0, s = 0;
        hw_cpu_ids(&f, &m, &s);
        console_puts("vendor: ");
        console_puts(hw_cpu_vendor());
        console_puts("\nbrand:  ");
        console_puts(hw_cpu_brand()[0] ? hw_cpu_brand() : "(sem folha estendida)");
        console_puts("\nfam/mod/step: ");
        console_print_u64(f);
        console_putc('/');
        console_print_u64(m);
        console_putc('/');
        console_print_u64(s);
        console_puts("\nflags:");
        if (hw_cpu_has(1, 'd', 0)) {
            console_puts(" fpu");
        }
        if (hw_cpu_has(1, 'd', 4)) {
            console_puts(" tsc");
        }
        if (hw_cpu_has(1, 'd', 8)) {
            console_puts(" cx8");
        }
        if (hw_cpu_has(1, 'd', 9)) {
            console_puts(" apic");
        }
        if (hw_cpu_has(1, 'd', 19)) {
            console_puts(" clfsh");
        }
        if (hw_cpu_has(1, 'd', 23)) {
            console_puts(" mmx");
        }
        if (hw_cpu_has(1, 'd', 24)) {
            console_puts(" fxsr");
        }
        if (hw_cpu_has(1, 'd', 25)) {
            console_puts(" sse");
        }
        if (hw_cpu_has(1, 'd', 26)) {
            console_puts(" sse2");
        }
        if (hw_cpu_has(1, 'd', 28)) {
            console_puts(" htt");
        }
        if (hw_cpu_has(1, 'c', 0)) {
            console_puts(" sse3");
        }
        if (hw_cpu_has(1, 'c', 9)) {
            console_puts(" ssse3");
        }
        if (hw_cpu_has(1, 'c', 19)) {
            console_puts(" sse4.1");
        }
        if (hw_cpu_has(1, 'c', 20)) {
            console_puts(" sse4.2");
        }
        if (hw_cpu_has(1, 'c', 25)) {
            console_puts(" aes");
        }
        if (hw_cpu_has(1, 'c', 28)) {
            console_puts(" avx");
        }
        if (hw_cpu_has(1, 'c', 30)) {
            console_puts(" rdrand");
        }
        if (hw_cpu_has(1, 'c', 31)) {
            console_puts(" hypervisor");
        }
        if (hw_cpu_has(7, 'b', 7)) {
            console_puts(" smep");
        }
        if (hw_cpu_has(7, 'b', 20)) {
            console_puts(" smap");
        }
        if (hw_cpu_has(0x80000001u, 'd', 27)) {
            console_puts(" rdtscp");
        }
        if (hw_cpu_has(0x80000001u, 'd', 29)) {
            console_puts(" lm");
        }
        console_putc('\n');
    } else if (strcmp(line, "reboot") == 0) {
        extern void hw_reboot(void);
        console_puts("reiniciando...\n");
        hw_reboot();
    } else if (strcmp(line, "matrix") == 0) {
        if (gui_active) {
            extern void gui_mtrx_open(void);
            gui_mtrx_open(); /* loop modal assume no prompt */
        } else {
            /* backend VGA texto: chuva direto em 0xB8000, q sai */
            extern int keyboard_has_key(void);
            extern int keyboard_try_get(char* out);
            extern void vga_clear_screen(void);
            volatile uint16_t* scr = (volatile uint16_t*)0xB8000;
            static uint32_t mseed = 123456789;
            int head[80], speed[80], len[80], x, y;
            uint64_t next;
            char dummy;
            for (x = 0; x < 80; x++) {
                mseed = mseed * 1103515245u + 12345u;
                head[x] = -((int)((mseed >> 16) % 50));
                mseed = mseed * 1103515245u + 12345u;
                speed[x] = 1 + (int)((mseed >> 16) % 3);
                mseed = mseed * 1103515245u + 12345u;
                len[x] = 6 + (int)((mseed >> 16) % 12);
            }
            for (y = 0; y < 25; y++) {
                for (x = 0; x < 80; x++) {
                    scr[y * 80 + x] = 0x0700 | ' ';
                }
            }
            next = pit_get_ticks() + 5;
            while (!keyboard_has_key()) {
                if (pit_get_ticks() < next) {
                    __asm__ volatile ("hlt");
                    continue;
                }
                next = pit_get_ticks() + 5;
                for (x = 0; x < 80; x++) {
                    int h = head[x];
                    /* apaga cauda */
                    int t = h - len[x];
                    if (t >= 0 && t < 25) {
                        scr[t * 80 + x] = 0x0700 | ' ';
                    }
                    /* rebaixa rastro anterior */
                    if (h >= 0 && h < 25) {
                        uint16_t cell = scr[h * 80 + x];
                        char ch = (char)(cell & 0xFF);
                        scr[h * 80 + x] = (uint16_t)(0x0200 | (uint8_t)ch);
                    }
                    mseed = mseed * 1103515245u + 12345u;
                    if (((mseed >> 16) % (uint32_t)speed[x]) == 0) {
                        head[x]++;
                        h = head[x];
                        if (h - len[x] > 25) {
                            mseed = mseed * 1103515245u + 12345u;
                            head[x] = -((int)((mseed >> 16) % 20));
                        } else if (h >= 0 && h < 25) {
                            char ch;
                            mseed = mseed * 1103515245u + 12345u;
                            {
                                static const char set[] =
                                    "abcABC0123$#@%&*+-<>\xB0\xB1\xB2";
                                ch = set[(mseed >> 16) %
                                         (sizeof(set) - 1)];
                            }
                            scr[h * 80 + x] = (uint16_t)(0x0A00 |
                                                         (uint8_t)ch);
                        }
                    }
                }
            }
            while (keyboard_try_get(&dummy)) {
            }
            vga_clear_screen();
        }
    } else if (strcmp(line, "fetch") == 0) {
        /* fastfetch do mapple: logo MG + infos reais do hardware. */
        extern const char* hw_cpu_brand(void);
        extern uint32_t hw_ext_mem_kb(void);
        extern int drv_count(void);
        extern int drv_ok_count(void);
        extern uint64_t sys_mem_kb;
        extern void gui_get_res(uint32_t* w, uint32_t* h);
        static const char* flogo[6] = {
            "#     #  #####",
            "##   ## ##   ##",
            "# # # # ##",
            "#  #  # ##  ###",
            "#     # ##   ##",
            "#     #  #####",
        };
        static const char* flab[9] = {
            "OS", "Kernel", "Uptime", "Shell", "Res",
            "CPU", "Mem", "Drivers", "Font",
        };
        char fval[9][80];
        int fi, fw;
        /* ultoa local */
        for (fi = 0; fi < 9; fi++) {
            fval[fi][0] = '\0';
        }
        {
            const char* s = "mapple 0.13.0 x86_64";
            int p = 0;
            while (*s && p < 79) fval[0][p++] = *s++;
            fval[0][p] = '\0';
        }
        {
            const char* s = gui_active ? "built 0.13.0 (gui+wm)" :
                                         "built 0.13.0 (texto)";
            int p = 0;
            while (*s && p < 79) fval[1][p++] = *s++;
            fval[1][p] = '\0';
        }
        {
            /* uptime em segundos */
            char rev[24];
            int rn = 0, p = 0;
            uint64_t v = pit_uptime_sec();
            if (v == 0) rev[rn++] = '0';
            while (v > 0 && rn < 23) {
                rev[rn++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (rn > 0 && p < 74) fval[2][p++] = rev[--rn];
            if (p < 78) {
                fval[2][p++] = 's';
                fval[2][p] = '\0';
            }
        }
        {
            const char* s = "built (/bin/built)";
            int p = 0;
            while (*s && p < 79) fval[3][p++] = *s++;
            fval[3][p] = '\0';
        }
        if (gui_active) {
            uint32_t rw = 0, rh = 0, vs[2];
            int vi, p = 0;
            gui_get_res(&rw, &rh);
            vs[0] = rw;
            vs[1] = rh;
            for (vi = 0; vi < 2; vi++) {
                char rev[12];
                int rn = 0;
                uint32_t v = vs[vi];
                if (v == 0) rev[rn++] = '0';
                while (v > 0 && rn < 11) {
                    rev[rn++] = (char)('0' + (v % 10));
                    v /= 10;
                }
                while (rn > 0 && p < 70) fval[4][p++] = rev[--rn];
                if (vi == 0 && p < 78) fval[4][p++] = 'x';
            }
            {
                const char* s = "x32";
                while (*s && p < 79) fval[4][p++] = *s++;
                fval[4][p] = '\0';
            }
        } else {
            const char* s = "80x25 texto";
            int p = 0;
            while (*s && p < 79) fval[4][p++] = *s++;
            fval[4][p] = '\0';
        }
        {
            const char* s = hw_cpu_brand();
            int p = 0;
            if (!s[0]) s = "(cpu)";
            while (*s && *s != ' ' && p < 79) {
                /* marca curta: ate 2a palavra p/ caber */
                fval[5][p++] = *s++;
            }
            /* mantem resto ate 40 chars */
            while (*s && p < 40) fval[5][p++] = *s++;
            fval[5][p] = '\0';
        }
        {
            uint64_t kb = sys_mem_kb ? sys_mem_kb :
                                        (uint64_t)hw_ext_mem_kb();
            uint64_t mb = kb / 1024;
            char rev[12];
            int rn = 0, p = 0;
            if (mb == 0) rev[rn++] = '0';
            while (mb > 0 && rn < 11) {
                rev[rn++] = (char)('0' + (mb % 10));
                mb /= 10;
            }
            while (rn > 0 && p < 70) fval[6][p++] = rev[--rn];
            {
                const char* s = " MB";
                while (*s && p < 79) fval[6][p++] = *s++;
                fval[6][p] = '\0';
            }
        }
        {
            int a = drv_ok_count(), b = drv_count(), p = 0, rn = 0;
            char rev[12];
            if (a == 0) rev[rn++] = '0';
            while (a > 0 && rn < 11) {
                rev[rn++] = (char)('0' + (a % 10));
                a /= 10;
            }
            while (rn > 0 && p < 70) fval[7][p++] = rev[--rn];
            if (p < 78) fval[7][p++] = '/';
            rn = 0;
            if (b == 0) rev[rn++] = '0';
            while (b > 0 && rn < 11) {
                rev[rn++] = (char)('0' + (b % 10));
                b /= 10;
            }
            while (rn > 0 && p < 78) fval[7][p++] = rev[--rn];
            fval[7][p] = '\0';
        }
        {
            const char* s = "mapple-8 (IBM VGA)";
            int p = 0;
            while (*s && p < 79) fval[8][p++] = *s++;
            fval[8][p] = '\0';
        }
        for (fi = 0; fi < 10; fi++) {
            if (fi < 6) {
                console_puts("\x1B[92m");
                console_puts(flogo[fi]);
                console_puts("\x1B[0m");
                fw = 0;
                {
                    const char* s = flogo[fi];
                    while (*s++) fw++;
                }
                while (fw < 20) {
                    console_putc(' ');
                    fw++;
                }
            } else {
                console_puts("                    ");
            }
            if (fi == 0) {
                console_puts("\x1B[96mroot\x1B[0m\x1B[92m@\x1B[96mmapple\x1B[0m\n");
            } else {
                console_puts("\x1B[96m");
                console_puts(flab[fi - 1]);
                console_puts(":\x1B[0m ");
                console_puts(fval[fi - 1]);
                console_putc('\n');
            }
        }

    } else if (strcmp(line, "notepad") == 0) {
        char ap[128];
        fs_resolve(sh_cwd, *args ? args : "/home/guest/notas.txt", ap, sizeof(ap));
        if (gui_active) {
            gui_note_open(ap); /* loop modal assume no prompt */
        } else {
            edit_file(ap); /* modo texto: editor tela-cheia */
        }
    } else if (strcmp(line, "explorer") == 0) {
        char ap[128];
        fs_resolve(sh_cwd, *args ? args : "/", ap, sizeof(ap));
        if (gui_active) {
            gui_expl_open(ap);
        } else {
            cmd_ls(ap);
        }
    } else if (strcmp(line, "taskmgr") == 0) {
        if (gui_active) {
            extern void gui_task_open(void);
            gui_task_open();
        } else {
            console_puts("taskmgr: requer modo grafico\n");
        }
    } else if (strcmp(line, "disks") == 0) {
        if (gui_active) {
            extern void gui_disk_open(void);
            gui_disk_open();
        } else {
            console_puts("disks: requer modo grafico\n");
        }
    } else if (strcmp(line, "winfo") == 0) {
        if (gui_active) {
            extern void gui_winfo(void);
            gui_winfo();
        } else {
            console_puts("winfo: requer modo grafico\n");
        }
    } else if (strcmp(line, "3ddd") == 0) {
        if (gui_active) {
            extern void gui_cube_open(void);
            gui_cube_open();
        } else {
            console_puts("3ddd: requer modo grafico\n");
        }
    } else if (strcmp(line, "browser") == 0) {
        if (gui_active) {
            extern void gui_web_open(const char* url);
            gui_web_open(*args ? args : "mapple://inicio");
        } else {
            console_puts("browser: requer modo grafico\n");
        }
    } else if (strcmp(line, "beep") == 0) {
        int ok;
        uint32_t f = sh_parse_u32(args, &ok);
        if (!ok || f == 0) {
            console_puts("uso: beep <hz> [ms]\n");
        } else {
            const char* sp = args;
            while (*sp && *sp != ' ') {
                sp++;
            }
            while (*sp == ' ') {
                sp++;
            }
            {
                int ok2;
                uint32_t ms = sh_parse_u32(sp, &ok2);
                audio_beep(f, ok2 ? ms : 200);
            }
        }
    } else if (strcmp(line, "play") == 0) {
        if (!*args) {
            console_puts("uso: play <freq:ms,...> (ex: play 440:150,0:50,660:200)\n");
        } else {
            audio_play(args);
        }
    } else if (strcmp(line, "usb") == 0) {
        usb_list();
    } else if (strcmp(line, "usbmount") == 0) {
        usb_mount();
    } else if (strcmp(line, "vol") == 0) {
        if (!*args) {
            console_puts("vol=");
            console_print_u64((uint64_t)audio_get_volume());
            console_puts(audio_is_muted() ? " (muted)\n" : "\n");
        } else {
            int ok, v = 0, neg = 0;
            const char* p = args;
            if (*p == '-') {
                neg = 1;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                p++;
            }
            ok = (p != args && *p == '\0');
            if (!ok) {
                console_puts("uso: vol [0-100]\n");
            } else {
                if (neg) {
                    v = 0;
                }
                if (v > 100) {
                    v = 100;
                }
                audio_set_volume(v);
                console_puts("vol=");
                console_print_u64((uint64_t)audio_get_volume());
                console_putc('\n');
            }
        }
    } else if (strcmp(line, "mute") == 0) {
        if (!*args) {
            audio_set_mute(!audio_is_muted());
            console_puts(audio_is_muted() ? "mute=on\n" : "mute=off\n");
        } else if (strcmp(args, "on") == 0 || strcmp(args, "1") == 0) {
            audio_set_mute(1);
            console_puts("mute=on\n");
        } else if (strcmp(args, "off") == 0 || strcmp(args, "0") == 0) {
            audio_set_mute(0);
            console_puts("mute=off\n");
        } else {
            console_puts("uso: mute [on|off]\n");
        }
    } else if (strcmp(line, "audioinfo") == 0) {
        audio_info();
    } else if (strcmp(line, "brilho") == 0 || strcmp(line, "brightness") == 0) {
        if (!*args) {
            console_puts("brilho=");
            console_print_u64((uint64_t)bright_get());
            if (!gui_active) {
                console_puts(" (texto: registrado, vale no grafico)\n");
            } else {
                console_putc('\n');
            }
        } else {
            int v = 0;
            const char* p = args;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                p++;
            }
            if (p == args || *p != '\0') {
                console_puts("uso: brilho [10-100]\n");
            } else {
                if (v < 10) {
                    v = 10;
                }
                if (v > 100) {
                    v = 100;
                }
                bright_set(v);
                if (gui_active) {
                    extern void gui_mark_dirty(void);
                    gui_mark_dirty();
                }
                console_puts("brilho=");
                console_print_u64((uint64_t)bright_get());
                console_putc('\n');
            }
        }
    } else if (strcmp(line, "ping") == 0) {
        ping_cmd(args);
    } else if (strcmp(line, "netcfg") == 0) {
        netcfg_cmd(args);
    } else if (strcmp(line, "drivers") == 0) {
        extern void drv_list(void);
        drv_list();
    } else if (strcmp(line, "calc") == 0) {
        if (gui_active) {
            extern void gui_calc_open(void);
            gui_calc_open();
        } else {
            console_puts("calc: requer modo grafico (use o applet calc)\n");
        }
    } else if (strcmp(line, "sobre") == 0) {
        if (gui_active) {
            extern void gui_about_open(void);
            gui_about_open();
        } else {
            console_puts("mapple 0.13.0 x86_64: kernel em C/C++/asm, shell built.\n");
        }
    } else if (strcmp(line, "cal") == 0) {
        if (gui_active) {
            extern void gui_cal_open(void);
            gui_cal_open();
        } else {
            console_puts("cal: requer modo grafico\n");
        }
    } else if (strcmp(line, "relogio") == 0) {
        if (gui_active) {
            extern void gui_clock_open(void);
            gui_clock_open();
        } else {
            cmd_date();
        }
    } else if (strcmp(line, "snake") == 0) {
        if (gui_active) {
            extern void gui_snake_open(void);
            gui_snake_open();
        } else {
            console_puts("snake: requer modo grafico\n");
        }
    } else if (strcmp(line, "musica") == 0) {
        if (gui_active) {
            extern void gui_mus_open(void);
            gui_mus_open();
        } else {
            console_puts("musica: requer modo grafico (tente 'play')\n");
        }
    } else if (strcmp(line, "foon") == 0) {
        if (gui_active) {
            extern void gui_foon_open(void);
            gui_foon_open();
        } else {
            console_puts("foon: requer modo grafico\n");
        }
    } else if (strcmp(line, "pong") == 0) {
        if (gui_active) {
            extern void gui_pong_open(void);
            gui_pong_open();
        } else {
            console_puts("pong: requer modo grafico\n");
        }
    } else if (strcmp(line, "breakout") == 0) {
        if (gui_active) {
            extern void gui_brk_open(void);
            gui_brk_open();
        } else {
            console_puts("breakout: requer modo grafico\n");
        }
    } else if (strcmp(line, "tetris") == 0) {
        if (gui_active) {
            extern void gui_tet_open(void);
            gui_tet_open();
        } else {
            console_puts("tetris: requer modo grafico\n");
        }
    } else if (strcmp(line, "torus") == 0) {
        if (gui_active) {
            extern void gui_torus_open(void);
            gui_torus_open();
        } else {
            console_puts("torus: requer modo grafico\n");
        }
    } else if (strcmp(line, "terreno") == 0) {
        if (gui_active) {
            extern void gui_terr_open(void);
            gui_terr_open();
        } else {
            console_puts("terreno: requer modo grafico\n");
        }
    } else if (strcmp(line, "tunel") == 0) {
        if (gui_active) {
            extern void gui_tun_open(void);
            gui_tun_open();
        } else {
            console_puts("tunel: requer modo grafico\n");
        }
    } else if (strcmp(line, "img") == 0) {
        if (!*args) {
            console_puts("uso: img <arquivo.bmp>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            if (gui_active) {
                extern void gui_img_open(const char* path);
                gui_img_open(ap);
            } else {
                console_puts("img: requer modo grafico\n");
            }
        }
    } else if (strcmp(line, "config") == 0) {
        if (gui_active) {
            extern void gui_set_open(void);
            gui_set_open();
        } else {
            console_puts("config: requer modo grafico\n");
        }
    } else if (strcmp(line, "fps") == 0) {
        extern int gui_get_fps(void);
        extern int gui_active;
        if (!gui_active) {
            console_puts("fps: requer modo grafico\n");
        } else {
            console_puts("fps: ");
            console_print_u64((uint64_t)gui_get_fps());
            console_putc('\n');
        }
    } else if (strcmp(line, "isn_terminal") == 0) {
        console_puts("isn_terminal 0.13.0 - terminal grafico do mapple\n");
        if (gui_active) {
            console_puts("modo grafico: esta janela. Feche pelo [X], reabra no Iniciar.\n");
        } else {
            console_puts("modo texto ativo (framebuffer indisponivel no boot).\n");
        }
    } else if (strcmp(line, "echo") == 0) {
        console_puts(args);
        console_putc('\n');
    } else if (strcmp(line, "clear") == 0) {
        if (gui_active) {
            term_clear();
        } else {
            vga_clear_screen();
        }
    } else if (strcmp(line, "ls") == 0) {
        cmd_ls(args);
    } else if (strcmp(line, "cat") == 0) {
        if (!*args) {
            console_puts("uso: cat <arquivo>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            int n = fs_read(ap, fbuf, sizeof(fbuf) - 1);
            if (n < 0) {
                console_puts("cat: arquivo nao encontrado: ");
                console_puts(args);
                console_putc('\n');
            } else {
                for (int i = 0; i < n; i++) {
                    console_putc((char)fbuf[i]);
                }
            }
        }
    } else if (strcmp(line, "uname") == 0) {
        console_puts("mapple mapple 0.13.0 x86_64\n");
    } else if (strcmp(line, "whoami") == 0) {
        console_puts("root\n");
    } else if (strcmp(line, "pwd") == 0) {
        console_puts(sh_cwd);
        console_putc('\n');
    } else if (strcmp(line, "cd") == 0) {
        char ap[128];
        fs_resolve(sh_cwd, *args ? args : "/", ap, sizeof(ap));
        if (!fs_isdir(ap)) {
            console_puts("cd: diretorio nao encontrado: ");
            console_puts(args);
            console_putc('\n');
        } else {
            strcpy(sh_cwd, ap);
        }
    } else if (strcmp(line, "touch") == 0) {
        if (!*args) {
            console_puts("uso: touch <arquivo>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            if (fs_touch(ap) != 0) {
                console_puts("touch: falhou\n");
            }
        }
    } else if (strcmp(line, "rm") == 0) {
        /* rm [-r] [-f] <arq/dir>... (multiplos caminhos ok) */
        int rec = 0, force = 0;
        const char* p = args;
        char tok[128];
        int any = 0;
        extern int fs_rm_recursive(const char* path, int* nf, int* nd);
        for (;;) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            {
                size_t k = 0;
                while (*p && *p != ' ' && k < sizeof(tok) - 1) {
                    tok[k++] = *p++;
                }
                tok[k] = '\0';
            }
            if (!any && tok[0] == '-' && tok[1] != '\0') {
                int ok = 1;
                for (size_t k = 1; tok[k]; k++) {
                    if (tok[k] == 'r' || tok[k] == 'R') {
                        rec = 1;
                    } else if (tok[k] == 'f') {
                        force = 1;
                    } else {
                        ok = 0;
                    }
                }
                if (ok) {
                    continue;
                }
            }
            any = 1;
            {
                char ap[128];
                int r;
                fs_resolve(sh_cwd, tok, ap, sizeof(ap));
                if (rec) {
                    int nf = 0, nd = 0;
                    r = fs_rm_recursive(ap, &nf, &nd);
                    if (r == -2) {
                        console_puts("rm: sistema de arquivos somente leitura: ");
                        console_puts(tok);
                        console_putc('\n');
                    } else if (r != 0 && !force) {
                        console_puts("rm: nao encontrado: ");
                        console_puts(tok);
                        console_putc('\n');
                    }
                } else if (fs_isdir(ap)) {
                    console_puts("rm: eh diretorio (use -r): ");
                    console_puts(tok);
                    console_putc('\n');
                    r = -1;
                } else {
                    r = fs_rm(ap);
                    if (r == -2) {
                        console_puts("rm: sistema de arquivos somente leitura: ");
                        console_puts(tok);
                        console_putc('\n');
                    } else if (r != 0 && !force) {
                        console_puts("rm: nao encontrado: ");
                        console_puts(tok);
                        console_putc('\n');
                    }
                }
            }
        }
        if (!any) {
            console_puts("uso: rm [-r] [-f] <arquivo>...\n");
        }
    } else if (strcmp(line, "mkdir") == 0) {
        if (!*args) {
            console_puts("uso: mkdir <dir>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            if (fs_mkdir(ap) != 0) {
                console_puts("mkdir: falhou (ja existe ou pai invalido)\n");
            }
        }
    } else if (strcmp(line, "mount") == 0) {
        if (!*args) {
            extern void sh_mount_list(void);
            sh_mount_list();
        } else {
            /* mount [-t tipo] ponto  (tipo padrao: nome da pasta) */
            char type[16] = { 0 }, point[128] = { 0 };
            const char* p = args;
            char tok[128];
            size_t k = 0;
            while (*p == ' ') {
                p++;
            }
            while (*p && *p != ' ' && k < sizeof(tok) - 1) {
                tok[k++] = *p++;
            }
            tok[k] = '\0';
            while (*p == ' ') {
                p++;
            }
            if (strcmp(tok, "-t") == 0 && *p) {
                k = 0;
                while (*p && *p != ' ' && k < sizeof(type) - 1) {
                    type[k++] = *p++;
                }
                type[k] = '\0';
                while (*p == ' ') {
                    p++;
                }
                k = 0;
                while (*p && *p != ' ' && k < sizeof(point) - 1) {
                    point[k++] = *p++;
                }
                point[k] = '\0';
            } else {
                size_t i = 0;
                while (tok[i] && i < sizeof(point) - 1) {
                    point[i] = tok[i];
                    i++;
                }
                point[i] = '\0';
                /* tipo = basename (dev/proc/sys) */
                {
                    const char* b = point;
                    for (const char* q = point; *q; q++) {
                        if (*q == '/') {
                            b = q + 1;
                        }
                    }
                    i = 0;
                    while (b[i] && i < sizeof(type) - 1) {
                        type[i] = b[i];
                        i++;
                    }
                    type[i] = '\0';
                }
            }
            if (!point[0] || !type[0]) {
                console_puts("uso: mount [-t dev|proc|sys] <ponto>\n");
            } else {
                char ap[128];
                fs_resolve(sh_cwd, point, ap, sizeof(ap));
                int r = vfs_mount(type, ap);
                if (r == -1) {
                    console_puts("mount: tipo invalido (dev|proc|sys)\n");
                } else if (r == -2) {
                    console_puts("mount: ponto invalido\n");
                } else if (r == -3) {
                    console_puts("mount: tabela cheia\n");
                } else {
                    console_puts(type);
                    console_puts(" em ");
                    console_puts(ap);
                    console_putc('\n');
                }
            }
        }
    } else if (strcmp(line, "umount") == 0) {
        if (!*args) {
            console_puts("uso: umount <ponto>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            if (vfs_umount(ap) != 0) {
                console_puts("umount: nao montado: ");
                console_puts(args);
                console_putc('\n');
            }
        }
    } else if (strcmp(line, "df") == 0) {
        extern void sh_df(void);
        sh_df();
    } else if (strcmp(line, "install") == 0) {
        extern void install_run(void);
        install_run();
    } else if (strcmp(line, "atatest") == 0) {
        extern int ata_write_lba28(int, uint32_t, const uint8_t*, uint32_t);
        extern int ata_read_lba28(int, uint32_t, uint8_t*, uint32_t);
        static uint8_t tw[512], tr[512];
        for (uint32_t t = 0; t < 3; t++) {
            uint32_t L = 300 + t;
            for (int i = 0; i < 512; i++) {
                tw[i] = (uint8_t)(0xC0 + t);
            }
            tw[0] = 0xDE;
            tw[1] = (uint8_t)L;
            console_puts("W");
            console_print_u64((uint64_t)L);
            console_putc(' ');
            ata_write_lba28(0, L, tw, 1);
        }
        console_puts("fim\n");
    } else if (strcmp(line, "cp") == 0 || strcmp(line, "mv") == 0) {
        char s1[128];
        const char* sp = args;
        while (*sp && *sp != ' ') sp++;
        if (!*sp) {
            console_puts("uso: ");
            console_puts(line);
            console_puts(" <orig> <dest>\n");
        } else {
            size_t k = 0;
            while (args < sp && k < sizeof(s1) - 1) s1[k++] = *args++;
            s1[k] = '\0';
            args++;
            char ap1[128], ap2[128];
            fs_resolve(sh_cwd, s1, ap1, sizeof(ap1));
            fs_resolve(sh_cwd, args, ap2, sizeof(ap2));
            int n = fs_read(ap1, fbuf, sizeof(fbuf));
            if (n < 0) {
                console_puts(line);
                console_puts(": origem nao encontrada\n");
            } else if (fs_write(ap2, fbuf, (size_t)n) != 0) {
                console_puts(line);
                console_puts(": falha na escrita\n");
            } else if (line[0] == 'm') {
                if (fs_rm(ap1) == -2) {
                    console_puts("mv: origem e somente leitura (copiado)\n");
                }
            }
        }
    } else if (strcmp(line, "head") == 0) {
        if (!*args) {
            console_puts("uso: head <arquivo>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            cmd_head(ap);
        }
    } else if (strcmp(line, "wc") == 0) {
        if (!*args) {
            console_puts("uso: wc <arquivo>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            cmd_wc(ap);
        }
    } else if (strcmp(line, "grep") == 0) {
        cmd_grep(args);
    } else if (strcmp(line, "hexdump") == 0) {
        if (!*args) {
            console_puts("uso: hexdump <arquivo>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            cmd_hexdump(ap);
        }
    } else if (strcmp(line, "which") == 0) {
        cmd_which(args);
    } else if (strcmp(line, "man") == 0) {
        cmd_man(args);
    } else if (strcmp(line, "env") == 0) {
        console_puts("USER=root\nHOME=/root\nSHELL=/bin/built\nPATH=/bin:/usr/bin\n");
    } else if (strcmp(line, "history") == 0) {
        for (uint32_t i = 0; i < sh_hcount && i < HIST_MAX; i++) {
            console_print_u64(i + 1);
            console_puts("  ");
            console_puts(sh_hist[i]);
            console_putc('\n');
        }
    } else if (strcmp(line, "date") == 0) {
        cmd_date();
    } else if (strcmp(line, "calc") == 0) {
        if (!*args) {
            console_puts("uso: calc <expressao>\n");
        } else {
            int64_t v;
            if (cc_eval_expr(args, &v) == 0) {
                console_print_i64(v);
                console_putc('\n');
            } else {
                console_puts("calc: expressao invalida\n");
            }
        }
    } else if (strcmp(line, "cc") == 0) {
        if (!*args) {
            console_puts("uso: cc <prog.c>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            cc_run_file(ap);
        }
    } else if (strcmp(line, "edit") == 0) {
        if (!*args) {
            console_puts("uso: edit <arquivo>\n");
        } else {
            char ap[128];
            fs_resolve(sh_cwd, args, ap, sizeof(ap));
            edit_file(ap);
        }
    } else if (strcmp(line, "busybox") == 0) {
        cmd_busybox(args);
    } else if (strcmp(line, "uptime") == 0) {
        console_puts("up ");
        console_print_u64(pit_uptime_sec());
        console_puts("s (ticks=");
        console_print_u64(pit_get_ticks());
        console_puts(")\n");
    } else if (strcmp(line, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(line, "color") == 0) {
        const char* sp = args;
        while (*sp && *sp != ' ') sp++;
        if (!*sp) {
            console_puts("uso: color <fg 0-15> <bg 0-15>\n");
        } else {
            char fg_s[8], bg_s[8];
            size_t i = 0;
            while (args < sp && i < 7) fg_s[i++] = *args++;
            fg_s[i] = '\0';
            args++;
            i = 0;
            while (*args && *args != ' ' && i < 7) bg_s[i++] = *args++;
            bg_s[i] = '\0';
            int fg = atoi(fg_s), bg = atoi(bg_s);
            if (fg < 0 || fg > 15 || bg < 0 || bg > 15) {
                console_puts("uso: color <fg 0-15> <bg 0-15>\n");
            } else {
                vga_set_color((uint8_t)fg, (uint8_t)bg);
                console_puts("Cor atualizada.\n");
            }
        }
    } else if (strcmp(line, "ps") == 0) {
        console_puts("PID  CMD\n0    [idle]\n1    /bin/built\nticks=");
        console_print_u64(pit_get_ticks());
        console_putc('\n');
    } else if (strcmp(line, "theme") == 0) {
        if (strcmp(args, "win98") == 0) {
            sh_theme98 = 1;
            if (gui_active) {
                gui_set_chrome(1);
            } else {
                sh_desktop();
            }
        } else if (strcmp(args, "unix") == 0) {
            sh_theme98 = 0;
            if (gui_active) {
                gui_set_chrome(0);
            } else {
                sh_desktop();
            }
        } else {
            console_puts("uso: theme [win98|unix] (atual: ");
            console_puts(sh_theme98 ? "win98" : "unix");
            console_puts(gui_active ? ", grafico" : ", texto");
            console_puts(")\n");
        }
    } else if (strcmp(line, "sleep") == 0) {
        uint64_t s = 0;
        int ok = 0;
        if (*args) {
            ok = 1;
            for (const char* p = args; *p && *p != ' '; p++) {
                if (*p < '0' || *p > '9') ok = 0;
                else s = s * 10 + (uint64_t)(*p - '0');
            }
        }
        if (!ok) {
            console_puts("uso: sleep <segundos>\n");
        } else {
            uint64_t fim = pit_get_ticks() + s * 100u;
            while (pit_get_ticks() < fim) {
                cpu_hlt();
            }
        }
    } else if (strcmp(line, "sh") == 0) {
        console_puts("built aninhado (exit volta).\n");
        /* loop aninhado simples */
        char line2[SHELL_MAX_LINE];
        while (!sh_exit_flag) {
            sh_readline(line2, sizeof(line2), 1);
            if (line2[0]) {
                sh_exec(line2);
            }
            if (sh_exit_flag) {
                sh_exit_flag = 0;
                break;
            }
        }
    } else if (strcmp(line, "exit") == 0) {
        sh_exit_flag = 1;
    } else if (strcmp(line, "reboot") == 0) {
        sh_reboot();
    } else if (strcmp(line, "halt") == 0) {
        console_puts("CPU parada. Feche a janela.\n");
        for (;;) {
            __asm__ volatile ("cli");
            __asm__ volatile ("hlt");
        }
    } else {
        rc_valid = 0;
        console_puts("built: applet nao implementado: ");
        console_puts(line);
        console_putc('\n');
    }
    (void)rc_valid;

    if (redir) {
        size_t n = console_capture_end();
        if (!*redpath) {
            console_puts("built: arquivo ausente apos '>'\n");
            return;
        }
        char ap[128];
        fs_resolve(sh_cwd, redpath, ap, sizeof(ap));
        if (append) {
            static uint8_t old[8192];
            int on = fs_read(ap, old, sizeof(old));
            if (on < 0) on = 0;
            if ((size_t)on + n > sizeof(old)) {
                console_puts("built: arquivo grande demais\n");
                return;
            }
            memcpy(old + on, capbuf, n);
            if (fs_write(ap, old, (size_t)on + n) != 0) {
                console_puts("built: falha na escrita\n");
            }
        } else {
            if (fs_write(ap, (uint8_t*)capbuf, n) != 0) {
                console_puts("built: falha na escrita\n");
            }
        }
    }
}

static char hist_draft[SHELL_MAX_LINE];
static int hist_pos = -1; /* -1 = linha atual */

/* prompt 2026: root verde + cwd azul (ANSI 32-bit, ambos os modos) */
static void prompt_print(int nested)
{
    if (nested) {
        console_puts("sh$ ");
        return;
    }
    console_puts("\x1B[32mroot@mapple\x1B[0m ");
    console_puts("\x1B[34m");
    console_puts(sh_cwd);
    console_puts("\x1B[0m$ ");
}

/* redesenha a linha atual (usa \r) */
static void rl_render(const char* buf, size_t len, size_t pos, size_t* shown, int nested)
{
    size_t i;
    console_putc('\r');
    prompt_print(nested);
    for (i = 0; i < len; i++) {
        console_putc(buf[i]);
    }
    for (; i < *shown; i++) {
        console_putc(' ');
    }
    if (len > *shown) {
        *shown = len;
    }
    console_putc('\r');
    prompt_print(nested);
    for (i = 0; i < pos; i++) {
        console_putc(buf[i]);
    }
}

static char comp_items[32][64];
static int comp_n = 0;

static void comp_cb(const char* name, int is_dir, uint32_t size, void* ctx)
{
    (void)size;
    (void)ctx;
    if (comp_n < 32) {
        size_t k = 0;
        (void)is_dir;
        while (name[k] && k < sizeof(comp_items[0]) - 1) {
            comp_items[comp_n][k] = name[k];
            k++;
        }
        comp_items[comp_n][k] = '\0';
        comp_n++;
    }
}

/* completa token com applets + arquivos do cwd */
static void rl_complete(char* buf, size_t* len, size_t* pos, size_t* shown, int nested)
{
    size_t ts = *len;
    char cand[32][64];
    int nc = 0, i;
    (void)nested;
    (void)shown;
    while (ts > 0 && buf[ts - 1] != ' ') {
        ts--;
    }
    if (*pos != *len) {
        return; /* so completa com o cursor no fim */
    }
    {
        const char* tok = buf + ts;
        size_t tn = *len - ts;
        for (i = 0; i < (int)APPLET_COUNT && nc < 32; i++) {
            if (strncmp(applets[i].name, tok, tn) == 0) {
                size_t k = 0;
                while (applets[i].name[k] && k < sizeof(cand[0]) - 1) {
                    cand[nc][k] = applets[i].name[k];
                    k++;
                }
                cand[nc][k] = '\0';
                nc++;
            }
        }
        comp_n = 0;
        fs_list(sh_cwd, comp_cb, 0);
        for (i = 0; i < comp_n && nc < 32; i++) {
            if (strncmp(comp_items[i], tok, tn) == 0) {
                size_t k = 0;
                while (comp_items[i][k] && k < sizeof(cand[0]) - 1) {
                    cand[nc][k] = comp_items[i][k];
                    k++;
                }
                cand[nc][k] = '\0';
                nc++;
            }
        }
        if (nc == 0) {
            return;
        }
        if (nc == 1) {
            size_t k = tn;
            while (cand[0][k] && *len + 1 < SHELL_MAX_LINE) {
                buf[(*len)++] = cand[0][k++];
            }
            buf[*len] = '\0';
            *pos = *len;
            return;
        }
        /* prefixo comum */
        {
            size_t p = tn;
            for (;;) {
                char ch = cand[0][p];
                int ok = 1, j;
                if (!ch) {
                    break;
                }
                for (j = 1; j < nc; j++) {
                    if (cand[j][p] != ch) {
                        ok = 0;
                        break;
                    }
                }
                if (!ok || *len + 1 >= SHELL_MAX_LINE) {
                    break;
                }
                buf[(*len)++] = ch;
                p++;
            }
            buf[*len] = '\0';
            *pos = *len;
        }
        /* lista opcoes */
        console_putc('\n');
        for (i = 0; i < nc; i++) {
            console_puts(cand[i]);
            console_puts("  ");
        }
        console_putc('\n');
    }
}

static void sh_readline(char* line, size_t cap, int nested)
{
    size_t len = 0, pos = 0, shown = 0;
    hist_pos = -1;
    line[0] = '\0';
    prompt_print(nested);
    for (;;) {
        char c = keyboard_getraw();
        if (c == '\n') {
            if (gui_switcher_open()) {
                gui_switcher_confirm();
                rl_render(line, len, pos, &shown, nested);
                continue;
            }
            console_putc('\n');
            line[len] = '\0';
            hist_pos = -1;
            return;
        } else if (c == '\b') {
            if (pos > 0) {
                memmove(line + pos - 1, line + pos, len - pos);
                pos--;
                len--;
                line[len] = '\0';
                rl_render(line, len, pos, &shown, nested);
            }
        } else if (c == KEY_DEL) {
            if (pos < len) {
                memmove(line + pos, line + pos + 1, len - pos - 1);
                len--;
                line[len] = '\0';
                rl_render(line, len, pos, &shown, nested);
            }
        } else if (c == KEY_LEFT) {
            if (pos > 0) {
                pos--;
                rl_render(line, len, pos, &shown, nested);
            }
        } else if (c == KEY_RIGHT) {
            if (pos < len) {
                pos++;
                rl_render(line, len, pos, &shown, nested);
            }
        } else if (c == KEY_HOME || c == 0x01) {
            pos = 0;
            rl_render(line, len, pos, &shown, nested);
        } else if (c == KEY_END || c == 0x05) {
            pos = len;
            rl_render(line, len, pos, &shown, nested);
        } else if (c == KEY_UP) {
            if (sh_hcount > 0) {
                if (hist_pos == -1) {
                    strcpy(hist_draft, line);
                    hist_draft[cap - 1] = '\0';
                    hist_pos = (int)sh_hcount - 1;
                } else if (hist_pos > 0) {
                    hist_pos--;
                }
                strcpy(line, sh_hist[hist_pos]);
                len = pos = strlen(line);
                rl_render(line, len, pos, &shown, nested);
            }
        } else if (c == KEY_DOWN) {
            if (hist_pos != -1) {
                hist_pos++;
                if (hist_pos >= (int)sh_hcount) {
                    hist_pos = -1;
                    strcpy(line, hist_draft);
                } else {
                    strcpy(line, sh_hist[hist_pos]);
                }
                len = pos = strlen(line);
                rl_render(line, len, pos, &shown, nested);
            }
        } else if (c == '\t') {
            if (gui_switcher_open()) {
                gui_switcher_next();
            } else {
                rl_complete(line, &len, &pos, &shown, nested);
            }
            rl_render(line, len, pos, &shown, nested);
        } else if (c == KEY_SWITCH) {
            gui_cycle_windows();
            rl_render(line, len, pos, &shown, nested);
        } else if (c == 0x03) {
            console_puts("^C\n");
            line[0] = '\0';
            hist_pos = -1;
            return;
        } else if (c == 0x0C) {
            if (gui_active) {
                term_clear();
            } else {
                vga_clear_screen();
            }
            shown = 0;
            rl_render(line, len, pos, &shown, nested);
        } else if (c == 0x1B) {
            if (gui_switcher_open()) {
                gui_switcher_cancel();
                rl_render(line, len, pos, &shown, nested);
                continue;
            }
            len = pos = 0;
            line[0] = '\0';
            hist_pos = -1;
            rl_render(line, len, pos, &shown, nested);
        } else if (c >= 0x20 && c <= 0x7E) {
            if (len + 1 < cap) {
                memmove(line + pos + 1, line + pos, len - pos);
                line[pos++] = c;
                len++;
                line[len] = '\0';
                hist_pos = -1;
                console_putc(c);
                /* eco simples se inseriu no fim e sem sujeira */
                if (pos == len) {
                    if (len > shown) {
                        shown = len;
                    }
                } else {
                    rl_render(line, len, pos, &shown, nested);
                }
            }
        }
    }
}

void shell_run(void)
{
    char line[SHELL_MAX_LINE];

    sh_desktop();
    console_puts("built 0.13.0 (/bin/built) - 'help' lista 78 comandos.\n");
    /* pausa curta p/ splash (15 ticks = 150ms x10) */
    {
        uint64_t fim = pit_get_ticks() + 150;
        while (pit_get_ticks() < fim) {
            cpu_hlt();
        }
    }

    sh_exit_flag = 0;
    while (!sh_exit_flag) {
        /* apps modais (abertos pelo menu/dock): rodam seus loops */
        while (gui_active && gui_note_modal()) {
            note_run_loop();
        }
        {
            extern int gui_web_modal(void);
            extern void browser_run_loop(void);
            while (gui_active && gui_web_modal()) {
                browser_run_loop();
            }
        }
        {
            extern int gui_calc_modal(void);
            extern void calc_run_loop(void);
            while (gui_active && gui_calc_modal()) {
                calc_run_loop();
            }
        }
        {
            extern int gui_snake_modal(void);
            extern void snake_run_loop(void);
            while (gui_active && gui_snake_modal()) {
                snake_run_loop();
            }
        }
        {
            extern int gui_mtrx_modal(void);
            extern void mtrx_run_loop(void);
            while (gui_active && gui_mtrx_modal()) {
                mtrx_run_loop();
            }
        }
        {
            extern int gui_game_modal(void);
            extern void game_run_loop(void);
            while (gui_active && gui_game_modal()) {
                game_run_loop();
            }
        }
        if (sh_exit_flag) {
            break;
        }
        sh_taskbar();
        sh_readline(line, sizeof(line), 0);
        if (!line[0]) {
            continue;
        }
        if (sh_hcount < HIST_MAX) {
            strcpy(sh_hist[sh_hcount++], line);
        } else {
            for (uint32_t i = 1; i < HIST_MAX; i++) {
                strcpy(sh_hist[i - 1], sh_hist[i]);
            }
            strcpy(sh_hist[HIST_MAX - 1], line);
        }
        sh_exec(line);
    }
    console_puts("logout\n");
}
