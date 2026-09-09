/* cc.c - Compilador proprio do mapple (subconjunto C, execucao direta).
 *
 * Suporta: int main() { ... }, declaracoes `int x = expr;`,
 * atribuicoes, + - * / % ( ), - unario, `print(expr);`, `return expr;`.
 * Uso: cc /caminho/prog.c   |   calc 2+3*4  (usa o mesmo parser)
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, size_t n);
extern void* memset(void* dst, int c, size_t n);
extern void console_putc(char c);
extern void console_puts(const char* s);
extern void console_print_i64(int64_t v);
extern int fs_read(const char* path, uint8_t* buf, size_t cap);

typedef struct {
    const char* src;
    size_t pos;
    int err;
    char msg[64];
} cc_lex_t;

#define CC_MAX_VARS 64
typedef struct {
    char name[32];
    int64_t value;
    int used;
} cc_var_t;

static cc_var_t cc_vars[CC_MAX_VARS];

static void cc_fail(cc_lex_t* l, const char* msg)
{
    l->err = 1;
    size_t i = 0;
    while (msg[i] && i < sizeof(l->msg) - 1) {
        l->msg[i] = msg[i];
        i++;
    }
    l->msg[i] = '\0';
}

static void cc_skip(cc_lex_t* l)
{
    for (;;) {
        char c = l->src[l->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            l->pos++;
        } else if (c == '/' && l->src[l->pos + 1] == '/') {
            while (l->src[l->pos] && l->src[l->pos] != '\n') l->pos++;
        } else {
            break;
        }
    }
}

static int cc_accept(cc_lex_t* l, char c)
{
    cc_skip(l);
    if (l->src[l->pos] == c) {
        l->pos++;
        return 1;
    }
    return 0;
}

static int cc_accept_kw(cc_lex_t* l, const char* kw)
{
    cc_skip(l);
    size_t n = strlen(kw);
    if (strncmp(l->src + l->pos, kw, n) == 0) {
        char after = l->src[l->pos + n];
        if ((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
            (after >= '0' && after <= '9') || after == '_') {
            return 0;
        }
        l->pos += n;
        return 1;
    }
    return 0;
}

static int cc_parse_ident(cc_lex_t* l, char* out, size_t cap)
{
    cc_skip(l);
    size_t n = 0;
    char c = l->src[l->pos];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
        return 0;
    }
    while (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') && n + 1 < cap) {
        out[n++] = c;
        l->pos++;
        c = l->src[l->pos];
    }
    out[n] = '\0';
    return 1;
}

static cc_var_t* cc_var_get(const char* name, int create)
{
    for (int i = 0; i < CC_MAX_VARS; i++) {
        if (cc_vars[i].used && strcmp(cc_vars[i].name, name) == 0) {
            return &cc_vars[i];
        }
    }
    if (!create) {
        return 0;
    }
    for (int i = 0; i < CC_MAX_VARS; i++) {
        if (!cc_vars[i].used) {
            cc_vars[i].used = 1;
            size_t k = 0;
            while (name[k] && k < sizeof(cc_vars[i].name) - 1) {
                cc_vars[i].name[k] = name[k];
                k++;
            }
            cc_vars[i].name[k] = '\0';
            cc_vars[i].value = 0;
            return &cc_vars[i];
        }
    }
    return 0;
}

static int64_t cc_expr(cc_lex_t* l);

static int64_t cc_number(cc_lex_t* l)
{
    cc_skip(l);
    int64_t v = 0;
    int any = 0;
    while (l->src[l->pos] >= '0' && l->src[l->pos] <= '9') {
        v = v * 10 + (l->src[l->pos] - '0');
        l->pos++;
        any = 1;
    }
    if (!any) {
        cc_fail(l, "numero esperado");
    }
    return v;
}

static int64_t cc_factor(cc_lex_t* l)
{
    cc_skip(l);
    char c = l->src[l->pos];
    if (c == '(') {
        l->pos++;
        int64_t v = cc_expr(l);
        if (!cc_accept(l, ')')) {
            cc_fail(l, "')' esperado");
        }
        return v;
    }
    if (c == '-') {
        l->pos++;
        return -cc_factor(l);
    }
    if (c >= '0' && c <= '9') {
        return cc_number(l);
    }
    char name[32];
    if (cc_parse_ident(l, name, sizeof(name))) {
        cc_var_t* v = cc_var_get(name, 0);
        if (!v) {
            cc_fail(l, "variavel desconhecida");
            return 0;
        }
        return v->value;
    }
    cc_fail(l, "expressao invalida");
    return 0;
}

static int64_t cc_term(cc_lex_t* l)
{
    int64_t v = cc_factor(l);
    for (;;) {
        cc_skip(l);
        char c = l->src[l->pos];
        if (c == '*' || c == '/' || c == '%') {
            l->pos++;
            int64_t r = cc_factor(l);
            if ((c == '/' || c == '%') && r == 0) {
                cc_fail(l, "divisao por zero");
                return 0;
            }
            if (c == '*') v = v * r;
            else if (c == '/') v = v / r;
            else v = v % r;
        } else {
            break;
        }
    }
    return v;
}

static int64_t cc_expr(cc_lex_t* l)
{
    int64_t v = cc_term(l);
    for (;;) {
        cc_skip(l);
        char c = l->src[l->pos];
        if (c == '+' || c == '-') {
            /* cuidado: "a - -b" ok; "--" nao existe aqui */
            l->pos++;
            int64_t r = cc_term(l);
            if (c == '+') v = v + r;
            else v = v - r;
        } else {
            break;
        }
    }
    return v;
}

/* Uma instrucao. Retorna 1 se foi `return` (com codigo em *ret). */
static int cc_stmt(cc_lex_t* l, int64_t* ret)
{
    cc_skip(l);
    if (!l->src[l->pos] || l->src[l->pos] == '}') {
        return 0;
    }
    if (cc_accept_kw(l, "int")) {
        char name[32];
        if (!cc_parse_ident(l, name, sizeof(name))) {
            cc_fail(l, "nome esperado apos int");
            return 0;
        }
        int64_t v = 0;
        if (cc_accept(l, '=')) {
            v = cc_expr(l);
        }
        if (!cc_accept(l, ';')) {
            cc_fail(l, "';' esperado");
            return 0;
        }
        cc_var_t* var = cc_var_get(name, 1);
        if (!var) {
            cc_fail(l, "muitas variaveis");
            return 0;
        }
        var->value = v;
        return 0;
    }
    if (cc_accept_kw(l, "print")) {
        if (!cc_accept(l, '(')) {
            cc_fail(l, "'(' esperado apos print");
            return 0;
        }
        int64_t v = cc_expr(l);
        if (!cc_accept(l, ')') || !cc_accept(l, ';')) {
            cc_fail(l, "');' esperado");
            return 0;
        }
        console_print_i64(v);
        console_putc('\n');
        return 0;
    }
    if (cc_accept_kw(l, "return")) {
        int64_t v = 0;
        cc_skip(l);
        if (l->src[l->pos] != ';') {
            v = cc_expr(l);
        }
        if (!cc_accept(l, ';')) {
            cc_fail(l, "';' esperado");
            return 0;
        }
        *ret = v;
        return 1;
    }
    /* atribuicao ou expressao solta */
    size_t save = l->pos;
    char name[32];
    if (cc_parse_ident(l, name, sizeof(name))) {
        cc_skip(l);
        if (l->src[l->pos] == '=' && l->src[l->pos + 1] != '=') {
            l->pos++;
            int64_t v = cc_expr(l);
            if (!cc_accept(l, ';')) {
                cc_fail(l, "';' esperado");
                return 0;
            }
            cc_var_t* var = cc_var_get(name, 0);
            if (!var) {
                cc_fail(l, "variavel desconhecida");
                return 0;
            }
            var->value = v;
            return 0;
        }
        l->pos = save; /* era expressao: volta e avalia */
    }
    (void)cc_expr(l);
    if (!cc_accept(l, ';')) {
        cc_fail(l, "';' esperado");
    }
    return 0;
}

static int64_t cc_block(cc_lex_t* l)
{
    int64_t ret = 0;
    for (;;) {
        cc_skip(l);
        if (!l->src[l->pos] || l->src[l->pos] == '}') {
            break;
        }
        if (cc_stmt(l, &ret)) {
            break;
        }
        if (l->err) {
            break;
        }
    }
    return ret;
}

/* Avalia expressao avulsa (usado pelo applet calc). */
int cc_eval_expr(const char* s, int64_t* out)
{
    cc_lex_t l;
    l.src = s;
    l.pos = 0;
    l.err = 0;
    memset(cc_vars, 0, sizeof(cc_vars));
    int64_t v = cc_expr(&l);
    cc_skip(&l);
    if (l.err || l.src[l.pos] != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

/* Compila+executa arquivo .c do FS. */
int cc_run_file(const char* path)
{
    static uint8_t buf[8192];
    int n = fs_read(path, buf, sizeof(buf) - 1);
    if (n < 0) {
        console_puts("cc: arquivo nao encontrado: ");
        console_puts(path);
        console_putc('\n');
        return -1;
    }
    buf[n] = '\0';

    cc_lex_t l;
    l.src = (const char*)buf;
    l.pos = 0;
    l.err = 0;
    memset(cc_vars, 0, sizeof(cc_vars));

    /* opcional: int main() { ... } */
    size_t save = 0;
    if (cc_accept_kw(&l, "int")) {
        char name[32];
        if (cc_parse_ident(&l, name, sizeof(name)) && strcmp(name, "main") == 0 &&
            cc_accept(&l, '(') && cc_accept(&l, ')') && cc_accept(&l, '{')) {
            int64_t ret = cc_block(&l);
            if (!l.err && !cc_accept(&l, '}')) {
                cc_fail(&l, "'}' esperado");
            }
            if (l.err) {
                console_puts("cc: erro: ");
                console_puts(l.msg);
                console_putc('\n');
                return -1;
            }
            console_puts("[exit ");
            console_print_i64(ret);
            console_puts("]\n");
            return 0;
        }
        l.pos = save; /* nao era main: interpreta como bloco solto */
    }

    int64_t ret = cc_block(&l);
    (void)ret;
    cc_skip(&l);
    if (l.err || l.src[l.pos] != '\0') {
        console_puts("cc: erro: ");
        console_puts(l.err ? l.msg : "codigo apos fim do programa");
        console_putc('\n');
        return -1;
    }
    return 0;
}
