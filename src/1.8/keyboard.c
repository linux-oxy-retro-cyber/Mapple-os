/* keyboard.c - Driver de teclado PS/2 (scancode set 1, layout US).
 *
 * - Remapeia o PIC (0x20/0x28) e libera IRQ1.
 * - Traduz make codes para ASCII (com Shift e CapsLock).
 * - Enfileira caracteres num buffer circular; shell consome via keyboard_getchar().
 */
#include <stdint.h>

extern void vga_print_string(const char* str);
extern void gui_poll(void); /* compositor: desenha se sujo (modo grafico) */
extern int gui_active;
extern int gui_term_focused(void);
extern void gui_app_push(char c);
extern int gui_note_modal(void);
extern void note_run_loop(void);
extern int gui_web_modal(void);
extern void browser_run_loop(void);
extern int gui_calc_modal(void);
extern void calc_run_loop(void);
extern int gui_snake_modal(void);
extern void snake_run_loop(void);
extern int gui_mtrx_modal(void);
extern void mtrx_run_loop(void);
extern void cpu_hlt(void);
static void kbd_push(char c);

extern int gui_switcher_open(void);
extern void gui_focus_term(void);

/* Roteia a tecla: switcher aberto -> fila de apps (shell nao ve);
 * terminal focado (ou modo texto) -> buffer do shell;
 * senao -> fila do app em foco. */
static void kbd_route(char c)
{
    if (gui_active && (gui_switcher_open() || !gui_term_focused())) {
        gui_app_push(c);
    } else {
        kbd_push(c);
    }
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t data)
{
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

#define KBD_DATA_PORT   0x60

#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

#define KBD_IRQ         1
#define PIC_REMAP_OFFSET 0x20

/* --- Mapa US: scancode set 1 (make). 0 = tecla nao imprimivel --- */
static const char kbd_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/',
    0, '*', 0, ' ', 0,
};

static const char kbd_shifted[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?',
    0, '*', 0, ' ', 0,
};

/* Teclas especiais (fora do ASCII): setas, Del, Home, End. */
#define KEY_UP    ((char)0x81)
#define KEY_DOWN  ((char)0x82)
#define KEY_LEFT  ((char)0x83)
#define KEY_RIGHT ((char)0x84)
#define KEY_DEL   ((char)0x85)
#define KEY_HOME  ((char)0x86)
#define KEY_END   ((char)0x87)

/* --- Buffer circular de entrada --- */
#define KBD_BUF_SIZE 256
static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

static int kbd_shift = 0;
static int kbd_caps = 0;
static int kbd_ctrl = 0;
static int kbd_alt = 0;

#define KEY_SWITCH ((char)0x88) /* Alt+Tab: troca de janela */

static void kbd_push(char c)
{
    uint8_t next = (uint8_t)(kbd_head + 1);
    if (next == kbd_tail) {
        return; /* cheio: descarta */
    }
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

/* Tecla disponivel sem bloquear (p/ efeitos como matrix em texto). */
int keyboard_has_key(void)
{
    int r;
    __asm__ volatile ("cli");
    r = (kbd_head != kbd_tail);
    __asm__ volatile ("sti");
    return r;
}

int keyboard_try_get(char* out)
{
    int r = 0;
    __asm__ volatile ("cli");
    if (kbd_head != kbd_tail) {
        if (out) {
            *out = kbd_buf[kbd_tail];
        }
        kbd_tail = (uint8_t)(kbd_tail + 1);
        r = 1;
    }
    __asm__ volatile ("sti");
    return r;
}

/* Leitura raw bloqueante: inclui especiais e controles. */
char keyboard_getraw(void)
{
    for (;;) {
        /* Apps modais (menu/dock): cedem o controle a eles.
         * Sem isso o shell dormiria aqui e o app nunca receberia teclas. */
        while (gui_active && gui_note_modal()) {
            note_run_loop();
        }
        while (gui_active && gui_web_modal()) {
            browser_run_loop();
        }
        while (gui_active && gui_calc_modal()) {
            calc_run_loop();
        }
        while (gui_active && gui_snake_modal()) {
            snake_run_loop();
        }
        while (gui_active && gui_mtrx_modal()) {
            mtrx_run_loop();
        }
        {
            extern int gui_game_modal(void);
            extern void game_run_loop(void);
            while (gui_active && gui_game_modal()) {
                game_run_loop();
            }
        }
        __asm__ volatile ("cli");
        if (kbd_head != kbd_tail) {
            char c = kbd_buf[kbd_tail];
            kbd_tail = (uint8_t)(kbd_tail + 1);
            __asm__ volatile ("sti");
            return c;
        }
        __asm__ volatile ("sti");
        gui_poll(); /* compositor roda enquanto o shell dorme */
        cpu_hlt();
    }
}

/* Leitura cozida: so texto (usada pelo shell). */
char keyboard_getchar(void)
{
    for (;;) {
        char c = keyboard_getraw();
        if (c == '\n' || c == '\b' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
            return c;
        }
    }
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND, 0x20);
    }
    outb(PIC1_COMMAND, 0x20);
}

void pic_unmask_irq(uint8_t irq)
{
    if (irq < 8) {
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << irq)));
    } else {
        /* IRQ do escravo exige a cascata (IRQ2) livre no mestre */
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
        outb(PIC2_DATA, (uint8_t)(inb(PIC2_DATA) & ~(1u << (irq - 8))));
    }
}

void keyboard_handler(void)
{
    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == 0xE0) {
        /* Prefixo estendido: setas, Del, Home, End + numerico. */
        uint8_t ext = inb(KBD_DATA_PORT);
        if (!(ext & 0x80)) {
            switch (ext) {
                case 0x48: kbd_route(KEY_UP); break;
                case 0x50: kbd_route(KEY_DOWN); break;
                case 0x4B: kbd_route(KEY_LEFT); break;
                case 0x4D: kbd_route(KEY_RIGHT); break;
                case 0x53: kbd_route(KEY_DEL); break;
                case 0x47: kbd_route(KEY_HOME); break;
                case 0x4F: kbd_route(KEY_END); break;
                case 0x1C: kbd_route('\n'); break; /* Enter numerico */
                case 0x35: kbd_route('/'); break;  /* / numerico */
                case 0x4E: kbd_route('+'); break;  /* + numerico */
                default: break;
            }
        }
        pic_send_eoi(KBD_IRQ);
        return;
    }

    if (sc & 0x80) {
        /* Break code: atualiza modificadores. */
        uint8_t make = (uint8_t)(sc & 0x7F);
        if (make == 0x2A || make == 0x36) {
            kbd_shift = 0;
        } else if (make == 0x1D) {
            kbd_ctrl = 0;
        } else if (make == 0x38) {
            kbd_alt = 0;
        }
        pic_send_eoi(KBD_IRQ);
        return;
    }

    /* Make code. */
    if (sc == 0x2A || sc == 0x36) {
        kbd_shift = 1;
    } else if (sc == 0x1D) {
        kbd_ctrl = 1;
    } else if (sc == 0x38) {
        kbd_alt = 1;
    } else if (sc == 0x0F && kbd_alt) {
        kbd_route(KEY_SWITCH); /* Alt+Tab */
    } else if (sc == 0x14 && kbd_alt) {
        gui_focus_term(); /* Alt+T: volta ao terminal */
    } else if (sc == 0x3A) {
        kbd_caps = !kbd_caps;
    } else if (sc < 128) {
        char c = kbd_shift ? kbd_shifted[sc] : kbd_normal[sc];
        /* CapsLock inverte caixa das letras. */
        if (kbd_caps && c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        } else if (kbd_caps && c >= 'A' && c <= 'Z' && !kbd_shift) {
            /* ja maiuscula via caps sem shift: mantem */
        } else if (kbd_caps && kbd_shift && c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c) {
            if (kbd_ctrl && c >= 'a' && c <= 'z') {
                kbd_route((char)(c - 'a' + 1)); /* Ctrl+A..Z -> 0x01..0x1A */
            } else if (kbd_ctrl && c >= 'A' && c <= 'Z') {
                kbd_route((char)(c - 'A' + 1));
            } else {
                kbd_route(c);
            }
        }
    }

    pic_send_eoi(KBD_IRQ);
}

static void pic_remap(void)
{
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);

    outb(PIC1_DATA, PIC_REMAP_OFFSET);
    outb(PIC2_DATA, PIC_REMAP_OFFSET + 8);

    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);

    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void keyboard_init(void)
{
    pic_remap();
    pic_unmask_irq(KBD_IRQ);
    vga_print_string("Keyboard initialized.\n");
}
