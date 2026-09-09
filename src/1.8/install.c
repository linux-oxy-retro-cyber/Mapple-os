/* install.c - Instalador do mapple em HD (roda dentro do OS).
 *
 * Instala o sistema num disco ATA vazio (APAGA TUDO!): MBR com o
 * boot.img do GRUB + tabela, core.img do GRUB no LBA 1.., FAT16
 * formatada em @2048 com /boot/kernel.elf + initrd.cpio + /boot/grub
 * completo (mods + tema + fonte + cfg). Layout identico ao `make hd`
 * (ver tools/mkhd.py). Tudo vem do payload embutido no build
 * (kernel.stage1 + initrd + core + boot + grubfiles.cpio).
 *
 * FAT16 escrita do zero: BPB + FATs + root fixo + subdirs com espaco
 * pre-alocado + arquivos contiguos + entradas 8.3 e LFN (nomes longos
 * como initrd.cpio e all_video.mod exigem). Alvos >= 64MB, LBA28.
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, size_t n);
extern char* strcpy(char* dst, const char* src);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void* memset(void* dst, int c, size_t n);

extern void console_putc(char c);
extern void console_puts(const char* s);
extern void console_print_u64(uint64_t v);
extern char keyboard_getchar(void);
extern void drv_ready(const char* name, int ok);

/* ATA (ata.c) */
typedef struct {
    int present;
    char model[41];
    uint64_t sectors;
} ata_dev_t;
extern const ata_dev_t* ata_get(int idx);
extern int ata_read_lba28(int idx, uint32_t lba, uint8_t* buf, uint32_t nsec);
extern int ata_write_lba28(int idx, uint32_t lba, const uint8_t* buf,
                           uint32_t nsec);

/* payload (payload_data.c, gerado) */
extern const uint8_t payload_kernel[];
extern const uint32_t payload_kernel_len;
extern const uint8_t payload_initrd[];
extern const uint32_t payload_initrd_len;
extern const uint8_t payload_core[];
extern const uint32_t payload_core_len;
extern const uint8_t payload_boot[];
extern const uint32_t payload_boot_len;
extern const uint8_t payload_grub[];
extern const uint32_t payload_grub_len;

#define PART_LBA 2048u

static int g_disk = -1;
static uint32_t g_part_sec = 0;
static uint32_t g_spc = 8;
static uint32_t g_resv = 8;
static uint32_t g_fat_sec = 0;
static uint32_t g_root_sec = 32; /* 512 entradas */
static uint32_t g_data_lba = 0;
static uint32_t g_next_clust = 2;
static uint8_t g_secbuf[512];

static void iput_u64(uint64_t v)
{
    console_print_u64(v);
}

/* entrada de linha com eco (para perguntas) */
static void iline(char* buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            console_putc('\n');
            buf[n] = '\0';
            return;
        } else if (c == '\b') {
            if (n > 0) {
                n--;
                console_putc('\b');
            }
        } else if (c >= 0x20 && c <= 0x7E) {
            if (n + 1 < cap) {
                buf[n++] = c;
                console_putc(c);
            }
        }
    }
}

/* escrita em blocos de <=128 setores */
static int dwrite(uint32_t lba, const uint8_t* buf, uint32_t nsec)
{
    while (nsec > 0) {
        uint32_t k = nsec > 128 ? 128 : nsec;
        if (ata_write_lba28(g_disk, lba, buf, k) != 0) {
            return -1;
        }
        lba += k;
        buf += k * 512;
        nsec -= k;
    }
    return 0;
}

static int dread(uint32_t lba, uint8_t* buf, uint32_t nsec)
{
    while (nsec > 0) {
        uint32_t k = nsec > 128 ? 128 : nsec;
        if (ata_read_lba28(g_disk, lba, buf, k) != 0) {
            return -1;
        }
        lba += k;
        buf += k * 512;
        nsec -= k;
    }
    return 0;
}

/* --- geometria FAT16 --- */
static void fat_geom(uint32_t part_sec)
{
    uint32_t spc = 8, clusters, fs;
    g_part_sec = part_sec;
    for (;;) {
        clusters = (part_sec - g_resv - g_root_sec) / spc;
        fs = ((clusters + 2) * 2 + 511) / 512;
        clusters = (part_sec - g_resv - 2 * fs - g_root_sec) / spc;
        fs = ((clusters + 2) * 2 + 511) / 512;
        if (clusters <= 65525 || spc >= 64) {
            break;
        }
        spc *= 2;
    }
    g_spc = spc;
    g_fat_sec = fs;
    g_data_lba = PART_LBA + g_resv + 2 * fs + g_root_sec;
    g_next_clust = 2;
}

static uint32_t clus_lba(uint32_t cl)
{
    return g_data_lba + (cl - 2) * g_spc;
}

/* entrada FAT para cluster (alocacao estritamente sequencial) */
static uint32_t fat_entry(uint32_t cl)
{
    if (cl == 0) {
        return 0xFFF8;
    }
    if (cl == 1) {
        return 0xFFFF;
    }
    if (cl + 1 < g_next_clust) {
        return cl + 1;
    }
    if (cl < g_next_clust) {
        return 0xFFFF;
    }
    return 0;
}

/* grava as 2 copias da FAT (gerada proceduralmente) */
static int fat_write_tables(void)
{
    uint8_t sec[512];
    for (uint32_t f = 0; f < 2; f++) {
        uint32_t base = PART_LBA + g_resv + f * g_fat_sec;
        for (uint32_t s = 0; s < g_fat_sec; s++) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t v = fat_entry(s * 256 + i);
                sec[i * 2] = (uint8_t)(v & 0xFF);
                sec[i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
            }
            if (dwrite(base + s, sec, 1) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* --- setor de boot FAT16 --- */
static int fat_write_boot(void)
{
    uint8_t* b = g_secbuf;
    uint32_t total32 = g_part_sec;
    int i;
    memset(b, 0, 512);
    b[0] = 0xEB;
    b[1] = 0x3C;
    b[2] = 0x90;
    {
        const char* oem = "MAPPLE  ";
        for (i = 0; i < 8; i++) {
            b[3 + i] = (uint8_t)oem[i];
        }
    }
    b[11] = 0x00;
    b[12] = 0x02;
    b[13] = (uint8_t)g_spc;
    b[14] = (uint8_t)(g_resv & 0xFF);
    b[15] = (uint8_t)((g_resv >> 8) & 0xFF);
    b[16] = 2;
    b[17] = 0x00;
    b[18] = 0x02;
    b[19] = 0x00;
    b[20] = 0x00;
    b[21] = 0xF8;
    b[22] = (uint8_t)(g_fat_sec & 0xFF);
    b[23] = (uint8_t)((g_fat_sec >> 8) & 0xFF);
    b[24] = 32;
    b[25] = 0;
    b[26] = 64;
    b[27] = 0;
    b[28] = (uint8_t)(PART_LBA & 0xFF);
    b[29] = (uint8_t)((PART_LBA >> 8) & 0xFF);
    b[30] = (uint8_t)((PART_LBA >> 16) & 0xFF);
    b[31] = (uint8_t)((PART_LBA >> 24) & 0xFF);
    b[32] = (uint8_t)(total32 & 0xFF);
    b[33] = (uint8_t)((total32 >> 8) & 0xFF);
    b[34] = (uint8_t)((total32 >> 16) & 0xFF);
    b[35] = (uint8_t)((total32 >> 24) & 0xFF);
    b[36] = 0x80;
    b[38] = 0x29;
    b[39] = 0x12;
    b[40] = 0x34;
    b[41] = 0x56;
    b[42] = 0x78;
    {
        const char* lb = "MAPPLE     ";
        for (i = 0; i < 11; i++) {
            b[43 + i] = (uint8_t)lb[i];
        }
        const char* ty = "FAT16   ";
        for (i = 0; i < 8; i++) {
            b[54 + i] = (uint8_t)ty[i];
        }
    }
    b[510] = 0x55;
    b[511] = 0xAA;
    return dwrite(PART_LBA, b, 1);
}

/* --- nomes 8.3 + LFN --- */
static void alias83(const char* lname, char out[11], int k)
{
    char base[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    char ext[4] = { 0, 0, 0, 0 };
    size_t i = 0, bi = 0, ei = 0;
    const char* dot = 0;
    for (i = 0; lname[i]; i++) {
        if (lname[i] == '.') {
            dot = lname + i;
        }
    }
    if (dot) {
        for (i = 0; lname + i < dot && bi < 8; i++) {
            char c = lname[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-') {
                base[bi++] = c;
            }
        }
        dot++;
        for (i = 0; dot[i] && ei < 3; i++) {
            char c = dot[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                ext[ei++] = c;
            }
        }
    } else {
        for (i = 0; lname[i] && bi < 8; i++) {
            char c = lname[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-') {
                base[bi++] = c;
            }
        }
    }
    for (i = 0; i < 8; i++) {
        out[i] = ' ';
    }
    for (i = 0; i < 3; i++) {
        out[8 + i] = ' ';
    }
    if (k > 0) {
        size_t n = bi > 6 ? 6 : bi;
        for (i = 0; i < n; i++) {
            out[i] = base[i];
        }
        out[n] = '~';
        out[n + 1] = (char)('0' + (k % 10));
        for (i = 0; i < ei; i++) {
            out[8 + i] = ext[i];
        }
    } else {
        for (i = 0; i < bi && i < 8; i++) {
            out[i] = base[i];
        }
        for (i = 0; i < ei; i++) {
            out[8 + i] = ext[i];
        }
    }
}

static uint8_t lfn_sum(const char a[11])
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) {
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + (uint8_t)a[i]);
    }
    return s;
}

/* entradas LFN em dst (32*N). Nome ASCII. Retorna N ou -1. */
static int lfn_write(const char* lname, const char alias[11], uint8_t* dst)
{
    uint16_t u[256];
    int ulen = 0, n, e, k;
    while (lname[ulen] && ulen < 255) {
        u[ulen] = (uint16_t)(uint8_t)lname[ulen];
        ulen++;
    }
    n = (ulen + 1 + 12) / 13;
    if (n > 4) {
        return -1;
    }
    for (e = n; e >= 1; e--) {
        uint8_t* en = dst + (n - e) * 32;
        int base = (e - 1) * 13;
        en[0] = (uint8_t)(e == n ? (0x40 | e) : e);
        for (k = 0; k < 13; k++) {
            uint16_t c;
            int idx = base + k;
            if (idx < ulen) {
                c = u[idx];
            } else if (idx == ulen) {
                c = 0x0000;
            } else {
                c = 0xFFFF;
            }
            if (k < 5) {
                en[1 + k * 2] = (uint8_t)(c & 0xFF);
                en[2 + k * 2] = (uint8_t)((c >> 8) & 0xFF);
            } else if (k < 11) {
                en[14 + (k - 5) * 2] = (uint8_t)(c & 0xFF);
                en[15 + (k - 5) * 2] = (uint8_t)((c >> 8) & 0xFF);
            } else {
                en[28 + (k - 11) * 2] = (uint8_t)(c & 0xFF);
                en[29 + (k - 11) * 2] = (uint8_t)((c >> 8) & 0xFF);
            }
        }
        en[11] = 0x0F;
        en[12] = 0x00;
        en[13] = lfn_sum(alias);
        en[26] = 0;
        en[27] = 0;
    }
    return n;
}

static void ent83(uint8_t* e, const char alias[11], int is_dir,
                  uint32_t clust, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, alias, 11);
    e[11] = is_dir ? 0x10 : 0x20;
    e[26] = (uint8_t)(clust & 0xFF);
    e[27] = (uint8_t)((clust >> 8) & 0xFF);
    e[28] = (uint8_t)(size & 0xFF);
    e[29] = (uint8_t)((size >> 8) & 0xFF);
    e[30] = (uint8_t)((size >> 16) & 0xFF);
    e[31] = (uint8_t)((size >> 24) & 0xFF);
}

/* --- tabela de diretorios (criados; root = idx especial) --- */
#define INST_MAX_DIRS 24
#define DIR_PRE_CLUST 8 /* clusters pre-alocados por subdir */
typedef struct {
    char path[96];      /* "boot/grub/..." ("" = root) */
    uint32_t start;     /* cluster inicial (0 = root fixo) */
    uint32_t nent;      /* entradas usadas */
    uint32_t cap;       /* capacidade em entradas */
    int is_root;
} inst_dir_t;
static inst_dir_t itabs[INST_MAX_DIRS];
static int itab_n = 0;

static inst_dir_t* itab_find(const char* dirpath)
{
    for (int i = 0; i < itab_n; i++) {
        if (strcmp(itabs[i].path, dirpath) == 0) {
            return &itabs[i];
        }
    }
    return 0;
}

/* root: regiao fixa; append com buffer de setor */
static uint8_t root_secbuf[512];
static uint32_t root_used = 0;

static int root_append(const uint8_t* ent32)
{
    uint32_t base = PART_LBA + g_resv + 2 * g_fat_sec;
    uint32_t idx = root_used % 16;
    if (root_used >= 512) {
        return -1;
    }
    if (root_used == 0 || idx == 0) {
        memset(root_secbuf, 0, 512);
        if (root_used > 0) {
            /* continua: reler nao preciso (sequencial, buffer zerado ok
             * pois so escrevemos para frente) */
        }
    }
    /* ATENCAO: root e write-once sequencial; releitura desnecessaria */
    memcpy(root_secbuf + idx * 32, ent32, 32);
    if (idx == 15) {
        if (dwrite(base + root_used / 16, root_secbuf, 1) != 0) {
            return -1;
        }
    }
    root_used++;
    return 0;
}

static int root_flush(void)
{
    uint32_t base = PART_LBA + g_resv + 2 * g_fat_sec;
    uint32_t idx = root_used % 16;
    if (idx != 0) {
        if (dwrite(base + root_used / 16, root_secbuf, 1) != 0) {
            return -1;
        }
    }
    memset(root_secbuf, 0, 512);
    {
        uint32_t start = root_used / 16 + (idx != 0 ? 1 : 0);
        for (uint32_t s = start; s < g_root_sec; s++) {
            if (dwrite(base + s, root_secbuf, 1) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int root_find(const char alias[11], uint32_t* cl)
{
    uint32_t base = PART_LBA + g_resv + 2 * g_fat_sec;
    uint32_t nsec = (root_used + 15) / 16;
    /* le o que ja foi descarregado + buffer atual */
    for (uint32_t s = 0; s < nsec; s++) {
        const uint8_t* sp;
        uint8_t tmp[512];
        if (s == root_used / 16 && (root_used % 16) != 0) {
            sp = root_secbuf;
        } else if (s < root_used / 16) {
            if (dread(base + s, tmp, 1) != 0) {
                return 0;
            }
            sp = tmp;
        } else {
            continue;
        }
        for (int e = 0; e < 16; e++) {
            const uint8_t* en = sp + e * 32;
            if (en[0] == 0x00 || en[0] == 0xE5 || en[11] == 0x0F) {
                continue;
            }
            int eq = 1;
            for (int k = 0; k < 11; k++) {
                if (en[k] != (uint8_t)alias[k]) {
                    eq = 0;
                    break;
                }
            }
            if (eq) {
                if (cl) {
                    *cl = (uint32_t)en[26] | ((uint32_t)en[27] << 8);
                }
                return 1;
            }
        }
    }
    return 0;
}

/* subdir: append (capacidade pre-alocada, cadeia contigua) */
static int sub_append(inst_dir_t* d, const uint8_t* ent32)
{
    uint32_t secidx = d->nent / 16;
    uint32_t pos = d->nent % 16;
    uint8_t sec[512];
    uint32_t cl = d->start + secidx / g_spc;
    uint32_t lba;
    if (d->nent >= d->cap) {
        return -1;
    }
    /* setor = inicio do cluster + offset dentro */
    lba = clus_lba(cl) + (secidx - (cl - d->start) * g_spc);
    {
        if (pos == 0 && d->nent > 0) {
            memset(sec, 0, 512);
        } else {
            if (dread(lba, sec, 1) != 0) {
                memset(sec, 0, 512);
            }
        }
        memcpy(sec + pos * 32, ent32, 32);
        if (dwrite(lba, sec, 1) != 0) {
            return -1;
        }
    }
    d->nent++;
    return 0;
}

static int sub_find(inst_dir_t* d, const char alias[11], uint32_t* cl,
                    int* isdir)
{
    uint8_t sec[512];
    uint32_t total = d->nent;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t secidx = i / 16;
        uint32_t pos = i % 16;
        uint32_t cl2 = d->start + secidx / g_spc;
        uint32_t lba = clus_lba(cl2) + (secidx % g_spc);
        if (pos == 0) {
            if (dread(lba, sec, 1) != 0) {
                return 0;
            }
        }
        {
            const uint8_t* e = sec + pos * 32;
            if (e[0] == 0x00 || e[0] == 0xE5 || e[11] == 0x0F) {
                continue;
            }
            int eq = 1;
            for (int k = 0; k < 11; k++) {
                if (e[k] != (uint8_t)alias[k]) {
                    eq = 0;
                    break;
                }
            }
            if (eq) {
                if (cl) {
                    *cl = (uint32_t)e[26] | ((uint32_t)e[27] << 8);
                }
                if (isdir) {
                    *isdir = (e[11] & 0x10) != 0;
                }
                return 1;
            }
        }
    }
    return 0;
}

/* acrescenta 1 arquivo (dados + entradas) ao dir; alias unico via ~k */
static int put_file(inst_dir_t* d, const char* lname, const uint8_t* data,
                    uint32_t size, int is_root)
{
    char alias[11];
    uint8_t lfn[128];
    uint8_t e[32];
    uint32_t need, cl, k;
    int nlfn = 0, t;
    /* alias unico */
    for (t = 0; t < 10; t++) {
        uint32_t dummy = 0;
        alias83(lname, alias, t);
        if (is_root) {
            if (!root_find(alias, &dummy)) {
                break;
            }
        } else {
            if (!sub_find(d, alias, &dummy, 0)) {
                break;
            }
        }
    }
    nlfn = lfn_write(lname, alias, lfn);
    if (nlfn < 0) {
        return -1;
    }
    need = (size + g_spc * 512 - 1) / (g_spc * 512);
    if (need == 0) {
        need = 1;
    }
    cl = g_next_clust;
    g_next_clust += need;
    /* dados (em blocos; ultimo parcial via buffer) */
    {
        uint32_t full = size / 512;
        uint32_t rest = size % 512;
        uint32_t lba = clus_lba(cl);
        uint32_t done = 0;
        uint8_t sec[512];
        while (done < full) {
            uint32_t k = full - done > 128 ? 128 : full - done;
            if (dwrite(lba + done, data + done * 512, k) != 0) {
                return -1;
            }
            done += k;
            console_putc('.');
        }
        if (rest) {
            memset(sec, 0, 512);
            memcpy(sec, data + done * 512, rest);
            if (dwrite(lba + done, sec, 1) != 0) {
                return -1;
            }
        }
    }
    /* entradas: LFN + 8.3 */
    ent83(e, alias, 0, cl, size);
    if (is_root) {
        for (k = 0; k < (uint32_t)nlfn; k++) {
            if (root_append(lfn + k * 32) != 0) {
                return -1;
            }
        }
        return root_append(e);
    }
    for (k = 0; k < (uint32_t)nlfn; k++) {
        if (sub_append(d, lfn + k * 32) != 0) {
            return -1;
        }
    }
    return sub_append(d, e);
}

/* cria subdir (pre-aloca clusters, escreve '.'/'..', registra no pai) */
static inst_dir_t* make_dir(inst_dir_t* parent, const char* lname,
                            int parent_is_root)
{
    char alias[11];
    uint8_t lfn[128];
    uint8_t e[32];
    uint32_t cl;
    int nlfn, t;
    inst_dir_t* nd;
    if (itab_n >= INST_MAX_DIRS) {
        return 0;
    }
    for (t = 0; t < 10; t++) {
        uint32_t dummy = 0;
        alias83(lname, alias, t);
        if (parent_is_root) {
            if (!root_find(alias, &dummy)) {
                break;
            }
        } else {
            if (!sub_find(parent, alias, &dummy, 0)) {
                break;
            }
        }
    }
    nlfn = lfn_write(lname, alias, lfn);
    if (nlfn < 0) {
        return 0;
    }
    cl = g_next_clust;
    g_next_clust += DIR_PRE_CLUST;
    nd = &itabs[itab_n++];
    {
        size_t k = 0;
        while (lname[k] && k < sizeof(nd->path) - 1) {
            /* path completo e montado pelo chamador */
            k++;
        }
    }
    nd->start = cl;
    nd->nent = 0;
    nd->cap = DIR_PRE_CLUST * g_spc * 16;
    nd->is_root = 0;
    /* '.' e '..' */
    {
        uint8_t dot[32], dotdot[32];
        char a1[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                        ' ', ' ', ' ' };
        char a2[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ',
                        ' ', ' ', ' ' };
        uint32_t par = parent_is_root ? 0 : parent->start;
        ent83(dot, a1, 1, cl, 0);
        ent83(dotdot, a2, 1, par, 0);
        if (sub_append(nd, dot) != 0) {
            return 0;
        }
        if (sub_append(nd, dotdot) != 0) {
            return 0;
        }
    }
    /* registra no pai */
    ent83(e, alias, 1, cl, 0);
    if (parent_is_root) {
        uint32_t k;
        for (k = 0; k < (uint32_t)nlfn; k++) {
            if (root_append(lfn + k * 32) != 0) {
                return 0;
            }
        }
        if (root_append(e) != 0) {
            return 0;
        }
    } else {
        uint32_t k;
        for (k = 0; k < (uint32_t)nlfn; k++) {
            if (sub_append(parent, lfn + k * 32) != 0) {
                return 0;
            }
        }
        if (sub_append(parent, e) != 0) {
            return 0;
        }
    }
    return nd;
}

/* --- CPIO: iterador newc --- */
static uint32_t cp_hex8(const uint8_t* p)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
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

static int cpio_each(const uint8_t* p, uint32_t total,
                     int (*cb)(const char*, const uint8_t*, uint32_t, int,
                               void*),
                     void* ctx)
{
    uint32_t off = 0;
    char name[128];
    while (off + 110 <= total) {
        const uint8_t* h = p + off;
        uint32_t fsize, nsize, mode;
        uint32_t i;
        if (!(h[0] == '0' && h[1] == '7' && h[2] == '0' && h[3] == '7' &&
              h[4] == '0' && h[5] == '1')) {
            break;
        }
        fsize = cp_hex8(h + 54);
        nsize = cp_hex8(h + 94);
        mode = cp_hex8(h + 14);
        if (nsize == 0 || nsize >= sizeof(name) ||
            off + 110 + nsize > total) {
            break;
        }
        for (i = 0; i < nsize && i < sizeof(name) - 1; i++) {
            name[i] = (char)h[110 + i];
        }
        name[i] = '\0';
        off += 110 + nsize + ((4 - ((110 + nsize) & 3)) & 3);
        if (strcmp(name, "TRAILER!!!") == 0) {
            break;
        }
        {
            const char* nn = name;
            if (nn[0] == '.' && nn[1] == '/') {
                nn += 2;
            }
            if (off + fsize > total) {
                break;
            }
            if (cb(nn, p + off, fsize,
                   ((mode & 0170000u) == 0040000u) ? 1 : 0, ctx) != 0) {
                return -1;
            }
        }
        off += (fsize + 3) & ~3u;
    }
    return 0;
}

/* divide "a/b/c" em dir="a/b" + base="c" */
static void split_path(const char* p, char* dir, char* base)
{
    const char* slash = 0;
    size_t k = 0;
    for (const char* q = p; *q; q++) {
        if (*q == '/') {
            slash = q;
        }
    }
    if (!slash) {
        dir[0] = '\0';
        while (*p && k < 63) {
            base[k++] = *p++;
        }
        base[k] = '\0';
        return;
    }
    while (p < slash && k < 95) {
        dir[k++] = *p++;
    }
    dir[k] = '\0';
    p = slash + 1;
    k = 0;
    while (*p && k < 63) {
        base[k++] = *p++;
    }
    base[k] = '\0';
}

typedef struct {
    int files;
    int errors;
} inst_ctx_t;

static int grub_pass1(const char* path, const uint8_t* data, uint32_t size,
                      int is_dir, void* ctx)
{
    inst_ctx_t* c = (inst_ctx_t*)ctx;
    char dir[96], base[64];
    (void)data;
    (void)size;
    if (!is_dir) {
        return 0;
    }
    split_path(path, dir, base);
    /* acha pai na tabela ("" = root) */
    {
        inst_dir_t* par = itab_find(dir);
        int pis_root = (dir[0] == '\0');
        inst_dir_t* nd;
        if (!pis_root && !par) {
            c->errors++;
            return 0;
        }
        nd = make_dir(pis_root ? 0 : par, base, pis_root);
        if (!nd) {
            c->errors++;
            return 0;
        }
        {
            /* path completo */
            size_t k = 0;
            const char* s = path;
            while (*s && k < sizeof(nd->path) - 1) {
                nd->path[k++] = *s++;
            }
            nd->path[k] = '\0';
        }
        console_putc('+');
    }
    return 0;
}

static int grub_pass2(const char* path, const uint8_t* data, uint32_t size,
                      int is_dir, void* ctx)
{
    inst_ctx_t* c = (inst_ctx_t*)ctx;
    char dir[96], base[64];
    if (is_dir) {
        return 0;
    }
    split_path(path, dir, base);
    {
        inst_dir_t* par = itab_find(dir);
        int pis_root = (dir[0] == '\0');
        if (!pis_root && !par) {
            c->errors++;
            return 0;
        }
        if (put_file(pis_root ? 0 : par, base, data, size, pis_root) != 0) {
            c->errors++;
            console_puts("\nfalha em ");
            console_puts(path);
            console_putc('\n');
            return 0;
        }
        c->files++;
        if ((c->files & 15) == 0) {
            console_putc('#');
        }
    }
    return 0;
}

/* grava MBR (boot.img + tabela) e core */
static int write_boot_core(void)
{
    uint8_t mbr[512];
    uint32_t ncore = (payload_core_len + 511) / 512;
    uint32_t nsec = (g_part_sec + PART_LBA);
    (void)nsec;
    if (payload_boot_len < 512) {
        return -1;
    }
    memcpy(mbr, payload_boot, 440);
    memset(mbr + 440, 0, 6);
    /* entrada 0: bootavel FAT16 @2048 */
    mbr[446] = 0x80;
    mbr[447] = 0x20;
    mbr[448] = 0x21;
    mbr[449] = 0x00;
    mbr[450] = 0x06;
    mbr[451] = 0xFE;
    mbr[452] = 0xFF;
    mbr[453] = 0xFF;
    mbr[454] = (uint8_t)(PART_LBA & 0xFF);
    mbr[455] = (uint8_t)((PART_LBA >> 8) & 0xFF);
    mbr[456] = (uint8_t)((PART_LBA >> 16) & 0xFF);
    mbr[457] = (uint8_t)((PART_LBA >> 24) & 0xFF);
    mbr[458] = (uint8_t)(g_part_sec & 0xFF);
    mbr[459] = (uint8_t)((g_part_sec >> 8) & 0xFF);
    mbr[460] = (uint8_t)((g_part_sec >> 16) & 0xFF);
    mbr[461] = (uint8_t)((g_part_sec >> 24) & 0xFF);
    memset(mbr + 462, 0, 48);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (dwrite(0, mbr, 1) != 0) {
        return -1;
    }
    /* core contiguo do LBA 1 */
    {
        uint32_t total = (payload_core_len + 511) / 512;
        uint32_t done = 0;
        static uint8_t sec[512];
        while (done < total) {
            uint32_t off = done * 512;
            uint32_t full = payload_core_len > off
                                ? (payload_core_len - off) / 512
                                : 0;
            if (full > 128) {
                full = 128;
            }
            if (full > 0) {
                if (dwrite(1 + done, payload_core + off, full) != 0) {
                    return -1;
                }
                done += full;
            } else {
                uint32_t cp = payload_core_len > off ? payload_core_len - off
                                                     : 0;
                memset(sec, 0, 512);
                if (cp) {
                    memcpy(sec, payload_core + off, cp);
                }
                if (dwrite(1 + done, sec, 1) != 0) {
                    return -1;
                }
                done++;
            }
            console_putc('.');
        }
    }
    if (ncore + 1 > PART_LBA) {
        return -1;
    }
    return 0;
}

/* --- fluxo principal --- */
void install_run(void)
{
    static const char* names[4] = { "hd0 (ide0m)", "hd1 (ide0s)",
                                    "hd2 (ide1m)", "hd3 (ide1s)" };
    char line[32];
    int i, n = 0;
    console_puts("\x1B[96m== instalador mapple 0.13.0 ==\x1B[0m\n");
    console_puts("discos ATA detectados:\n");
    for (i = 0; i < 4; i++) {
        const ata_dev_t* d = ata_get(i);
        if (!d || !d->present || d->present == 2) {
            continue;
        }
        console_puts("  [");
        iput_u64((uint64_t)i);
        console_puts("] ");
        console_puts(names[i]);
        console_puts(" ");
        console_puts(d->model);
        console_puts(" ");
        iput_u64(d->sectors / 2048);
        console_puts(" MB\n");
        n++;
    }
    if (n == 0) {
        console_puts("nenhum HD (use -hda no QEMU).\n");
        return;
    }
    console_puts("alvo [0-3]? ");
    iline(line, sizeof(line));
    g_disk = (line[0] >= '0' && line[0] <= '3' && !line[1])
                 ? (int)(line[0] - '0')
                 : -1;
    {
        const ata_dev_t* d =
            (g_disk >= 0) ? ata_get(g_disk) : (const ata_dev_t*)0;
        if (!d || !d->present || d->present == 2) {
            console_puts("alvo invalido.\n");
            return;
        }
        if (d->sectors < PART_LBA + 131072) {
            console_puts("disco pequeno (<64MB uteis).\n");
            return;
        }
        console_puts("APAGA TUDO em ");
        console_puts(d->model);
        console_puts("! digite SIM: ");
        iline(line, sizeof(line));
        if (!((line[0] == 'S' || line[0] == 's') &&
              (line[1] == 'I' || line[1] == 'i') &&
              (line[2] == 'M' || line[2] == 'm') && !line[3])) {
            console_puts("cancelado.\n");
            return;
        }
        console_puts("formatando FAT16...\n");
        fat_geom((uint32_t)(d->sectors - PART_LBA));
        console_puts("cluster ");
        iput_u64((uint64_t)g_spc * 512);
        console_puts("B part ");
        iput_u64((uint64_t)g_part_sec / 2048);
        console_puts("MB\n");
        /* zera registradores de root */
        root_used = 0;
        itab_n = 0;
        /* root na tabela */
        itabs[0].path[0] = '\0';
        itabs[0].start = 0;
        itabs[0].nent = 0;
        itabs[0].cap = 512;
        itabs[0].is_root = 1;
        itab_n = 1;
        if (fat_write_boot() != 0) {
            console_puts("falha no boot sector.\n");
            return;
        }
        /* root zerada */
        {
            uint8_t z[512];
            memset(z, 0, 512);
            for (uint32_t s = 0; s < g_root_sec; s++) {
                if (dwrite(PART_LBA + g_resv + 2 * g_fat_sec + s, z, 1) !=
                    0) {
                    console_puts("falha no root.\n");
                    return;
                }
            }
        }
        console_puts("gravando boot+core...\n");
        if (write_boot_core() != 0) {
            console_puts("falha no boot/core.\n");
            return;
        }
        console_putc('\n');
        console_puts("gravando /boot (kernel+initrd)...\n");
        {
            /* /boot */
            inst_dir_t* boot;
            boot = make_dir(0, "boot", 1);
            if (!boot) {
                console_puts("falha mkdir /boot.\n");
                return;
            }
            {
                size_t k = 0;
                const char* s = "boot";
                while (*s) {
                    boot->path[k++] = *s++;
                }
                boot->path[k] = '\0';
            }
            if (put_file(boot, "kernel.elf", payload_kernel,
                         payload_kernel_len, 0) != 0) {
                console_puts("falha kernel.\n");
                return;
            }
            console_putc('\n');
            if (put_file(boot, "initrd.cpio", payload_initrd,
                         payload_initrd_len, 0) != 0) {
                console_puts("falha initrd.\n");
                return;
            }
            console_putc('\n');
        }
        console_puts("gravando /boot/grub (mods+tema)...\n");
        {
            inst_ctx_t cx;
            cx.files = 0;
            cx.errors = 0;
            if (cpio_each(payload_grub, payload_grub_len, grub_pass1, &cx) !=
                0) {
                console_puts("falha cpio pass1.\n");
                return;
            }
            console_putc('\n');
            if (cpio_each(payload_grub, payload_grub_len, grub_pass2, &cx) !=
                0) {
                console_puts("falha cpio pass2.\n");
                return;
            }
            console_putc('\n');
            console_puts("arquivos grub: ");
            iput_u64((uint64_t)cx.files);
            console_puts(" erros: ");
            iput_u64((uint64_t)cx.errors);
            console_putc('\n');
            if (cx.errors) {
                console_puts("instalacao incompleta.\n");
                return;
            }
        }
        if (root_flush() != 0) {
            console_puts("falha root flush.\n");
            return;
        }
        console_puts("gravando FATs...\n");
        if (fat_write_tables() != 0) {
            console_puts("falha FAT.\n");
            return;
        }
        console_puts("verificando...\n");
        {
            uint8_t v[512];
            if (dread(0, v, 1) != 0 || v[510] != 0x55 || v[511] != 0xAA) {
                console_puts("falha verify MBR.\n");
                return;
            }
            if (dread(1, v, 1) != 0 || v[0] != payload_core[0] ||
                v[1] != payload_core[1]) {
                console_puts("falha verify core.\n");
                return;
            }
            if (dread(PART_LBA, v, 1) != 0 || v[510] != 0x55 ||
                v[511] != 0xAA) {
                console_puts("falha verify boot.\n");
                return;
            }
        }
        drv_ready("installer", 1);
        console_puts("\x1B[92mOK! reinicie pelo HD (qemu -hda).\x1B[0m\n");
    }
}
