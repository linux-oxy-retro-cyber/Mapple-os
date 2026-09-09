/*
 * vga.c - VGA text mode driver for the x86_64 minimalist kernel
 *
 * This driver provides basic functionality to interact with the VGA text buffer,
 * allowing the kernel to print characters and strings to the console.
 */

#include <stdint.h>

/* VGA text buffer memory address */
#define VGA_ADDRESS 0xB8000

/* VGA dimensions */
#define VGA_COLS 80
#define VGA_ROWS 25

/* Default color attributes (light grey on black) */
#define VGA_COLOR_DEFAULT 0x07

/* Global variables for cursor position and current color */
static uint16_t* vga_buffer = (uint16_t*)VGA_ADDRESS;
static uint8_t vga_current_color = VGA_COLOR_DEFAULT;
static int vga_cursor_row = 0;
static int vga_cursor_col = 0;

/* Quando ligado, a ultima linha e reservada (taskbar) e o scroll a preserva. */
static int vga_taskbar = 0;

void vga_taskbar_enable(int on)
{
    vga_taskbar = on ? 1 : 0;
}

static int vga_max_row(void)
{
    return vga_taskbar ? (VGA_ROWS - 1) : VGA_ROWS;
}

/* Forward declarations */
void vga_clear_screen(void);
static void vga_update_cursor(void);
static void vga_scroll(void);

/**
 * @brief Initializes the VGA text mode driver.
 *        Clears the screen and sets the cursor to the top-left corner.
 */
void vga_init(void)
{
    vga_clear_screen();
    vga_cursor_row = 0;
    vga_cursor_col = 0;
    vga_update_cursor();
}

/**
 * @brief Clears the entire VGA text mode screen.
 *        Fills the buffer with spaces using the current color attribute.
 */
void vga_clear_screen(void)
{
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_buffer[i] = (uint16_t)' ' | ((uint16_t)vga_current_color << 8);
    }
    vga_cursor_row = 0;
    vga_cursor_col = 0;
    vga_update_cursor();
}

/**
 * @brief Sets the foreground and background color for future text output.
 * @param foreground The foreground color (0-15).
 * @param background The background color (0-15).
 */
void vga_set_color(uint8_t foreground, uint8_t background)
{
    vga_current_color = (background << 4) | (foreground & 0x0F);
}

/* Cor a partir de componentes (usado pelo parser ANSI do console) */
void vga_set_attr(uint8_t foreground, uint8_t background)
{
    vga_set_color(foreground, background);
}

/**
 * @brief Puts a single character on the screen at the current cursor position.
 *        Handles special characters like newline (\n) and backspace (\b).
 * @param c The character to display.
 */
void vga_put_char(char c)
{
    switch (c) {
        case '\r':
            vga_cursor_col = 0;
            break;
        case '\n':
            vga_cursor_col = 0;
            vga_cursor_row++;
            break;
        case '\b':
            if (vga_cursor_col > 0) {
                vga_cursor_col--;
            } else if (vga_cursor_row > 0) {
                vga_cursor_row--;
                vga_cursor_col = VGA_COLS - 1;
            }
            vga_buffer[vga_cursor_row * VGA_COLS + vga_cursor_col] = (uint16_t)' ' | ((uint16_t)vga_current_color << 8);
            break;
        case '\t':
            // Simple tab: advance to next multiple of 8, or next line if no space
            vga_cursor_col = (vga_cursor_col + 8) & ~ (8 - 1);
            if (vga_cursor_col >= VGA_COLS) {
                vga_cursor_col = 0;
                vga_cursor_row++;
            }
            break;
        default:
            vga_buffer[vga_cursor_row * VGA_COLS + vga_cursor_col] = (uint16_t)c | ((uint16_t)vga_current_color << 8);
            vga_cursor_col++;
            break;
    }

    if (vga_cursor_col >= VGA_COLS) {
        vga_cursor_col = 0;
        vga_cursor_row++;
    }

    if (vga_cursor_row >= vga_max_row()) {
        vga_scroll();
    }

    vga_update_cursor();
}

/* Posiciona o cursor de hardware (com clamp). */
void vga_move_cursor(int row, int col)
{
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= vga_max_row()) row = vga_max_row() - 1;
    if (col >= VGA_COLS) col = VGA_COLS - 1;
    vga_cursor_row = row;
    vga_cursor_col = col;
    vga_update_cursor();
}

void vga_get_cursor(int* row, int* col)
{
    if (row) *row = vga_cursor_row;
    if (col) *col = vga_cursor_col;
}

/* Escreve string numa posicao arbitraria sem mover o cursor logico. */
void vga_write_at(int row, int col, const char* s, uint8_t fg, uint8_t bg)
{
    uint16_t attr = (uint16_t)(((bg & 0x0F) << 4) | (fg & 0x0F)) << 8;
    int sr = vga_cursor_row, sc = vga_cursor_col;
    while (*s && col < VGA_COLS) {
        if (row >= 0 && row < VGA_ROWS && col >= 0) {
            vga_buffer[row * VGA_COLS + col] = (uint16_t)(uint8_t)*s | attr;
        }
        s++;
        col++;
    }
    vga_cursor_row = sr;
    vga_cursor_col = sc;
    vga_update_cursor();
}

/* Preenche uma linha inteira com espacos coloridos (sem mover o cursor). */
void vga_fill_row(int row, uint8_t fg, uint8_t bg)
{
    uint16_t cell = (uint16_t)' ' | ((uint16_t)(((bg & 0x0F) << 4) | (fg & 0x0F)) << 8);
    if (row < 0 || row >= VGA_ROWS) {
        return;
    }
    for (int j = 0; j < VGA_COLS; j++) {
        vga_buffer[row * VGA_COLS + j] = cell;
    }
}

/* Taskbar estilo Win98 na ultima linha: esquerda fixa, relogio a direita. */
void vga_draw_taskbar(const char* left, const char* right)
{
    int row = VGA_ROWS - 1;
    vga_fill_row(row, 0x00, 0x07); /* preto sobre cinza claro */
    vga_write_at(row, 1, left, 0x00, 0x07);
    int n = 0;
    const char* p = right;
    while (*p++) n++;
    int col = VGA_COLS - n - 1;
    if (col < 0) col = 0;
    vga_write_at(row, col, right, 0x00, 0x07);
    /* botao "Iniciar" em destaque */
    vga_write_at(row, 1, " Iniciar ", 0x00, 0x07);
}

/**
 * @brief Prints a null-terminated string to the console.
 * @param str The string to print.
 */
void vga_print_string(const char* str)
{
    while (*str != '\0') {
        vga_put_char(*str);
        str++;
    }
}

/**
 * @brief Updates the hardware cursor position.
 *        This involves writing to VGA I/O ports.
 */
static inline void vga_outb(uint16_t port, uint8_t data)
{
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

static void vga_update_cursor(void)
{
    uint16_t pos = (uint16_t)(vga_cursor_row * VGA_COLS + vga_cursor_col);

    /* VGA CRT controller: escreve o indice em 0x3D4 e o dado em 0x3D5 */
    vga_outb(0x3D4, 0x0F);
    vga_outb(0x3D5, (uint8_t)(pos & 0xFF));
    vga_outb(0x3D4, 0x0E);
    vga_outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));

    /* Mantem o formato padrao do cursor; apenas a posicao e atualizada acima. */
}

/**
 * @brief Scrolls the screen up by one line.
 *        The top row is discarded, and a new blank row appears at the bottom.
 */
static void vga_scroll(void)
{
    int limit = vga_max_row(); /* ultima linha preservada se taskbar ligada */
    // Move all rows up by one
    for (int i = 1; i < limit; i++) {
        for (int j = 0; j < VGA_COLS; j++) {
            vga_buffer[(i - 1) * VGA_COLS + j] = vga_buffer[i * VGA_COLS + j];
        }
    }

    // Clear the last row of the scroll region
    for (int j = 0; j < VGA_COLS; j++) {
        vga_buffer[(limit - 1) * VGA_COLS + j] = (uint16_t)' ' | ((uint16_t)vga_current_color << 8);
    }

    vga_cursor_row = limit - 1;
    vga_cursor_col = 0;
    vga_update_cursor();
}
