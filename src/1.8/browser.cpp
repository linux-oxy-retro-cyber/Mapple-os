// browser.cpp - Navegador offline do mapple ("Navegador").
//
// Sem rede (sem TCP/IP): navega em paginas mapple:// embutidas e em
// arquivos .html/.txt do ramfs, com subset HTML (h1 p a b br hr title),
// links clicaveis, voltar/avancar e barra de URL (Ctrl+L).
//
// C++ freestanding: struct POD (sem construtor -> seguro sem CRT),
// sem new/exceptions/RTTI/virtual. Fronteira extern "C".
#include <stdint.h>
#include <stddef.h>

extern "C" {

// libc propria
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strcpy(char* dst, const char* src);
void* memset(void* dst, int c, size_t n);

// console / fs / gui (C)
void console_putc(char c);
void console_puts(const char* s);
int fs_read(const char* path, uint8_t* buf, size_t cap);
void btext(int x, int y, const char* s, uint32_t fg, uint32_t bg);
void bfill(int x, int y, int w, int h, uint32_t c);
int bmp_open(const uint8_t* data, size_t size, void* out);
char gui_app_getkey(void);
char gui_app_getkey_alive(int (*alive)(void));
void gui_mark_dirty(void);
uint64_t pit_get_ticks(void);
int gui_web_modal(void);

} // extern "C"

namespace mapple {

enum class ItemKind : uint8_t { Text = 0, H1 = 1, Link = 2, Rule = 3 };

struct Item {
    ItemKind kind;
    char text[96];
    char href[96];
};

struct Line {
    char text[96];
    int link; /* indice em links[] ou -1 */
};

struct Link {
    char text[64];
    char href[96];
    int line0, line1; /* linhas ocupadas (p/ clique) */
};

class Browser {
    bool opened_ = false;
    char url_[128] = { 0 };
    char path_[128] = { 0 };
    char title_[32] = { 0 };
    bool has_title_ = false;
    char status_[96] = { 0 };
    char url_buf_[128] = { 0 };
    bool url_edit_ = false;
    char hist_[8][128] = { { 0 } };
    int hist_n_ = 0;
    int hist_i_ = -1;
    char lines_[200][96] = { { 0 } };
    int linelink_[200] = { 0 };
    uint8_t linekind_[200] = { 0 };
    uint32_t linefg_[200] = { 0 };
    uint32_t linebg_[200] = { 0 };
    uint8_t linealign_[200] = { 0 }; /* 0 esq, 1 centro */
    int lineimgw_[200] = { 0 };
    int lineimgh_[200] = { 0 };
    int lineimgoff_[200] = { 0 };
    int nlines_ = 0;
    uint32_t cur_fg_ = 0x000000;
    uint32_t cur_bg_ = 0xFFFFFF;
    int style_inline_ = 0; /* estilo inline ativo (vence regra de tag) */
    /* CSS por tag (0 = padrao) */
    uint32_t css_h1_ = 0x000080, css_p_ = 0x000000, css_a_ = 0x0000CC;
    int css_h1_on_ = 0, css_p_on_ = 0, css_a_on_ = 0;
    int center_depth_ = 0;
    Link links_[32];
    int nlinks_ = 0;
    int view_top_ = 0;
public:
    void open_window(const char* url)
    {
        if (!opened_) {
            opened_ = true;
            hist_n_ = 0;
            hist_i_ = -1;
        }
        navigate(url, true);
    }

    bool is_open() const
    {
        return opened_;
    }

    void close()
    {
        opened_ = false;
    }

    const char* url() const
    {
        return url_;
    }

    void navigate(const char* url, bool push)
    {
        /* COPIA LOCAL OBRIGATORIA: url pode apontar p/ links_[].href,
         * que load()/parse() REESCREVE. Sem isso, historico corrompe. */
        char u[128];
        char path[128];
        copy_str(u, url, sizeof(u));
        resolve(u, path, sizeof(path));
        console_puts("nav:");
        console_puts(u);
        console_puts(push ? " P>" : " >");
        console_puts(path);
        console_puts("\n");
        if (!load(path)) {
            set_status("pagina nao encontrada");
            return;
        }
        copy_str(url_, u, sizeof(url_));
        copy_str(path_, path, sizeof(path_));
        if (push && hist_i_ >= 0 && strcmp(hist_[hist_i_], u) == 0) {
            push = false; /* evita duplicar a mesma pagina */
        }
        if (push) {
            if (hist_i_ + 1 < 8) {
                hist_i_++;
            } else {
                for (int i = 1; i < 8; i++) {
                    copy_str(hist_[i - 1], hist_[i], sizeof(hist_[0]));
                }
            }
            copy_str(hist_[hist_i_], u, sizeof(hist_[0]));
            hist_n_ = hist_i_ + 1;
        }
        view_top_ = 0;
        url_edit_ = false;
        set_status("pronto");
    }

    void back()
    {
        if (hist_i_ > 0) {
            hist_i_--;
            navigate(hist_[hist_i_], false);
        }
    }

    void forward()
    {
        if (hist_i_ + 1 < hist_n_) {
            hist_i_++;
            navigate(hist_[hist_i_], false);
        }
    }

    /* desenha na area cliente */
    void draw(int cx, int cy, int cw, int ch, int mx, int my)
    {
        int cols = cw / 8;
        int rows = (ch - 36) / 8;
        int y, r;
        if (cols < 8) {
            cols = 8;
        }
        if (rows < 2) {
            rows = 2;
        }
        if (cols > 95) {
            cols = 95;
        }
        /* barra: < > + url */
        bfill(cx, cy, cw, 16, 0xE8E8E8);
        btext(cx + 4, cy + 4, "<", 0x000000, 0xE8E8E8);
        btext(cx + 20, cy + 4, ">", 0x000000, 0xE8E8E8);
        {
            char ub[96];
            int n = 0;
            ub[n++] = ' ';
            const char* s = url_edit_ ? url_buf_ : url_;
            while (*s && n < 90) {
                ub[n++] = *s++;
            }
            ub[n] = '\0';
            btext(cx + 40, cy + 4, ub, 0x000000, 0xFFFFFF);
        }
        /* conteudo */
        if (view_top_ < 0) {
            view_top_ = 0;
        }
        if (view_top_ > nlines_ - rows) {
            view_top_ = nlines_ - rows;
        }
        if (view_top_ < 0) {
            view_top_ = 0;
        }
        for (r = 0; r < rows; r++) {
            int li = view_top_ + r;
            int yy = cy + 20 + r * 8;
            char row[96];
            int n = 0;
            if (li < 0 || li >= nlines_) {
                continue;
            }
            if (linekind_[li] == 4) {
                /* imagem: desenha reduzida p/ largura */
                int iw = lineimgw_[li], ih = lineimgh_[li];
                int off = lineimgoff_[li];
                int dw = iw, dh = ih;
                if (dw > cw - 8) {
                    dh = dh * (cw - 8) / dw;
                    dw = cw - 8;
                }
                if (dh > 96) {
                    dw = dw * 96 / dh;
                    dh = 96;
                }
                {
                    int y;
                    for (y = 0; y < dh && y + yy < cy + ch - 12; y++) {
                        int sy = y * ih / (dh ? dh : 1);
                        int x;
                        for (x = 0; x < dw; x++) {
                            int sx2 = x * iw / (dw ? dw : 1);
                            const uint8_t* p = imgpool_ + (size_t)off +
                                               ((size_t)sy * iw + sx2) * 3;
                            bfill(cx + 4 + x, yy + y, 1, 1,
                                  ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
                        }
                    }
                }
                continue;
            }
            if (linekind_[li] == 5) {
                continue; /* continuaçao da imagem */
            }
            while (lines_[li][n] && n < cols && n < 95) {
                row[n] = lines_[li][n];
                n++;
            }
            row[n] = '\0';
            {
                int islink = linelink_[li] >= 0;
                int hov = islink && mx >= cx && mx < cx + cw && my >= yy && my < yy + 8;
                uint32_t fg = linefg_[li];
                uint32_t bg = linebg_[li];
                int tx = cx + 4;
                if (linealign_[li] == 1) {
                    tx = cx + 4 + (cw - 8 - n * 8) / 2;
                    if (tx < cx + 4) {
                        tx = cx + 4;
                    }
                }
                if (hov) {
                    bfill(cx, yy, cw, 8, 0xD0D0FF);
                    bg = 0xD0D0FF;
                }
                btext(tx, yy, row, fg, bg);
                if (islink) {
                    bfill(tx, yy + 7, n * 8, 1, fg); /* sublinhado */
                }
                if (linekind_[li] == 3) {
                    bfill(cx + 4, yy + 3, cw - 8, 1, 0x808080); /* hr */
                }
            }
        }
        /* status */
        bfill(cx, cy + ch - 12, cw, 12, 0xE8E8E8);
        btext(cx + 4, cy + ch - 10, status_, 0x000000, 0xE8E8E8);
        y = 0;
        (void)y;
    }

    /* clique em coords locais do cliente; retorna 1 se consumiu */
    bool click(int lx, int ly, int cw, int ch)
    {
        int rows = (ch - 36) / 8;
        /* voltar */
        if (lx >= 4 && lx < 20 && ly >= 4 && ly < 20) {
            back();
            return true;
        }
        if (lx >= 20 && lx < 36 && ly >= 4 && ly < 20) {
            forward();
            return true;
        }
        /* barra de url: foca edicao */
        if (ly >= 4 && ly < 20 && lx >= 40) {
            url_edit_ = true;
            copy_str(url_buf_, url_, sizeof(url_buf_));
            return true;
        }
        {
            int r = (ly - 20) / 8;
            int li = view_top_ + r;
            if (li >= 0 && li < nlines_ && linelink_[li] >= 0) {
                int lk = linelink_[li];
                navigate(links_[lk].href, true);
                return true;
            }
        }
        (void)rows;
        (void)cw;
        return false;
    }

    /* loop modal: teclado (sai se a janela fechar) */
    void run_loop()
    {
        for (;;) {
            if (!::gui_web_modal()) {
                return;
            }
            char c = gui_app_getkey_alive(::gui_web_modal);
            if (!::gui_web_modal()) {
                return;
            }
            if (c == 0x1B) { /* Esc: sai da url ou fecha */
                if (url_edit_) {
                    url_edit_ = false;
                } else {
                    return; /* shell fecha a janela */
                }
            } else if (c == 0x0F) { /* Ctrl+O: nada (reservado) */
            } else if (c == '\n') {
                if (url_edit_) {
                    url_edit_ = false;
                    navigate(url_buf_, true);
                }
            } else if (c == '\b') {
                if (url_edit_) {
                    size_t n = strlen(url_buf_);
                    if (n > 0) {
                        url_buf_[n - 1] = '\0';
                    }
                }
            } else if (c >= 0x20 && (uint8_t)c <= 0x7E) {
                if (url_edit_) {
                    size_t n = strlen(url_buf_);
                    if (n + 1 < sizeof(url_buf_)) {
                        url_buf_[n] = c;
                        url_buf_[n + 1] = '\0';
                    }
                }
            } else if (c == (char)0x81) { /* Up: rola */
                if (view_top_ > 0) {
                    view_top_--;
                }
            } else if (c == (char)0x82) { /* Down */
                view_top_++;
            }
            gui_mark_dirty();
        }
    }

private:
    static void copy_str(char* dst, const char* src, size_t cap)
    {
        size_t i = 0;
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }

    void set_status(const char* s)
    {
        copy_str(status_, s, sizeof(status_));
    }

    void resolve(const char* url, char* path, size_t cap)
    {
        if (strncmp(url, "mapple://", 9) == 0) {
            const char* rest = url + 9;
            copy_str(path, "/", cap);
            size_t n = strlen(path);
            size_t i = 0;
            bool has_dot = false;
            while (rest[i] && n + 1 < cap) {
                if (rest[i] == '.') {
                    has_dot = true;
                }
                path[n++] = rest[i++];
            }
            path[n] = '\0';
            if (!has_dot) {
                /* mapple://inicio -> /index.html; demais -> /x.html */
                if (strcmp(rest, "inicio") == 0) {
                    copy_str(path, "/index.html", cap);
                } else {
                    size_t m = strlen(path);
                    const char* ext = ".html";
                    size_t k = 0;
                    while (ext[k] && m + 1 < cap) {
                        path[m++] = ext[k++];
                    }
                    path[m] = '\0';
                }
            }
        } else if (url[0] == '/') {
            copy_str(path, url, cap);
        } else {
            copy_str(path, "/", cap);
            size_t n = strlen(path);
            size_t i = 0;
            while (url[i] && n + 1 < cap) {
                path[n++] = url[i++];
            }
            path[n] = '\0';
        }
    }

    bool load(const char* path)
    {
        static uint8_t buf[8192];
        int n = fs_read(path, buf, sizeof(buf) - 1);
        if (n < 0) {
            return false;
        }
        buf[n] = '\0';
        parse((const char*)buf);
        return true;
    }

    void emit_text(const char* s, int link, int kind)
    {
        /* quebra em palavras -> linhas de 95 */
        char word[96];
        int wi = 0;
        size_t i = 0;
        for (;;) {
            char c = s[i];
            bool end = (c == '\0' || c == ' ' || c == '\t' || c == '\n');
            if (!end && wi < 90) {
                word[wi++] = c;
                i++;
                continue;
            }
            if (wi > 0) {
                emit_word(word, wi, link, kind);
                wi = 0;
            }
            if (c == '\0') {
                break;
            }
            if (c == '\n') {
                new_line(link, kind);
            }
            i++;
        }
    }

    void emit_word(const char* w, int wn, int link, int kind)
    {
        if (nlines_ <= 0) {
            new_line(link, kind);
        }
        size_t cur = strlen(lines_[nlines_ - 1]);
        if (cur > 0 && cur + 1 + (size_t)wn >= 95) {
            new_line(link, kind);
            cur = 0;
        }
        if (cur > 0) {
            lines_[nlines_ - 1][cur++] = ' ';
        }
        for (int k = 0; k < wn && cur < 94; k++) {
            lines_[nlines_ - 1][cur++] = w[k];
        }
        lines_[nlines_ - 1][cur] = '\0';
        linelink_[nlines_ - 1] = link;
        linekind_[nlines_ - 1] = (uint8_t)kind;
        paint_line(nlines_ - 1, link, kind);
    }

    /* pool de imagens da pagina (BMP decodificados, BGR) */
    uint8_t imgpool_[128 * 1024];
    uint32_t imgpool_n_;

    void img_tag(const char* src)
    {
        char path[128];
        static uint8_t fbuf[131072];
        int n;
        /* declaracao BMP local (evita header) */
        struct BH {
            const uint8_t* base;
            int w, h, stride, flip;
        };
        BH b;
        if (src[0] == '/') {
            copy_str(path, src, sizeof(path));
        } else {
            copy_str(path, "/", sizeof(path));
            size_t nn = strlen(path);
            size_t k = 0;
            while (src[k] && nn + 1 < sizeof(path)) {
                path[nn++] = src[k++];
            }
            path[nn] = '\0';
        }
        n = fs_read(path, fbuf, sizeof(fbuf));
        if (n <= 0) {
            return;
        }
        if (bmp_open(fbuf, (size_t)n, &b) != 0) {
            return;
        }
        if (b.w <= 0 || b.h <= 0 || b.w > 400 || b.h > 300) {
            return;
        }
        {
            size_t need = (size_t)b.w * (size_t)b.h * 3;
            size_t rows, r;
            if (imgpool_n_ + need > sizeof(imgpool_)) {
                return;
            }
            for (r = 0; r < (size_t)b.h; r++) {
                int x;
                for (x = 0; x < b.w; x++) {
                    const uint8_t* p;
                    int yy = (int)r, xx = x;
                    if (b.flip) {
                        p = b.base + (size_t)(b.h - 1 - yy) * (size_t)b.stride + (size_t)xx * 3;
                    } else {
                        p = b.base + (size_t)yy * (size_t)b.stride + (size_t)xx * 3;
                    }
                    imgpool_[imgpool_n_ + (r * b.w + x) * 3 + 0] = p[2];
                    imgpool_[imgpool_n_ + (r * b.w + x) * 3 + 1] = p[1];
                    imgpool_[imgpool_n_ + (r * b.w + x) * 3 + 2] = p[0];
                }
            }
            new_line(-1, 4);
            lineimgoff_[nlines_ - 1] = (int)imgpool_n_;
            lineimgw_[nlines_ - 1] = b.w;
            lineimgh_[nlines_ - 1] = b.h;
            imgpool_n_ += (uint32_t)need;
            /* reserva linhas p/ altura (12px cada) */
            rows = ((size_t)b.h + 11) / 12;
            for (r = 1; r < (int)rows && nlines_ < 200; r++) {
                new_line(-1, 5);
            }
        }
    }

    /* --- mini-JS: var/print/alert/if/while, int64 --- */
    struct JVar {
        char name[24];
        int64_t value;
        int used;
    };
    JVar jvars_[32];

    const char* js_;
    bool js_err_;

    void js_skip()
    {
        for (;;) {
            char c = *js_;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';') {
                js_++;
            } else if (c == '/' && js_[1] == '/') {
                while (*js_ && *js_ != '\n') {
                    js_++;
                }
            } else {
                break;
            }
        }
    }

    bool js_accept(char c)
    {
        js_skip();
        if (*js_ == c) {
            js_++;
            return true;
        }
        return false;
    }

    bool js_kw(const char* kw)
    {
        js_skip();
        size_t n = strlen(kw);
        if (strncmp(js_, kw, n) != 0) {
            return false;
        }
        char a = js_[n];
        if ((a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z') || (a >= '0' && a <= '9') || a == '_') {
            return false;
        }
        js_ += n;
        return true;
    }

    JVar* js_var(const char* name, bool create)
    {
        for (int i = 0; i < 32; i++) {
            if (jvars_[i].used && strcmp(jvars_[i].name, name) == 0) {
                return &jvars_[i];
            }
        }
        if (!create) {
            return nullptr;
        }
        for (int i = 0; i < 32; i++) {
            if (!jvars_[i].used) {
                jvars_[i].used = 1;
                copy_str(jvars_[i].name, name, sizeof(jvars_[i].name));
                jvars_[i].value = 0;
                return &jvars_[i];
            }
        }
        return nullptr;
    }

    int64_t js_factor()
    {
        js_skip();
        char c = *js_;
        if (c == '(') {
            js_++;
            int64_t v = js_expr();
            if (!js_accept(')')) {
                js_err_ = true;
            }
            return v;
        }
        if (c == '-') {
            js_++;
            return -js_factor();
        }
        if (c >= '0' && c <= '9') {
            int64_t v = 0;
            while (*js_ >= '0' && *js_ <= '9') {
                v = v * 10 + (*js_ - '0');
                js_++;
            }
            return v;
        }
        if (c == '"') {
            js_err_ = true; /* string so em print/alert */
            return 0;
        }
        char name[24];
        size_t n = 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
            js_err_ = true;
            return 0;
        }
        while (((*js_ >= 'a' && *js_ <= 'z') || (*js_ >= 'A' && *js_ <= 'Z') ||
                (*js_ >= '0' && *js_ <= '9') || *js_ == '_') &&
               n + 1 < sizeof(name)) {
            name[n++] = *js_++;
        }
        name[n] = '\0';
        JVar* v = js_var(name, false);
        if (!v) {
            js_err_ = true;
            return 0;
        }
        return v->value;
    }

    int64_t js_term()
    {
        int64_t v = js_factor();
        for (;;) {
            js_skip();
            char c = *js_;
            if (c != '*' && c != '/' && c != '%') {
                break;
            }
            js_++;
            int64_t r = js_factor();
            if ((c == '/' || c == '%') && r == 0) {
                js_err_ = true;
                return 0;
            }
            if (c == '*') {
                v = v * r;
            } else if (c == '/') {
                v = v / r;
            } else {
                v = v % r;
            }
        }
        return v;
    }

    int64_t js_expr()
    {
        int64_t v = js_term();
        for (;;) {
            js_skip();
            char c = *js_;
            if (c != '+' && c != '-') {
                break;
            }
            js_++;
            int64_t r = js_term();
            v = (c == '+') ? v + r : v - r;
        }
        return v;
    }

    bool js_cond()
    {
        int64_t a = js_expr();
        js_skip();
        const char* op = js_;
        bool two = (js_[0] == '=' && js_[1] == '=') || (js_[0] == '!' && js_[1] == '=') ||
                   (js_[0] == '<' && js_[1] == '=') || (js_[0] == '>' && js_[1] == '=');
        char c1 = js_[0];
        if (two) {
            js_ += 2;
        } else if (c1 == '<' || c1 == '>') {
            js_ += 1;
        } else {
            return a != 0;
        }
        int64_t b = js_expr();
        if (two && op[0] == '=') {
            return a == b;
        }
        if (two && op[0] == '!') {
            return a != b;
        }
        if (two && op[0] == '<') {
            return a <= b;
        }
        if (two && op[0] == '>') {
            return a >= b;
        }
        if (c1 == '<') {
            return a < b;
        }
        return a > b;
    }

    void js_stmt(bool exec)
    {
        js_skip();
        if (!*js_ || *js_ == '}') {
            return;
        }
        if (js_kw("var")) {
            char name[24];
            size_t n = 0;
            js_skip();
            while (((*js_ >= 'a' && *js_ <= 'z') || (*js_ >= 'A' && *js_ <= 'Z') || *js_ == '_') &&
                   n + 1 < sizeof(name)) {
                name[n++] = *js_++;
            }
            name[n] = '\0';
            int64_t v = 0;
            if (js_accept('=')) {
                v = js_expr();
            }
            if (exec && !js_err_) {
                JVar* vv = js_var(name, true);
                if (vv) {
                    vv->value = v;
                }
            }
            js_accept(';');
            return;
        }
        bool is_alert = false;
        bool is_print = false;
        {
            /* evita consumir: testa alert, senao print */
            const char* save = js_;
            (void)save;
        }
        if (js_kw("alert")) {
            is_alert = true;
        } else if (js_kw("print")) {
            is_print = true;
        }
        if (is_alert || is_print) {
            if (!js_accept('(')) {
                js_err_ = true;
                return;
            }
            js_skip();
            if (*js_ == '"') {
                char s[128];
                size_t n = 0;
                js_++;
                while (*js_ && *js_ != '"' && n + 1 < sizeof(s)) {
                    s[n++] = *js_++;
                }
                s[n] = '\0';
                if (*js_ == '"') {
                    js_++;
                }
                if (exec && !js_err_) {
                    if (is_alert) {
                        set_status(s);
                    } else {
                        emit_text(s, -1, 0);
                        new_line(-1, 0);
                    }
                }
            } else {
                int64_t v = js_expr();
                if (exec && !js_err_) {
                    char b[24];
                    int nn = 0;
                    bool neg = false;
                    uint64_t u;
                    if (v < 0) {
                        neg = true;
                        u = (uint64_t)(-(v + 1)) + 1;
                    } else {
                        u = (uint64_t)v;
                    }
                    char tmp[24];
                    int k = 0;
                    if (u == 0) {
                        tmp[k++] = '0';
                    }
                    while (u > 0 && k < 24) {
                        tmp[k++] = (char)('0' + u % 10);
                        u /= 10;
                    }
                    if (neg && nn < 23) {
                        b[nn++] = '-';
                    }
                    while (k > 0 && nn < 23) {
                        b[nn++] = tmp[--k];
                    }
                    b[nn] = '\0';
                    emit_text(b, -1, 0);
                    new_line(-1, 0);
                }
            }
            if (!js_accept(')')) {
                js_err_ = true;
            }
            js_accept(';');
            return;
        }
        if (js_kw("if")) {
            bool c = false;
            if (!js_accept('(')) {
                js_err_ = true;
                return;
            }
            c = js_cond();
            if (!js_accept(')')) {
                js_err_ = true;
                return;
            }
            js_block_or_stmt(exec && c && !js_err_);
            return;
        }
        if (js_kw("while")) {
            const char* again = nullptr;
            if (!js_accept('(')) {
                js_err_ = true;
                return;
            }
            /* reavalia: guarda posicao (sem blocos aninhados complexos: limite 200 iters) */
            again = js_;
            for (int it = 0; it < 200; it++) {
                js_ = again;
                bool c = js_cond();
                if (!js_accept(')')) {
                    js_err_ = true;
                    return;
                }
                if (!c || js_err_) {
                    js_block_or_stmt(false);
                    break;
                }
                js_block_or_stmt(exec && !js_err_);
                if (js_err_) {
                    break;
                }
            }
            return;
        }
        /* atribuicao ou expressao */
        {
            const char* save = js_;
            char name[24];
            size_t n = 0;
            while (((*js_ >= 'a' && *js_ <= 'z') || (*js_ >= 'A' && *js_ <= 'Z') || *js_ == '_') &&
                   n + 1 < sizeof(name)) {
                name[n++] = *js_++;
            }
            name[n] = '\0';
            js_skip();
            if (n > 0 && *js_ == '=' && js_[1] != '=') {
                js_++;
                int64_t v = js_expr();
                if (exec && !js_err_) {
                    JVar* vv = js_var(name, false);
                    if (!vv) {
                        js_err_ = true;
                    } else {
                        vv->value = v;
                    }
                }
                js_accept(';');
                return;
            }
            js_ = save;
            (void)js_expr();
            js_accept(';');
        }
    }

    void js_block_or_stmt(bool exec)
    {
        js_skip();
        if (js_accept('{')) {
            while (!js_err_) {
                js_skip();
                if (js_accept('}')) {
                    break;
                }
                if (!*js_) {
                    js_err_ = true;
                    break;
                }
                js_stmt(exec);
            }
            return;
        }
        js_stmt(exec);
    }

    void script_run(const char* code)
    {
        for (int i = 0; i < 32; i++) {
            jvars_[i].used = 0;
        }
        js_ = code;
        js_err_ = false;
        while (*js_ && !js_err_) {
            js_skip();
            if (!*js_) {
                break;
            }
            js_stmt(true);
        }
        if (js_err_) {
            set_status("script erro");
        }
    }

    void paint_line(int li, int link, int kind)
    {
        if (kind == 3) {
            linefg_[li] = 0x000000;
            linebg_[li] = 0xFFFFFF;
        } else if (style_inline_) {
            linefg_[li] = cur_fg_;
            linebg_[li] = cur_bg_;
        } else if (kind == 2) {
            linefg_[li] = css_a_on_ ? css_a_ : 0x0000CC;
            linebg_[li] = cur_bg_;
        } else if (kind == 1) {
            linefg_[li] = css_h1_on_ ? css_h1_ : 0x000080;
            linebg_[li] = cur_bg_;
        } else {
            linefg_[li] = css_p_on_ ? css_p_ : cur_fg_;
            linebg_[li] = cur_bg_;
        }
    }

    void new_line(int link, int kind)
    {
        if (nlines_ >= 200) {
            return;
        }
        lines_[nlines_][0] = '\0';
        linelink_[nlines_] = link;
        linekind_[nlines_] = (uint8_t)kind;
        paint_line(nlines_, link, kind);
        linealign_[nlines_] = (uint8_t)(center_depth_ > 0 ? 1 : 0);
        lineimgw_[nlines_] = 0;
        lineimgh_[nlines_] = 0;
        nlines_++;
    }

    static uint32_t css_named(const char* s)
    {
        struct {
            const char* n;
            uint32_t c;
        } tab[] = {
            { "black", 0x000000 }, { "white", 0xFFFFFF },
            { "red", 0xCC0000 }, { "green", 0x00AA00 },
            { "blue", 0x0000CC }, { "yellow", 0x999900 },
            { "gray", 0x808080 }, { "grey", 0x808080 },
            { "orange", 0xDD7700 }, { "purple", 0x880088 },
            { "teal", 0x008080 }, { "navy", 0x000080 },
        };
        for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
            const char* a = s;
            const char* b = tab[i].n;
            while (*a && *a == *b) {
                a++;
                b++;
            }
            if (*a == '\0' && *b == '\0') {
                return tab[i].c;
            }
        }
        return 0x000000;
    }

    static uint32_t css_value(const char* s)
    {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (s[0] == '#') {
            uint32_t v = 0;
            int n = 0;
            s++;
            while (((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
                    (*s >= 'A' && *s <= 'F')) &&
                   n < 6) {
                char c = *s++;
                uint32_t d = (uint32_t)(c <= '9' ? c - '0' : ((c & 0xDF) - 'A' + 10));
                v = (v << 4) | d;
                n++;
            }
            if (n == 6) {
                return v;
            }
            return 0x000000;
        }
        {
            char name[24];
            size_t k = 0;
            while (((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) && k < sizeof(name) - 1) {
                char c = *s++;
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c + 32);
                }
                name[k++] = c;
            }
            name[k] = '\0';
            return css_named(name);
        }
    }

    /* style="color:X;background:Y" -> aplica em cur_* (membro!) */
    void css_inline(const char* style, uint32_t* fg, uint32_t* bg)
    {
        bool touched = false;
        while (*style) {
            while (*style == ' ' || *style == ';' || *style == '\t') {
                style++;
            }
            if (strncmp(style, "color", 5) == 0 &&
                (style[5] == ':' || style[5] == ' ')) {
                style += 5;
                while (*style == ' ' || *style == ':') {
                    style++;
                }
                *fg = css_value(style);
                touched = true;
                while (*style && *style != ';') {
                    style++;
                }
            } else if ((strncmp(style, "background", 10) == 0 ||
                        strncmp(style, "background-color", 16) == 0)) {
                while (*style && *style != ':') {
                    style++;
                }
                if (*style == ':') {
                    style++;
                }
                *bg = css_value(style);
                while (*style && *style != ';') {
                    style++;
                }
            } else {
                while (*style && *style != ';') {
                    style++;
                }
            }
        }
        if (touched) {
            style_inline_ = 1;
        }
    }

    /* extrai style="..." da tag (s aponta p/ dentro de <...>) */
    static void tag_style(const char* s, char* out, size_t cap)
    {
        out[0] = '\0';
        while (*s && *s != '>') {
            if (strncmp(s, "style", 5) == 0 && (s[5] == '=' || s[5] == ' ')) {
                s += 5;
                while (*s == ' ') {
                    s++;
                }
                if (*s == '=') {
                    s++;
                }
                while (*s == ' ') {
                    s++;
                }
                {
                    char q = 0;
                    size_t n = 0;
                    if (*s == '"' || *s == '\'') {
                        q = *s++;
                    }
                    while (*s && *s != '>' && *s != q && n + 1 < cap) {
                        out[n++] = *s++;
                    }
                    out[n] = '\0';
                    return;
                }
            }
            s++;
        }
    }

    /* <style>h1{color:red}p{...}a{...}</style> */
    void css_block(const char* s)
    {
        while (*s) {
            char sel[8];
            size_t k = 0;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
                s++;
            }
            while (((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                    (*s >= '0' && *s <= '9')) &&
                   k < sizeof(sel) - 1) {
                char c = *s++;
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c + 32);
                }
                sel[k++] = c;
            }
            sel[k] = '\0';
            while (*s && *s != '{') {
                s++;
            }
            if (*s != '{') {
                return;
            }
            s++;
            {
                uint32_t fg = 0, bg = 0xFFFFFF;
                bool has_fg = false, has_bg = false;
                char body[128];
                size_t b = 0;
                while (*s && *s != '}' && b < sizeof(body) - 1) {
                    body[b++] = *s++;
                }
                body[b] = '\0';
                if (*s == '}') {
                    s++;
                }
                /* procura color: e background: */
                {
                    const char* p = body;
                    while (*p) {
                        while (*p == ' ' || *p == ';') {
                            p++;
                        }
                        if (strncmp(p, "color", 5) == 0 && (p[5] == ':' || p[5] == ' ')) {
                            p += 5;
                            while (*p == ' ' || *p == ':') {
                                p++;
                            }
                            fg = css_value(p);
                            has_fg = true;
                        } else if (strncmp(p, "background", 10) == 0) {
                            while (*p && *p != ':') {
                                p++;
                            }
                            if (*p == ':') {
                                p++;
                            }
                            bg = css_value(p);
                            has_bg = true;
                        }
                        while (*p && *p != ';' && *p != '}') {
                            p++;
                        }
                    }
                }
                (void)has_bg;
                if (strcmp(sel, "h1") == 0 && has_fg) {
                    css_h1_ = fg;
                    css_h1_on_ = 1;
                } else if (strcmp(sel, "p") == 0 && has_fg) {
                    css_p_ = fg;
                    css_p_on_ = 1;
                } else if (strcmp(sel, "a") == 0 && has_fg) {
                    css_a_ = fg;
                    css_a_on_ = 1;
                }
            }
        }
    }

    void add_link(const char* text, const char* href)
    {
        int id = -1;
        if (nlinks_ < 32) {
            id = nlinks_++;
            copy_str(links_[id].text, text, sizeof(links_[id].text));
            copy_str(links_[id].href, href, sizeof(links_[id].href));
        }
        emit_text(text, id, 2);
    }

    void parse(const char* s)
    {
        size_t i = 0;
        nlines_ = 0;
        nlinks_ = 0;
        imgpool_n_ = 0;
        cur_fg_ = 0x000000;
        cur_bg_ = 0xFFFFFF;
        style_inline_ = 0;
        css_h1_on_ = 0;
        css_p_on_ = 0;
        css_a_on_ = 0;
        center_depth_ = 0;
        new_line(-1, 0);
        while (s[i]) {
            if (s[i] == '<') {
                size_t j = i + 1;
                char tag[16];
                int tn = 0;
                bool closing = false;
                if (s[j] == '/') {
                    closing = true;
                    j++;
                }
                while (s[j] && s[j] != '>' && s[j] != ' ' && tn < 15) {
                    char c = s[j];
                    if (c >= 'A' && c <= 'Z') {
                        c = (char)(c + 32);
                    }
                    tag[tn++] = c;
                    j++;
                }
                tag[tn] = '\0';
                if (tn == 1 && tag[0] == 'a' && !closing) {
                    /* <a href="...">texto</a> */
                    char href[96] = { 0 };
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        if (s[k] == 'h' && strncmp(s + k, "href=", 5) == 0) {
                            k += 5;
                            char q = 0;
                            if (s[k] == '"' || s[k] == '\'') {
                                q = s[k++];
                            }
                            size_t h = 0;
                            while (s[k] && s[k] != '>' && s[k] != q &&
                                   s[k] != ' ' && h < sizeof(href) - 1) {
                                href[h++] = s[k++];
                            }
                            href[h] = '\0';
                        } else {
                            k++;
                        }
                    }
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    if (s[k] == '>') {
                        k++;
                    }
                    /* coleta texto ate </a> */
                    char txt[96];
                    size_t t = 0;
                    while (s[k] && !(s[k] == '<' && s[k + 1] == '/') && t < sizeof(txt) - 1) {
                        txt[t++] = s[k++];
                    }
                    txt[t] = '\0';
                    add_link(txt, href[0] ? href : "/");
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    if (s[k] == '>') {
                        k++;
                    }
                    i = k;
                    continue;
                }
                if (tn == 2 && tag[0] == 'h' && tag[1] == '1' && !closing) {
                    /* <h1>texto</h1> */
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    if (s[k] == '>') {
                        k++;
                    }
                    char txt[96];
                    size_t t = 0;
                    while (s[k] && !(s[k] == '<' && s[k + 1] == '/') && t < sizeof(txt) - 1) {
                        txt[t++] = s[k++];
                    }
                    txt[t] = '\0';
                    new_line(-1, 0);
                    emit_text(txt, -1, 1);
                    new_line(-1, 0);
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    if (s[k] == '>') {
                        k++;
                    }
                    i = k;
                    continue;
                }
                if ((tn == 1 && (tag[0] == 'p' || tag[0] == 'b')) && !closing) {
                    char st[96];
                    tag_style(s + j, st, sizeof(st));
                    new_line(-1, 0);
                    if (st[0]) {
                        css_inline(st, &cur_fg_, &cur_bg_);
                    }
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 1 && (tag[0] == 'h' || tag[0] == 'u') && !closing) {
                    /* h2/h3 como h1; ul so abre bloco */
                    char st[96];
                    tag_style(s, st, sizeof(st));
                    new_line(-1, 0);
                    if (st[0]) {
                        css_inline(st, &cur_fg_, &cur_bg_);
                    }
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 2 && tag[0] == 'l' && tag[1] == 'i' && !closing) {
                    new_line(-1, 0);
                    emit_text("- ", -1, 0);
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 6 && strncmp(tag, "center", 6) == 0) {
                    if (!closing) {
                        center_depth_++;
                    } else if (center_depth_ > 0) {
                        center_depth_--;
                    }
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 5 && strncmp(tag, "style", 5) == 0 && !closing) {
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    if (s[k] == '>') {
                        k++;
                    }
                    {
                        char body[256];
                        size_t t = 0;
                        while (s[k] && !(s[k] == '<' && s[k + 1] == '/') && t < sizeof(body) - 1) {
                            body[t++] = s[k++];
                        }
                        body[t] = '\0';
                        css_block(body);
                    }
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 3 && strncmp(tag, "img", 3) == 0 && !closing) {
                    /* <img src="..."> */
                    char src[128] = { 0 };
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        if (s[k] == 's' && strncmp(s + k, "src=", 4) == 0) {
                            size_t q = k + 4;
                            char qq = 0;
                            size_t h = 0;
                            if (s[q] == '"' || s[q] == '\'') {
                                qq = s[q++];
                            }
                            while (s[q] && s[q] != '>' && s[q] != qq && s[q] != ' ' && h < sizeof(src) - 1) {
                                src[h++] = s[q++];
                            }
                            src[h] = '\0';
                            k = q; /* avanca: sem isso, loop infinito */
                        } else {
                            k++;
                        }
                    }
                    if (src[0]) {
                        img_tag(src);
                    }
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 6 && strncmp(tag, "script", 6) == 0 && !closing) {
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    if (s[k] == '>') {
                        k++;
                    }
                    {
                        char code[1024];
                        size_t t = 0;
                        while (s[k] && !(s[k] == '<' && s[k + 1] == '/') && t < sizeof(code) - 1) {
                            code[t++] = s[k++];
                        }
                        code[t] = '\0';
                        script_run(code);
                    }
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 2 && tag[0] == 'b' && tag[1] == 'r') {
                    new_line(-1, 0);
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 2 && tag[0] == 'h' && tag[1] == 'r') {
                    new_line(-1, 3);
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                if (tn == 5 && tag[0] == 't' && tag[1] == 'i') {
                    /* <title>: usa como titulo da janela */
                    size_t k = j;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    if (s[k] == '>') {
                        k++;
                    }
                    size_t t = 0;
                    while (s[k] && !(s[k] == '<' && s[k + 1] == '/') && t < 31) {
                        title_[t++] = s[k++];
                    }
                    title_[t] = '\0';
                    has_title_ = true;
                    while (s[k] && s[k] != '>') {
                        k++;
                    }
                    i = (s[k] == '>') ? k + 1 : k;
                    continue;
                }
                /* tag desconhecida (ou fechamento): pula; reset de estilo */
                if (closing) {
                    cur_fg_ = 0x000000;
                    cur_bg_ = 0xFFFFFF;
                    style_inline_ = 0;
                }
                while (s[j] && s[j] != '>') {
                    j++;
                }
                i = (s[j] == '>') ? j + 1 : j;
                continue;
            }
            /* texto corrido ate '<' */
            {
                char txt[128];
                size_t t = 0;
                while (s[i] && s[i] != '<' && t < sizeof(txt) - 1) {
                    txt[t++] = s[i++];
                }
                txt[t] = '\0';
                if (t > 0) {
                    /* arquivo sem tags = texto puro */
                    emit_text(txt, -1, 0);
                }
            }
        }
        if (nlines_ <= 0) {
            new_line(-1, 0);
        }
    }


public:
    const char* window_title() const
    {
        return has_title_ ? title_ : "Navegador";
    }
};

static Browser s_browser; // POD zeroed (.bss) + open() inicializa

} // namespace mapple

extern "C" int gui_web_modal(void);

/* Fronteira C */
extern "C" {

void browser_open(const char* url)
{
    mapple::s_browser.open_window(url);
}

bool browser_is_open(void)
{
    return mapple::s_browser.is_open();
}

void browser_close(void)
{
    mapple::s_browser.close();
}

void browser_draw(int cx, int cy, int cw, int ch, int mx, int my)
{
    mapple::s_browser.draw(cx, cy, cw, ch, mx, my);
}

int browser_click(int lx, int ly, int cw, int ch)
{
    return mapple::s_browser.click(lx, ly, cw, ch) ? 1 : 0;
}

void browser_run_loop(void)
{
    mapple::s_browser.run_loop();
}

const char* browser_title(void)
{
    return mapple::s_browser.window_title();
}

} // extern "C"
