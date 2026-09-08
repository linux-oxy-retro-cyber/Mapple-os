/* fs.c - Sistema de arquivos do mapple.
 *
 * Camada 0 (ROM): ramfs embutido pelo Makefile (rootfs/ -> ramfs_data.c).
 * Camada 1 (RW): overlay em RAM (ate 32 arquivos de 2KB + 32 diretorios).
 * escrita sombreia a ROM; rm de arquivo ROM e recusado (como num FS real).
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

extern void console_putc(char c);
extern void console_puts(const char* s);
extern void console_print_u64(uint64_t v);

typedef struct ramfs_entry {
    const char* name;
    const uint8_t* data;
    uint32_t size;
} ramfs_entry_t;

extern const ramfs_entry_t ramfs_table[];
extern const uint32_t ramfs_count;
extern const char* ramfs_dirs[];
extern const uint32_t ramfs_ndirs;

#define FS_MAX_FILES 32
#define FS_MAX_SIZE  2048
#define FS_MAX_DIRS  32
#define FS_PATH_MAX  128

typedef struct {
    int used;
    char path[FS_PATH_MAX];
    uint8_t data[FS_MAX_SIZE];
    uint32_t size;
} fs_file_t;

static fs_file_t fs_files[FS_MAX_FILES];
static char fs_dirs[FS_MAX_DIRS][FS_PATH_MAX];
static uint32_t fs_ndirs = 0;

/* Resolve `in` (absoluto ou relativo a `cwd`) em `out` (normalizado). */
void fs_resolve(const char* cwd, const char* in, char* out, size_t cap)
{
    char tmp[FS_PATH_MAX * 2];
    size_t n = 0;

    if (!in || !*in) {
        in = ".";
    }
    if (in[0] != '/') {
        for (size_t i = 0; cwd[i] && n < sizeof(tmp) - 1; i++) {
            tmp[n++] = cwd[i];
        }
        if (n == 0 || tmp[n - 1] != '/') {
            tmp[n++] = '/';
        }
    }
    for (size_t i = 0; in[i] && n < sizeof(tmp) - 1; i++) {
        tmp[n++] = in[i];
    }
    tmp[n] = '\0';

    /* normaliza: tokens separados por '/', trata '.' e '..' */
    char stack[32][64];
    size_t depth = 0;
    size_t i = 0;
    while (tmp[i]) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        size_t j = i;
        while (tmp[j] && tmp[j] != '/') j++;
        size_t len = j - i;
        if (len == 1 && tmp[i] == '.') {
            /* ignora */
        } else if (len == 2 && tmp[i] == '.' && tmp[i + 1] == '.') {
            if (depth > 0) depth--;
        } else if (depth < 32 && len < 64) {
            for (size_t k = 0; k < len; k++) {
                stack[depth][k] = tmp[i + k];
            }
            stack[depth][len] = '\0';
            depth++;
        }
        i = j;
    }

    size_t o = 0;
    out[o++] = '/';
    for (size_t d = 0; d < depth && o < cap - 1; d++) {
        size_t k = 0;
        while (stack[d][k] && o < cap - 1) {
            out[o++] = stack[d][k++];
        }
        if (d + 1 < depth && o < cap - 1) {
            out[o++] = '/';
        }
    }
    out[o] = '\0';
}

static fs_file_t* fs_find_ov(const char* path)
{
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
        if (fs_files[i].used && strcmp(fs_files[i].path, path) == 0) {
            return &fs_files[i];
        }
    }
    return 0;
}

static const ramfs_entry_t* fs_find_rom(const char* path)
{
    for (uint32_t i = 0; i < ramfs_count; i++) {
        if (strcmp(ramfs_table[i].name, path) == 0) {
            return &ramfs_table[i];
        }
    }
    return 0;
}

static int fs_has_children(const char* dir, size_t dl)
{
    for (uint32_t i = 0; i < ramfs_count; i++) {
        if (strncmp(ramfs_table[i].name, dir, dl) == 0 && ramfs_table[i].name[dl]) {
            return 1;
        }
    }
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
        if (fs_files[i].used && strncmp(fs_files[i].path, dir, dl) == 0 && fs_files[i].path[dl]) {
            return 1;
        }
    }
    return 0;
}

int fs_isdir(const char* path)
{
    uint32_t i;
    if (strcmp(path, "/") == 0) {
        return 1;
    }
    for (i = 0; i < ramfs_ndirs; i++) {
        if (strcmp(ramfs_dirs[i], path) == 0) {
            return 1;
        }
    }
    for (i = 0; i < fs_ndirs; i++) {
        if (strcmp(fs_dirs[i], path) == 0) {
            return 1;
        }
    }
    /* diretorio implicito: tem filhos */
    char prefix[FS_PATH_MAX];
    size_t n = strlen(path);
    if (n + 2 > sizeof(prefix)) {
        return 0;
    }
    strcpy(prefix, path);
    prefix[n] = '/';
    prefix[n + 1] = '\0';
    return fs_has_children(prefix, n + 1);
}

int fs_exists(const char* path)
{
    return fs_find_ov(path) != 0 || fs_find_rom(path) != 0 || fs_isdir(path);
}

/* Le arquivo em buf (cap). Retorna tamanho ou -1. */
int fs_read(const char* path, uint8_t* buf, size_t cap)
{
    fs_file_t* f = fs_find_ov(path);
    if (f) {
        if (f->size > cap) {
            return -1;
        }
        memcpy(buf, f->data, f->size);
        return (int)f->size;
    }
    const ramfs_entry_t* r = fs_find_rom(path);
    if (r) {
        if (r->size > cap) {
            return -1;
        }
        memcpy(buf, r->data, r->size);
        return (int)r->size;
    }
    return -1;
}

/* Escreve (cria ou sombreia). Retorna 0 ok, -1 erro. */
int fs_write(const char* path, const uint8_t* data, size_t size)
{
    if (size > FS_MAX_SIZE || strlen(path) >= FS_PATH_MAX) {
        return -1;
    }
    if (fs_isdir(path)) {
        return -1;
    }
    fs_file_t* f = fs_find_ov(path);
    if (!f) {
        for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
            if (!fs_files[i].used) {
                f = &fs_files[i];
                break;
            }
        }
    }
    if (!f) {
        return -1; /* overlay cheio */
    }
    f->used = 1;
    strcpy(f->path, path);
    if (size) {
        memcpy(f->data, data, size);
    }
    f->size = (uint32_t)size;
    return 0;
}

int fs_touch(const char* path)
{
    if (fs_find_ov(path) || fs_find_rom(path)) {
        return 0; /* ja existe */
    }
    return fs_write(path, 0, 0);
}

int fs_mkdir(const char* path)
{
    if (fs_exists(path)) {
        return -1;
    }
    /* pai precisa ser diretorio */
    char parent[FS_PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= FS_PATH_MAX) {
        return -1;
    }
    size_t cut = n;
    while (cut > 1 && path[cut - 1] != '/') cut--;
    if (cut <= 1) {
        strcpy(parent, "/");
    } else {
        for (size_t i = 0; i < cut - 1 && i < sizeof(parent) - 1; i++) {
            parent[i] = path[i];
        }
        parent[cut - 1] = '\0';
    }
    if (!fs_isdir(parent)) {
        return -1;
    }
    if (fs_ndirs >= FS_MAX_DIRS) {
        return -1;
    }
    strcpy(fs_dirs[fs_ndirs++], path);
    return 0;
}

int fs_rm(const char* path)
{
    fs_file_t* f = fs_find_ov(path);
    if (f) {
        f->used = 0;
        return 0;
    }
    if (fs_find_rom(path)) {
        return -2; /* ROM: somente leitura */
    }
    return -1;
}

/* Itera filhos diretos de dir: cb(nome, is_dir, size, ctx). */
typedef void (*fs_iter_cb)(const char* name, int is_dir, uint32_t size, void* ctx);

static void fs_emit_unique(char seen[][64], uint32_t* nseen, const char* comp, int is_dir,
                           uint32_t size, fs_iter_cb cb, void* ctx)
{
    for (uint32_t s = 0; s < *nseen; s++) {
        if (strcmp(seen[s], comp) == 0) {
            return;
        }
    }
    if (*nseen < 16) {
        strcpy(seen[*nseen], comp);
        (*nseen)++;
    }
    cb(comp, is_dir, size, ctx);
}

void fs_list(const char* dir, fs_iter_cb cb, void* ctx)
{
    char prefix[FS_PATH_MAX];
    size_t dl = strlen(dir);
    if (dl + 2 > sizeof(prefix)) {
        return;
    }
    strcpy(prefix, dir);
    size_t pl;
    if (strcmp(dir, "/") == 0) {
        pl = 1;
    } else {
        prefix[dl] = '/';
        prefix[dl + 1] = '\0';
        pl = dl + 1;
    }

    char seen[16][64];
    uint32_t nseen = 0;

    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < (pass == 0 ? ramfs_count : FS_MAX_FILES); i++) {
            const char* name;
            uint32_t size = 0;
            if (pass == 0) {
                /* overlay sombreia ROM: pula se existir no overlay */
                if (fs_find_ov(ramfs_table[i].name)) {
                    continue;
                }
                name = ramfs_table[i].name;
                size = ramfs_table[i].size;
            } else {
                if (!fs_files[i].used) {
                    continue;
                }
                name = fs_files[i].path;
                size = fs_files[i].size;
            }
            size_t j = 0;
            while (j < pl && name[j] == prefix[j]) j++;
            if (j != pl || !name[j]) {
                continue;
            }
            const char* rest = name + j;
            char comp[64];
            size_t k = 0;
            while (rest[k] && rest[k] != '/' && k < sizeof(comp) - 1) {
                comp[k] = rest[k];
                k++;
            }
            comp[k] = '\0';
            fs_emit_unique(seen, &nseen, comp, rest[k] == '/', size, cb, ctx);
        }
    }
    /* diretorios explicitos vazios */
    for (uint32_t i = 0; i < fs_ndirs; i++) {
        size_t j = 0;
        while (j < pl && fs_dirs[i][j] == prefix[j]) j++;
        if (j != pl || !fs_dirs[i][j]) {
            continue;
        }
        const char* rest = fs_dirs[i] + j;
        char comp[64];
        size_t k = 0;
        while (rest[k] && rest[k] != '/' && k < sizeof(comp) - 1) {
            comp[k] = rest[k];
            k++;
        }
        comp[k] = '\0';
        if (!rest[k]) {
            fs_emit_unique(seen, &nseen, comp, 1, 0, cb, ctx);
        }
    }
    /* diretorios do ramfs (inclui vazios vindos do rootfs/) */
    for (uint32_t i = 0; i < ramfs_ndirs; i++) {
        size_t j = 0;
        while (j < pl && ramfs_dirs[i][j] == prefix[j]) j++;
        if (j != pl || !ramfs_dirs[i][j]) {
            continue;
        }
        const char* rest = ramfs_dirs[i] + j;
        char comp[64];
        size_t k = 0;
        while (rest[k] && rest[k] != '/' && k < sizeof(comp) - 1) {
            comp[k] = rest[k];
            k++;
        }
        comp[k] = '\0';
        if (!rest[k]) {
            fs_emit_unique(seen, &nseen, comp, 1, 0, cb, ctx);
        }
    }
}

void fs_stats(uint32_t* nfiles, uint32_t* nbytes)
{
    uint32_t nf = 0, nb = 0;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
        if (fs_files[i].used) {
            nf++;
            nb += fs_files[i].size;
        }
    }
    if (nfiles) *nfiles = nf;
    if (nbytes) *nbytes = nb;
}

void fs_init(void)
{
    console_puts("fs: ROM=");
    console_print_u64(ramfs_count);
    console_puts(" overlay=");
    console_print_u64(FS_MAX_FILES);
    console_puts("x");
    console_print_u64(FS_MAX_SIZE);
    console_puts("B\n");
}
