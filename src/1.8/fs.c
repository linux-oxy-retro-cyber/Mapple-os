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

/* ROM embutida (fallback, gerada pelo Makefile) */
extern const ramfs_entry_t ramfs_table[];
extern const uint32_t ramfs_count;
extern const char* ramfs_dirs[];
extern const uint32_t ramfs_ndirs;

/* initramfs (CPIO via modulo GRUB; quando ativa, sombreia a embutida) */
extern int initramfs_active(void);
extern uint32_t initramfs_count(void);
extern const char* initramfs_name(uint32_t i);
extern const uint8_t* initramfs_data(uint32_t i);
extern uint32_t initramfs_size(uint32_t i);
extern uint32_t initramfs_ndirs(void);
extern const char* initramfs_dir(uint32_t i);

/* Camada ROM unificada: initramfs se ativa, senao embutida. */
static uint32_t rom_nfiles(void)
{
    return initramfs_active() ? initramfs_count() : ramfs_count;
}

static const char* rom_fname(uint32_t i)
{
    return initramfs_active() ? initramfs_name(i) : ramfs_table[i].name;
}

static const uint8_t* rom_fdata(uint32_t i)
{
    return initramfs_active() ? initramfs_data(i) : ramfs_table[i].data;
}

static uint32_t rom_fsize(uint32_t i)
{
    return initramfs_active() ? initramfs_size(i) : ramfs_table[i].size;
}

static uint32_t rom_ndirs_fn(void)
{
    return initramfs_active() ? initramfs_ndirs() : ramfs_ndirs;
}

static const char* rom_dirname(uint32_t i)
{
    return initramfs_active() ? initramfs_dir(i) : ramfs_dirs[i];
}

uint32_t fs_rom_nfiles(void)
{
    return rom_nfiles();
}

uint32_t fs_rom_nbytes(void)
{
    uint32_t nb = 0;
    for (uint32_t i = 0; i < rom_nfiles(); i++) {
        nb += rom_fsize(i);
    }
    return nb;
}

int fs_rom_is_initramfs(void)
{
    return initramfs_active();
}

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

/* VFS (definido adiante, apos fs_stats) */
typedef struct {
    int used;
    char point[64];
    char fstype[16];
} vfs_mount_t;
static vfs_mount_t* vfs_at(const char* path, const char** rel);
static int vfs_known(const char* fstype, const char* rel);
static int vfs_gen(const char* fstype, const char* rel, uint8_t* buf,
                   size_t cap);
static const char* vfs_nth_name(const char* fstype, uint32_t idx);
static void vfs_init(void);

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
    static ramfs_entry_t hit;
    for (uint32_t i = 0; i < rom_nfiles(); i++) {
        if (strcmp(rom_fname(i), path) == 0) {
            hit.name = rom_fname(i);
            hit.data = rom_fdata(i);
            hit.size = rom_fsize(i);
            return &hit;
        }
    }
    return 0;
}

static int fs_has_children(const char* dir, size_t dl)
{
    for (uint32_t i = 0; i < rom_nfiles(); i++) {
        const char* nm = rom_fname(i);
        if (strncmp(nm, dir, dl) == 0 && nm[dl]) {
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
    const char* rel = 0;
    vfs_mount_t* vm = vfs_at(path, &rel);
    if (vm && rel && !*rel) {
        return 1; /* ponto de montagem */
    }
    if (strcmp(path, "/") == 0) {
        return 1;
    }
    for (i = 0; i < rom_ndirs_fn(); i++) {
        if (strcmp(rom_dirname(i), path) == 0) {
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
    const char* rel = 0;
    vfs_mount_t* vm = vfs_at(path, &rel);
    if (vm) {
        if (!rel || !*rel) {
            return 1;
        }
        return vfs_known(vm->fstype, rel);
    }
    return fs_find_ov(path) != 0 || fs_find_rom(path) != 0 || fs_isdir(path);
}

/* Le arquivo em buf (cap). Retorna tamanho ou -1. */
int fs_read(const char* path, uint8_t* buf, size_t cap)
{
    const char* rel = 0;
    vfs_mount_t* vm = vfs_at(path, &rel);
    if (vm) {
        if (!rel || !*rel) {
            return -1; /* ponto de montagem e diretorio */
        }
        if (rel[0] && !vfs_known(vm->fstype, rel)) {
            /* pode ser arquivo real escondido? montagem sombreia: nao */
            return -1;
        }
        return vfs_gen(vm->fstype, rel, buf, cap);
    }
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
    const char* rel = 0;
    vfs_mount_t* vm = vfs_at(path, &rel);
    (void)data;
    if (vm) {
        /* /dev/null descarta; resto virtual e somente-leitura */
        if (rel && strcmp(vm->fstype, "dev") == 0 && strcmp(rel, "null") == 0) {
            return 0;
        }
        return -1;
    }
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
    const char* rel = 0;
    if (vfs_at(path, &rel)) {
        return -1; /* virtual: nao remove (use umount no ponto) */
    }
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

/* Remove recursivo (rm -rf): apaga arquivos do overlay em `path` e
 * abaixo + entradas de diretorio do overlay. ROM nunca e tocada:
 * retorna -2 se `path` for (ou contiver) arquivo ROM. "/" e recusado.
 * `nfiles`/`ndirs` (podem ser 0) recebem as contagens. 0 ok. */
int fs_rm_recursive(const char* path, int* nfiles, int* ndirs)
{
    char prefix[FS_PATH_MAX];
    size_t pl = strlen(path);
    int nf = 0, nd = 0;
    uint32_t i;
    if (strcmp(path, "/") == 0) {
        return -1;
    }
    {
        const char* rel = 0;
        vfs_mount_t* vm = vfs_at(path, &rel);
        if (vm && rel && !*rel) {
            return -1; /* ponto de montagem: umount primeiro */
        }
    }
    if (pl + 2 > sizeof(prefix)) {
        return -1;
    }
    strcpy(prefix, path);
    prefix[pl] = '/';
    prefix[pl + 1] = '\0';
    pl++;
    /* ROM no caminho? (arquivo exato ou qualquer filho) */
    if (fs_find_rom(path)) {
        return -2;
    }
    for (i = 0; i < rom_nfiles(); i++) {
        if (strncmp(rom_fname(i), prefix, pl) == 0) {
            return -2;
        }
    }
    /* arquivos do overlay: exato + filhos */
    for (i = 0; i < FS_MAX_FILES; i++) {
        if (fs_files[i].used &&
            (strcmp(fs_files[i].path, path) == 0 ||
             strncmp(fs_files[i].path, prefix, pl) == 0)) {
            fs_files[i].used = 0;
            nf++;
        }
    }
    /* diretorios do overlay: exato + filhos */
    {
        uint32_t k = 0;
        while (k < fs_ndirs) {
            if (strcmp(fs_dirs[k], path) == 0 ||
                strncmp(fs_dirs[k], prefix, pl) == 0) {
                fs_ndirs--;
                if (k != fs_ndirs) {
                    strcpy(fs_dirs[k], fs_dirs[fs_ndirs]);
                }
                nd++;
            } else {
                k++;
            }
        }
    }
    if (nf == 0 && nd == 0) {
        return -1; /* nada encontrado */
    }
    if (nfiles) {
        *nfiles = nf;
    }
    if (ndirs) {
        *ndirs = nd;
    }
    return 0;
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
        for (uint32_t i = 0; i < (pass == 0 ? rom_nfiles() : FS_MAX_FILES); i++) {
            const char* name;
            uint32_t size = 0;
            if (pass == 0) {
                /* overlay sombreia ROM: pula se existir no overlay */
                if (fs_find_ov(rom_fname(i))) {
                    continue;
                }
                name = rom_fname(i);
                size = rom_fsize(i);
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
    for (uint32_t i = 0; i < rom_ndirs_fn(); i++) {
        size_t j = 0;
        const char* rd = rom_dirname(i);
        while (j < pl && rd[j] == prefix[j]) j++;
        if (j != pl || !rd[j]) {
            continue;
        }
        const char* rest = rd + j;
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
    /* montagem virtual no proprio dir: soma os dispositivos (com dedupe) */
    {
        const char* rel = 0;
        vfs_mount_t* vm = vfs_at(dir, &rel);
        if (vm && rel && !*rel) {
            uint32_t vi = 0;
            const char* nm;
            while ((nm = vfs_nth_name(vm->fstype, vi)) != 0) {
                fs_emit_unique(seen, &nseen, nm, 0, 0, cb, ctx);
                vi++;
            }
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

/* --- VFS: montagem de dispositivos virtuais em pastas --- */
extern uint64_t pit_uptime_sec(void);
extern uint64_t pit_get_ticks(void);
extern uint64_t sys_mem_kb;
extern const char* hw_cpu_vendor(void);
extern const char* hw_cpu_brand(void);
extern int gui_active;
extern void gui_get_res(uint32_t* w, uint32_t* h);
extern int audio_get_volume(void);
extern int audio_is_muted(void);
extern int bright_get(void);

#define VFS_MAX_MOUNTS 8

static vfs_mount_t vfs_tab[VFS_MAX_MOUNTS];
static uint32_t vfs_rnd = 0x12345678;

/* monta se path == ponto ou esta abaixo (retorna montagem + rel) */
static vfs_mount_t* vfs_at(const char* path, const char** rel)
{
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!vfs_tab[i].used) {
            continue;
        }
        const char* p = vfs_tab[i].point;
        size_t n = strlen(p);
        size_t j = 0;
        while (j < n && path[j] == p[j]) j++;
        if (j != n) {
            continue;
        }
        if (path[j] == '\0') {
            if (rel) {
                *rel = path + j;
            }
            return &vfs_tab[i];
        }
        if (path[j] == '/') {
            if (rel) {
                *rel = path + j + 1;
            }
            return &vfs_tab[i];
        }
    }
    return 0;
}

/* nomes virtuais por tipo (planos, sem subdirs) */
static int vfs_known(const char* fstype, const char* rel)
{
    if (strcmp(fstype, "dev") == 0) {
        return strcmp(rel, "null") == 0 || strcmp(rel, "zero") == 0 ||
               strcmp(rel, "random") == 0;
    }
    if (strcmp(fstype, "proc") == 0) {
        return strcmp(rel, "version") == 0 || strcmp(rel, "uptime") == 0 ||
               strcmp(rel, "mem") == 0 || strcmp(rel, "mounts") == 0 ||
               strcmp(rel, "cpu") == 0;
    }
    if (strcmp(fstype, "sys") == 0) {
        return strcmp(rel, "hostname") == 0 || strcmp(rel, "resolution") == 0 ||
               strcmp(rel, "volume") == 0 || strcmp(rel, "brightness") == 0;
    }
    return 0;
}

/* n-esimo nome virtual (p/ ls com dedupe); 0 se acabou */
static const char* vfs_nth_name(const char* fstype, uint32_t idx)
{
    static const char* dev_n[] = { "null", "zero", "random" };
    static const char* proc_n[] = { "version", "uptime", "mem", "mounts", "cpu" };
    static const char* sys_n[] = { "hostname", "resolution", "volume",
                                   "brightness" };
    if (strcmp(fstype, "dev") == 0) {
        return idx < 3 ? dev_n[idx] : 0;
    }
    if (strcmp(fstype, "proc") == 0) {
        return idx < 5 ? proc_n[idx] : 0;
    }
    if (strcmp(fstype, "sys") == 0) {
        return idx < 4 ? sys_n[idx] : 0;
    }
    return 0;
}

static uint32_t vfs_xorshift(void)
{
    vfs_rnd ^= vfs_rnd << 13;
    vfs_rnd ^= vfs_rnd >> 17;
    vfs_rnd ^= vfs_rnd << 5;
    return vfs_rnd;
}

static size_t vfs_num(char* out, size_t cap, uint64_t v)
{
    char tmp[24];
    int k = 0, n = 0;
    if (v == 0) {
        tmp[k++] = '0';
    }
    while (v > 0 && k < 23) {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (k > 0 && (size_t)n + 1 < cap) {
        out[n++] = tmp[--k];
    }
    out[n] = '\0';
    return (size_t)n;
}

/* gera conteudo virtual em buf; retorna tamanho ou -1 */
static int vfs_gen(const char* fstype, const char* rel, uint8_t* buf,
                   size_t cap)
{
    char tmp[256];
    size_t n = 0;
    if (!vfs_known(fstype, rel)) {
        return -1;
    }
    if (strcmp(fstype, "dev") == 0) {
        if (strcmp(rel, "null") == 0) {
            return 0;
        }
        if (strcmp(rel, "zero") == 0) {
            memset(buf, 0, cap);
            return (int)cap;
        }
        /* random */
        for (size_t i = 0; i < cap; i++) {
            if ((i & 3) == 0) {
                uint32_t r = vfs_xorshift();
                memcpy(tmp, &r, 4);
            }
            buf[i] = (uint8_t)tmp[i & 3];
        }
        return (int)cap;
    }
    if (strcmp(fstype, "proc") == 0) {
        if (strcmp(rel, "version") == 0) {
            const char* s = "mapple 0.13.0 x86_64 (C/C++/asm, ring 0)\n";
            while (*s && n + 1 < cap) {
                buf[n++] = (uint8_t)*s++;
            }
            buf[n] = '\0';
            return (int)n;
        }
        if (strcmp(rel, "uptime") == 0) {
            n = vfs_num(tmp, sizeof(tmp), pit_uptime_sec());
            if (n + 8 < cap) {
                const char* s = "s up\n";
                size_t k = 0;
                while (s[k]) {
                    tmp[n++] = s[k++];
                }
                tmp[n] = '\0';
            }
            for (size_t i = 0; i <= n && i < cap; i++) {
                buf[i] = (uint8_t)tmp[i];
            }
            return (int)n;
        }
        if (strcmp(rel, "mem") == 0) {
            uint32_t nf = 0, nb = 0, i;
            for (i = 0; i < FS_MAX_FILES; i++) {
                if (fs_files[i].used) {
                    nf++;
                    nb += fs_files[i].size;
                }
            }
            n = 0;
            {
                const char* s = "mem_kb=";
                while (*s && n + 1 < sizeof(tmp)) {
                    tmp[n++] = *s++;
                }
            }
            n += vfs_num(tmp + n, sizeof(tmp) - n, sys_mem_kb);
            {
                const char* s = " overlay_files=";
                while (*s && n + 1 < sizeof(tmp)) {
                    tmp[n++] = *s++;
                }
            }
            n += vfs_num(tmp + n, sizeof(tmp) - n, nf);
            {
                const char* s = " overlay_bytes=";
                while (*s && n + 1 < sizeof(tmp)) {
                    tmp[n++] = *s++;
                }
            }
            n += vfs_num(tmp + n, sizeof(tmp) - n, nb);
            if (n + 1 < sizeof(tmp)) {
                tmp[n++] = '\n';
            }
            for (size_t i = 0; i <= n && i < cap; i++) {
                buf[i] = (uint8_t)tmp[i];
            }
            return (int)n;
        }
        if (strcmp(rel, "mounts") == 0) {
            n = 0;
            for (uint32_t i = 0; i < VFS_MAX_MOUNTS && n + 1 < sizeof(tmp); i++) {
                if (!vfs_tab[i].used) {
                    continue;
                }
                {
                    const char* a = vfs_tab[i].fstype;
                    while (*a && n + 1 < sizeof(tmp)) {
                        tmp[n++] = *a++;
                    }
                }
                if (n + 1 < sizeof(tmp)) {
                    tmp[n++] = ' ';
                }
                {
                    const char* a = vfs_tab[i].point;
                    while (*a && n + 1 < sizeof(tmp)) {
                        tmp[n++] = *a++;
                    }
                }
                if (n + 1 < sizeof(tmp)) {
                    tmp[n++] = '\n';
                }
            }
            for (size_t i = 0; i <= n && i < cap; i++) {
                buf[i] = (uint8_t)tmp[i];
            }
            return (int)n;
        }
        /* cpu */
        {
            const char* v = hw_cpu_vendor();
            const char* b = hw_cpu_brand();
            const char* s = "vendor=";
            while (*s && n + 1 < sizeof(tmp)) {
                tmp[n++] = *s++;
            }
            while (*v && n + 1 < sizeof(tmp)) {
                tmp[n++] = *v++;
            }
            s = "\nbrand=";
            while (*s && n + 1 < sizeof(tmp)) {
                tmp[n++] = *s++;
            }
            if (!b[0]) {
                b = "(sem folha estendida)";
            }
            while (*b && n + 1 < sizeof(tmp)) {
                tmp[n++] = *b++;
            }
            if (n + 1 < sizeof(tmp)) {
                tmp[n++] = '\n';
            }
            for (size_t i = 0; i <= n && i < cap; i++) {
                buf[i] = (uint8_t)tmp[i];
            }
            return (int)n;
        }
    }
    /* sys */
    if (strcmp(rel, "hostname") == 0) {
        int r = fs_read("/etc/hostname", buf, cap > 0 ? cap - 1 : 0);
        if (r < 0) {
            const char* s = "mapple\n";
            n = 0;
            while (*s && n + 1 < cap) {
                buf[n++] = (uint8_t)*s++;
            }
            buf[n] = '\0';
            return (int)n;
        }
        if ((size_t)r < cap) {
            buf[r] = '\0';
        }
        return r;
    }
    if (strcmp(rel, "resolution") == 0) {
        if (!gui_active) {
            const char* s = "80x25 texto\n";
            n = 0;
            while (*s && n + 1 < cap) {
                buf[n++] = (uint8_t)*s++;
            }
            buf[n] = '\0';
            return (int)n;
        } else {
            uint32_t w = 0, h = 0;
            gui_get_res(&w, &h);
            n = vfs_num(tmp, sizeof(tmp), w);
            if (n + 1 < sizeof(tmp)) {
                tmp[n++] = 'x';
            }
            n += vfs_num(tmp + n, sizeof(tmp) - n, h);
            {
                const char* s = "x32\n";
                while (*s && n + 1 < sizeof(tmp)) {
                    tmp[n++] = *s++;
                }
            }
            for (size_t i = 0; i <= n && i < cap; i++) {
                buf[i] = (uint8_t)tmp[i];
            }
            return (int)n;
        }
    }
    if (strcmp(rel, "volume") == 0) {
        n = vfs_num(tmp, sizeof(tmp), (uint64_t)audio_get_volume());
        if (audio_is_muted() && n + 8 < sizeof(tmp)) {
            const char* s = " (muted)";
            while (*s) {
                tmp[n++] = *s++;
            }
        }
        if (n + 1 < sizeof(tmp)) {
            tmp[n++] = '\n';
        }
        for (size_t i = 0; i <= n && i < cap; i++) {
            buf[i] = (uint8_t)tmp[i];
        }
        return (int)n;
    }
    /* brightness */
    n = vfs_num(tmp, sizeof(tmp), (uint64_t)bright_get());
    if (n + 1 < sizeof(tmp)) {
        tmp[n++] = '\n';
    }
    for (size_t i = 0; i <= n && i < cap; i++) {
        buf[i] = (uint8_t)tmp[i];
    }
    return (int)n;
}

/* monta tipo virtual em ponto (que deve existir como dir). 0 ok. */
int vfs_mount(const char* fstype, const char* point)
{
    uint32_t i;
    if (strcmp(fstype, "dev") != 0 && strcmp(fstype, "proc") != 0 &&
        strcmp(fstype, "sys") != 0) {
        return -1;
    }
    if (!point || point[0] != '/' || strlen(point) >= sizeof(vfs_tab[0].point)) {
        return -1;
    }
    /* ponto existente? (desmonta temporariamente p/ ver o dir real) */
    {
        char saved_type[16] = { 0 };
        int was = -1;
        for (i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (vfs_tab[i].used && strcmp(vfs_tab[i].point, point) == 0) {
                size_t k = 0;
                while (vfs_tab[i].fstype[k] && k < sizeof(saved_type) - 1) {
                    saved_type[k] = vfs_tab[i].fstype[k];
                    k++;
                }
                vfs_tab[i].used = 0;
                was = (int)i;
                break;
            }
        }
        {
            extern int fs_isdir(const char* path);
            extern int fs_mkdir(const char* path);
            int ok = fs_isdir(point);
            if (was >= 0) {
                vfs_tab[was].used = 1;
                {
                    size_t k = 0;
                    while (saved_type[k] &&
                           k < sizeof(vfs_tab[was].fstype) - 1) {
                        vfs_tab[was].fstype[k] = saved_type[k];
                        k++;
                    }
                    vfs_tab[was].fstype[k] = '\0';
                }
            }
            if (!ok) {
                /* tenta criar no overlay */
                if (fs_mkdir(point) != 0) {
                    return -2;
                }
            }
        }
    }
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_tab[i].used && strcmp(vfs_tab[i].point, point) == 0) {
            size_t k = 0;
            while (fstype[k] && k < sizeof(vfs_tab[i].fstype) - 1) {
                vfs_tab[i].fstype[k] = fstype[k];
                k++;
            }
            vfs_tab[i].fstype[k] = '\0';
            return 0;
        }
    }
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!vfs_tab[i].used) {
            size_t k = 0;
            vfs_tab[i].used = 1;
            while (point[k] && k < sizeof(vfs_tab[i].point) - 1) {
                vfs_tab[i].point[k] = point[k];
                k++;
            }
            vfs_tab[i].point[k] = '\0';
            k = 0;
            while (fstype[k] && k < sizeof(vfs_tab[i].fstype) - 1) {
                vfs_tab[i].fstype[k] = fstype[k];
                k++;
            }
            vfs_tab[i].fstype[k] = '\0';
            return 0;
        }
    }
    return -3; /* tabela cheia */
}

int vfs_umount(const char* point)
{
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_tab[i].used && strcmp(vfs_tab[i].point, point) == 0) {
            vfs_tab[i].used = 0;
            return 0;
        }
    }
    return -1;
}

void vfs_list_mounts(void (*cb)(const char*, const char*, void*), void* ctx)
{
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_tab[i].used) {
            cb(vfs_tab[i].point, vfs_tab[i].fstype, ctx);
        }
    }
}

static void vfs_init(void)
{
    vfs_rnd = (uint32_t)pit_get_ticks() + 0x9E3779B9u;
    if (vfs_rnd == 0) {
        vfs_rnd = 0x12345678;
    }
    vfs_mount("dev", "/dev");
    vfs_mount("proc", "/proc");
    vfs_mount("sys", "/sys");
}

void fs_init(void)
{
    console_puts("fs: ROM=");
    console_print_u64(rom_nfiles());
    console_puts(initramfs_active() ? " (initramfs) overlay=" : " (embed) overlay=");
    console_print_u64(FS_MAX_FILES);
    console_puts("x");
    console_print_u64(FS_MAX_SIZE);
    console_puts("B\n");
    vfs_init();
    console_puts("vfs: /dev /proc /sys montados\n");
}
