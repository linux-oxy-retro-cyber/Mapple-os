/* edit.c - Editor de texto tela-cheia estilo nano ("built-nano").
 *
 * Setas/Home/End/Del, backspace, Enter; Ctrl+O salva, Ctrl+X salva+sai,
 * Esc sai sem salvar. Salva no FS (sombreando a ROM se preciso).
 */
#include <stdint.h>
#include <stddef.h>

extern void* memmove(void* dst, const void* src, size_t n);
extern size_t strlen(const char* s);

extern char keyboard_getraw(void);
extern void ts_clear(void);
extern void ts_move(int row, int col);
extern void ts_write_at(int row, int col, const char* s, uint8_t fg, uint8_t bg);
extern void vga_taskbar_enable(int on);
extern void ts_taskbar(int on);
extern void gui_editor_enter(void);
extern void gui_editor_exit(void);
extern int shell_win98(void);

extern int fs_read(const char* path, uint8_t* buf, size_t cap);
extern int fs_write(const char* path, const uint8_t* data, size_t size);

/* Codigos especiais do teclado (ver keyboard.c). */
#define KEY_UP    ((char)0x81)
#define KEY_DOWN  ((char)0x82)
#define KEY_LEFT  ((char)0x83)
#define KEY_RIGHT ((char)0x84)
#define KEY_DEL   ((char)0x85)
#define KEY_HOME  ((char)0x86)
#define KEY_END   ((char)0x87)

#define EDIT_MAX  8192
#define EDIT_ROWS 24  /* ultima linha = barra de status */

static uint8_t edit_buf[EDIT_MAX];
static uint32_t edit_len = 0;

static uint32_t edit_line_start(uint32_t idx)
{
    while (idx > 0 && edit_buf[idx - 1] != '\n') {
        idx--;
    }
    return idx;
}

static uint32_t edit_line_end(uint32_t idx)
{
    while (idx < edit_len && edit_buf[idx] != '\n') {
        idx++;
    }
    return idx;
}

static uint32_t edit_view = 0; /* scroll horizontal */

static void edit_render(const char* path, uint32_t cursor, uint32_t top, const char* msg)
{
    uint32_t cls = edit_line_start(cursor);
    uint32_t ccol = cursor - cls;
    if (ccol < edit_view) {
        edit_view = ccol;
    } else if (ccol >= edit_view + 80) {
        edit_view = ccol - 80 + 1;
    }
    ts_clear();
    /* corpo: 24 linhas a partir de top, com scroll horizontal */
    uint32_t idx = top;
    for (int row = 0; row < EDIT_ROWS; row++) {
        uint32_t lend = edit_line_end(idx);
        uint32_t q = idx + edit_view;
        if (q > lend) {
            q = lend;
        }
        uint32_t col = 0;
        while (q < lend && col < 80) {
            char s[2] = { (char)edit_buf[q], '\0' };
            ts_write_at(row, (int)col, s, 0x07, 0x00);
            q++;
            col++;
        }
        idx = (lend < edit_len) ? lend + 1 : edit_len;
    }
    /* barra de status */
    char bar[81];
    size_t n = 0;
    const char* a = " built-nano: ";
    while (*a && n < 80) bar[n++] = *a++;
    size_t pn = strlen(path);
    if (pn > 40) pn = 40;
    for (size_t i = 0; i < pn && n < 80; i++) bar[n++] = path[i];
    const char* b = "  ^O salvar  ^X sair";
    if (msg && *msg) {
        b = msg;
    }
    while (*b && n < 80) bar[n++] = *b++;
    bar[n] = '\0';
    ts_write_at(24, 0, "                                                                                ", 0x00, 0x07);
    ts_write_at(24, 0, bar, 0x00, 0x07);

    /* cursor: linha/col relativos a top */
    uint32_t line = 0;
    for (uint32_t i = 0; i < cursor; i++) {
        if (edit_buf[i] == '\n') line++;
    }
    uint32_t topline = 0;
    for (uint32_t i = 0; i < top; i++) {
        if (edit_buf[i] == '\n') topline++;
    }
    uint32_t ls = edit_line_start(cursor);
    ts_move((int)(line - topline), (int)(cursor - ls) - (int)edit_view);
}

static void edit_goto_line(uint32_t* cursor, int dir)
{
    uint32_t ls = edit_line_start(*cursor);
    uint32_t col = *cursor - ls;
    if (dir < 0) {
        if (ls == 0) return;
        uint32_t pls = edit_line_start(ls - 1);
        uint32_t pe = edit_line_end(pls);
        *cursor = pls + col;
        if (*cursor > pe) *cursor = pe;
    } else {
        uint32_t le = edit_line_end(*cursor);
        if (le >= edit_len) return;
        uint32_t nls = le + 1;
        uint32_t ne = edit_line_end(nls);
        *cursor = nls + col;
        if (*cursor > ne) *cursor = ne;
    }
}

void edit_file(const char* path)
{
    int n = fs_read(path, edit_buf, sizeof(edit_buf));
    edit_len = (n < 0) ? 0 : (uint32_t)n;
    edit_view = 0;

    uint32_t cursor = edit_len;
    uint32_t top = 0;
    const char* msg = 0;
    int saved_flash = 0;

    vga_taskbar_enable(0);
    ts_taskbar(0);
    gui_editor_enter();

    for (;;) {
        /* garante cursor visivel */
        uint32_t line = 0;
        for (uint32_t i = 0; i < cursor; i++) {
            if (edit_buf[i] == '\n') line++;
        }
        uint32_t topline = 0;
        for (uint32_t i = 0; i < top; i++) {
            if (edit_buf[i] == '\n') topline++;
        }
        while (line < topline) {
            /* sobe top uma linha */
            if (top == 0) break;
            top = (top > 0 && edit_buf[top - 1] == '\n') ? top - 1 : edit_line_start(top - 1);
            topline--;
        }
        while (line >= topline + EDIT_ROWS) {
            uint32_t le = edit_line_end(top);
            top = (le < edit_len) ? le + 1 : edit_len;
            topline++;
        }

        edit_render(path, cursor, top, saved_flash ? " [arquivo salvo]" : msg);
        if (saved_flash) {
            saved_flash = 0;
            msg = 0;
        }

        char c = keyboard_getraw();
        if (c == 0x1B) {
            break; /* Esc: sai sem salvar */
        } else if (c == 0x0F) {
            /* Ctrl+O: salva */
            if (fs_write(path, edit_buf, edit_len) == 0) {
                saved_flash = 1;
            } else {
                msg = " [erro ao salvar]";
            }
        } else if (c == 0x18) {
            /* Ctrl+X: salva e sai */
            fs_write(path, edit_buf, edit_len);
            break;
        } else if (c == '\n') {
            if (edit_len < EDIT_MAX - 1) {
                memmove(edit_buf + cursor + 1, edit_buf + cursor, edit_len - cursor);
                edit_buf[cursor++] = '\n';
                edit_len++;
            }
        } else if (c == '\b') {
            if (cursor > 0) {
                memmove(edit_buf + cursor - 1, edit_buf + cursor, edit_len - cursor);
                cursor--;
                edit_len--;
            }
        } else if (c == KEY_DEL) {
            if (cursor < edit_len) {
                memmove(edit_buf + cursor, edit_buf + cursor + 1, edit_len - cursor - 1);
                edit_len--;
            }
        } else if (c == KEY_LEFT) {
            if (cursor > 0) cursor--;
        } else if (c == KEY_RIGHT) {
            if (cursor < edit_len) cursor++;
        } else if (c == KEY_UP) {
            edit_goto_line(&cursor, -1);
        } else if (c == KEY_DOWN) {
            edit_goto_line(&cursor, 1);
        } else if (c == KEY_HOME) {
            cursor = edit_line_start(cursor);
        } else if (c == KEY_END) {
            cursor = edit_line_end(cursor);
        } else if (c >= 0x20 && c <= 0x7E) {
            if (edit_len < EDIT_MAX - 1) {
                memmove(edit_buf + cursor + 1, edit_buf + cursor, edit_len - cursor);
                edit_buf[cursor++] = (uint8_t)c;
                edit_len++;
            }
        }
    }

    vga_taskbar_enable(shell_win98());
    ts_taskbar(shell_win98());
    gui_editor_exit();
    ts_clear();
}
