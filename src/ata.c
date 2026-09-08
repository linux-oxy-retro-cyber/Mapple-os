/* ata.c - Driver ATA PIO (somente IDENTIFY, sem FS ainda).
 *
 * Sonda primary/secondary x master/slave e coleta modelo/tamanho.
 * Base do futuro "Gerenciador de discos". QEMU expoe o CD-ROM de boot
 * como ATAPI no secondary master.
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern void* memset(void* dst, int c, size_t n);
extern void serial_puts(const char* s);

#define ATA_PRIMARY     0x1F0
#define ATA_SECONDARY   0x170

#define ATA_REG_DATA        0
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA_LO      3
#define ATA_REG_LBA_MID     4
#define ATA_REG_LBA_HI      5
#define ATA_REG_DRIVE       6
#define ATA_REG_STATUS      7
#define ATA_REG_COMMAND     7

#define ATA_ST_BSY  0x80
#define ATA_ST_DRDY 0x40
#define ATA_ST_DRQ  0x08
#define ATA_ST_ERR  0x01

#define ATA_CMD_IDENTIFY         0xEC
#define ATA_CMD_IDENTIFY_PACKET  0xA1

typedef struct {
    int present;            /* 0 ausente, 1 ATA, 2 ATAPI */
    char model[41];
    uint64_t sectors;       /* setores de 512B (0 se desconhecido) */
} ata_dev_t;

static ata_dev_t ata_devs[4];

static inline void outb(uint16_t p, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p));
}

static inline uint8_t inb(uint16_t p)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

static inline uint16_t inw(uint16_t p)
{
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

/* Espera BSY=0 com timeout. Retorna status ou -1. */
static int ata_wait_bsy(uint16_t base)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(base + ATA_REG_STATUS);
        if (s == 0xFF) {
            return -1; /* barramento flutuante: sem nada */
        }
        if (!(s & ATA_ST_BSY)) {
            return s;
        }
    }
    return -1;
}

/* Espera DRQ=1 (dados prontos). */
static int ata_wait_drq(uint16_t base)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(base + ATA_REG_STATUS);
        if (s == 0xFF) {
            return -1;
        }
        if (!(s & ATA_ST_BSY)) {
            if (s & ATA_ST_ERR) {
                return -1;
            }
            if (s & ATA_ST_DRQ) {
                return s;
            }
        }
    }
    return -1;
}

static void ata_swap_str(char* dst, const uint16_t* src_words, int nwords, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < nwords && n + 2 < cap; i++) {
        dst[n++] = (char)(src_words[i] >> 8);
        dst[n++] = (char)(src_words[i] & 0xFF);
    }
    dst[n] = '\0';
    /* apara espacos do fim */
    while (n > 0 && dst[n - 1] == ' ') {
        dst[--n] = '\0';
    }
}

static void ata_probe_one(int idx, uint16_t base, uint8_t drive)
{
    ata_dev_t* d = &ata_devs[idx];
    d->present = 0;
    d->model[0] = '\0';
    d->sectors = 0;

    /* seleciona drive + atraso 400ns (4 leituras de status) */
    outb(base + ATA_REG_DRIVE, drive);
    for (volatile int i = 0; i < 1000; i++) {}

    uint8_t st = inb(base + ATA_REG_STATUS);
    if (st == 0xFF || st == 0x00) {
        return; /* nada neste drive */
    }

    /* limpa registradores LBA (assinatura) */
    outb(base + ATA_REG_SECCOUNT, 0);
    outb(base + ATA_REG_LBA_LO, 0);
    outb(base + ATA_REG_LBA_MID, 0);
    outb(base + ATA_REG_LBA_HI, 0);

    outb(base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    st = inb(base + ATA_REG_STATUS);
    if (st == 0x00) {
        return; /* sem dispositivo */
    }
    /* espera BSY zerar (ATAPI responde ERR aqui: normal!) */
    if (ata_wait_bsy(base) < 0) {
        return;
    }
    /* assinatura ATAPI? */
    uint8_t mid = inb(base + ATA_REG_LBA_MID);
    uint8_t hi = inb(base + ATA_REG_LBA_HI);
    if (mid == 0x14 && hi == 0xEB) {
        /* ATAPI: usa IDENTIFY PACKET DEVICE */
        outb(base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
        if (ata_wait_drq(base) < 0) {
            return;
        }
        d->present = 2;
    } else {
        if (ata_wait_drq(base) < 0) {
            return;
        }
        d->present = 1;
    }

    uint16_t buf[256];
    for (int i = 0; i < 256; i++) {
        buf[i] = inw(base + ATA_REG_DATA);
    }
    ata_swap_str(d->model, buf + 27, 20, sizeof(d->model));
    /* LBA48 (words 100-103) ou LBA28 (words 60-61) */
    uint64_t lba48 = ((uint64_t)buf[103] << 48) | ((uint64_t)buf[102] << 32) |
                     ((uint64_t)buf[101] << 16) | (uint64_t)buf[100];
    if (lba48 != 0) {
        d->sectors = lba48;
    } else {
        d->sectors = ((uint32_t)buf[61] << 16) | buf[60];
    }
}

void ata_probe(void)
{
    memset(ata_devs, 0, sizeof(ata_devs));
    ata_probe_one(0, ATA_PRIMARY, 0xA0);
    ata_probe_one(1, ATA_PRIMARY, 0xB0);
    ata_probe_one(2, ATA_SECONDARY, 0xA0);
    ata_probe_one(3, ATA_SECONDARY, 0xB0);

    static const char* names[4] = { "ide0m", "ide0s", "ide1m", "ide1s" };
    for (int i = 0; i < 4; i++) {
        serial_puts("ata: ");
        serial_puts(names[i]);
        if (!ata_devs[i].present) {
            serial_puts(" vazio\n");
        } else {
            serial_puts(ata_devs[i].present == 2 ? " ATAPI " : " ATA ");
            serial_puts(ata_devs[i].model);
            serial_puts("\n");
        }
    }
}

const ata_dev_t* ata_get(int idx)
{
    if (idx < 0 || idx > 3) {
        return 0;
    }
    return &ata_devs[idx];
}
