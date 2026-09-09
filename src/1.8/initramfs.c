/* initramfs.c - initramfs de verdade (CPIO newc via modulo GRUB).
 *
 * O Makefile empacota rootfs/ em build/initrd.cpio (formato newc,
 * o mesmo do Linux). O GRUB entrega como modulo Multiboot1
 * (`module /boot/initrd.cpio`) e este parser cataloga arquivos e
 * diretorios SEM copiar dados (aponta para a memoria do modulo;
 * so os nomes sao copiados, ~4KB). Sem modulo valido, o fs.c usa o
 * ROM embutido (ramfs_data.c) como fallback.
 *
 * Limites honestos: 64 arquivos, 32 diretorios explicitos, nomes
 *ate 127 chars. Dono/permissao/links sao ignorados (tudo e 0444
 * na pratica: escrita sombreia no overlay, como antes).
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, size_t n);
extern char* strcpy(char* dst, const char* src);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void serial_puts(const char* s);
extern void console_puts(const char* s);
extern void console_print_u64(uint64_t v);

#define IRFS_MAX_FILES 64
#define IRFS_MAX_DIRS 32
#define IRFS_NAMESZ 4096

static int ir_active = 0;
static const char* ir_names[IRFS_MAX_FILES];
static const uint8_t* ir_data[IRFS_MAX_FILES];
static uint32_t ir_size[IRFS_MAX_FILES];
static uint32_t ir_n = 0;
static char ir_namebuf[IRFS_NAMESZ];
static uint32_t ir_nameoff = 0;
static char ir_dirs[IRFS_MAX_DIRS][128];
static uint32_t ir_ndirs = 0;

static uint32_t hex8(const uint8_t* p)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint8_t c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return 0;
        }
        v = (v << 4) | d;
    }
    return v;
}

static void ir_add_dir(const char* path)
{
    uint32_t i;
    size_t n;
    if (!path[0] || path[0] != '/') {
        return;
    }
    for (i = 0; i < ir_ndirs; i++) {
        if (strcmp(ir_dirs[i], path) == 0) {
            return;
        }
    }
    if (ir_ndirs >= IRFS_MAX_DIRS) {
        return;
    }
    n = 0;
    while (path[n] && n < sizeof(ir_dirs[0]) - 1) {
        ir_dirs[ir_ndirs][n] = path[n];
        n++;
    }
    ir_dirs[ir_ndirs][n] = '\0';
    ir_ndirs++;
}

/* registra pais implicitos de /a/b/c (=> /a, /a/b) */
static void ir_add_parents(const char* path)
{
    char tmp[128];
    size_t n = 0, i;
    while (path[n] && n < sizeof(tmp) - 1) {
        tmp[n] = path[n];
        n++;
    }
    tmp[n] = '\0';
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            ir_add_dir(tmp);
            tmp[i] = '/';
        }
    }
}

/* "./x" -> "/x", "x" -> "/x", "/x" -> "/x" */
static void ir_normalize(const char* in, char* out, size_t cap)
{
    size_t i = 0, o = 0;
    if (in[0] == '.' && in[1] == '/') {
        i = 2;
    }
    if (in[i] != '/') {
        if (o + 1 < cap) {
            out[o++] = '/';
        }
    }
    while (in[i] && o + 1 < cap) {
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

int initramfs_init(uint32_t magic, uint32_t mbi)
{
    uint32_t flags, nmods, maddr, start, end;
    const uint8_t* p;
    uint32_t off = 0, total;
    if (magic != 0x2BADB002 || mbi == 0) {
        return 0;
    }
    flags = *(volatile uint32_t*)(uintptr_t)mbi;
    if (!(flags & 8u)) {
        return 0; /* sem modulos */
    }
    nmods = *(volatile uint32_t*)(uintptr_t)(mbi + 20);
    maddr = *(volatile uint32_t*)(uintptr_t)(mbi + 24);
    if (nmods == 0 || maddr == 0) {
        return 0;
    }
    start = *(volatile uint32_t*)(uintptr_t)maddr;
    end = *(volatile uint32_t*)(uintptr_t)(maddr + 4);
    if (end <= start || end - start < 128 || end - start > 64 * 1024 * 1024) {
        return 0;
    }
    p = (const uint8_t*)(uintptr_t)start;
    total = end - start;
    if (!(p[0] == '0' && p[1] == '7' && p[2] == '0' &&
          p[3] == '7' && p[4] == '0' && p[5] == '1')) {
        serial_puts("initramfs: modulo 0 nao e CPIO newc (fallback ROM)\n");
        return 0;
    }
    ir_n = 0;
    ir_ndirs = 0;
    ir_nameoff = 0;
    while (off + 110 <= total) {
        const uint8_t* h = p + off;
        uint32_t fsize, nsize, mode;
        char raw[128], norm[128];
        uint32_t i;
        if (!(h[0] == '0' && h[1] == '7' && h[2] == '0' &&
              h[3] == '7' && h[4] == '0' && h[5] == '1')) {
            break;
        }
        fsize = hex8(h + 54);
        nsize = hex8(h + 94);
        mode = hex8(h + 14);
        if (nsize == 0 || nsize >= sizeof(raw) || off + 110 + nsize > total) {
            break;
        }
        for (i = 0; i < nsize && i < sizeof(raw) - 1; i++) {
            raw[i] = (char)h[110 + i];
        }
        raw[i] = '\0';
        /* newc: cabecalho (110) + nome sao alinhados juntos a 4 */
        off += 110 + nsize + ((4 - ((110 + nsize) & 3)) & 3);
        if (strcmp(raw, "TRAILER!!!") == 0) {
            break;
        }
        ir_normalize(raw, norm, sizeof(norm));
        if ((mode & 0170000u) == 0040000u) {
            /* diretorio explicito */
            ir_add_dir(norm);
        } else if ((mode & 0170000u) == 0120000u) {
            /* symlink: ignora (sem VFS de links ainda) */
        } else {
            /* arquivo regular */
            size_t nl;
            if (off + fsize > total) {
                break;
            }
            if (ir_n < IRFS_MAX_FILES) {
                nl = strlen(norm) + 1;
                if (ir_nameoff + nl <= sizeof(ir_namebuf)) {
                    for (i = 0; i < nl; i++) {
                        ir_namebuf[ir_nameoff + i] = norm[i];
                    }
                    ir_names[ir_n] = ir_namebuf + ir_nameoff;
                    ir_nameoff += (uint32_t)nl;
                    ir_data[ir_n] = p + off;
                    ir_size[ir_n] = fsize;
                    ir_n++;
                    ir_add_parents(norm);
                }
            }
        }
        off += (fsize + 3) & ~3u;
    }
    ir_active = 1;
    serial_puts("initramfs: CPIO ok, arquivos=");
    {
        char b[16];
        int n = 0, k = 0;
        uint32_t v = ir_n;
        char t[12];
        if (v == 0) {
            t[k++] = '0';
        }
        while (v > 0 && k < 12) {
            t[k++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (k > 0 && n < 15) {
            b[n++] = t[--k];
        }
        b[n] = '\0';
        serial_puts(b);
    }
    serial_puts("\n");
    console_puts("initramfs: raiz do CD/disco (CPIO newc)\n");
    return 1;
}

int initramfs_active(void)
{
    return ir_active;
}
uint32_t initramfs_count(void)
{
    return ir_n;
}
const char* initramfs_name(uint32_t i)
{
    return i < ir_n ? ir_names[i] : "";
}
const uint8_t* initramfs_data(uint32_t i)
{
    return i < ir_n ? ir_data[i] : 0;
}
uint32_t initramfs_size(uint32_t i)
{
    return i < ir_n ? ir_size[i] : 0;
}
uint32_t initramfs_ndirs(void)
{
    return ir_ndirs;
}
const char* initramfs_dir(uint32_t i)
{
    return i < ir_ndirs ? ir_dirs[i] : "";
}
