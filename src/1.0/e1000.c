/* e1000.c - Driver Intel 82540EM (QEMU default, PCI 8086:100E).
 *
 * TX/RX por polling (sem IRQ): aneis de descritores legacy, buffers
 * proprios. Usado por net.c (ARP/IPv4/ICMP). QEMU user-net responde
 * ARP e ping no gateway 10.0.2.2.
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern void* memset(void* dst, int c, size_t n);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void serial_puts(const char* s);
extern void serial_putc(char c);
extern int pci_find(uint16_t vendor, uint16_t device);
extern void pci_enable(int idx);

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass;
    uint32_t bar[6];
} pci_dev_t;
extern const pci_dev_t* pci_get(int idx);

/* Registradores (offset do MMIO) */
#define R_CTRL   0x0000
#define R_STATUS 0x0008
#define R_RCTL   0x0100
#define R_TCTL   0x0400
#define R_TIPG   0x0410
#define R_RDBAL  0x2800
#define R_RDBAH  0x2804
#define R_RDLEN  0x2808
#define R_RDH    0x2810
#define R_RDT    0x2818
#define R_TDBAL  0x3800
#define R_TDBAH  0x3804
#define R_TDLEN  0x3808
#define R_TDH    0x3810
#define R_TDT    0x3818
#define R_RAL0   0x5400
#define R_RAH0   0x5404
#define R_MTA    0x5200

/* Bits */
#define ST_LU      0x02
#define RCTL_EN    0x00000002u
#define RCTL_BAM   0x00008000u
#define RCTL_BSIZE_2048 0x00000000u
#define TCTL_EN    0x00000002u
#define TCTL_PSP   0x00000008u
#define TX_CMD_EOP  0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS   0x08
#define TX_STA_DD   0x01
#define RX_STA_DD   0x01

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

#define TX_N 8
#define RX_N 32
#define RX_BUF 2048

static volatile uint32_t* mmio = 0;
static tx_desc_t tx_ring[TX_N] __attribute__((aligned(16)));
static rx_desc_t rx_ring[RX_N] __attribute__((aligned(16)));
static uint8_t rx_bufs[RX_N][RX_BUF] __attribute__((aligned(16)));
static int tx_cur = 0;
static int rx_cur = 0;
static uint8_t mac[6];
static int e1000_ok = 0;

static inline uint32_t rr(uint32_t off)
{
    return mmio[off / 4];
}

static inline void wr(uint32_t off, uint32_t v)
{
    mmio[off / 4] = v;
}

static void hexb(uint8_t v)
{
    static const char* h = "0123456789ABCDEF";
    serial_putc(h[(v >> 4) & 0xF]);
    serial_putc(h[v & 0xF]);
}

int e1000_init(void)
{
    int idx = pci_find(0x8086, 0x100E);
    const pci_dev_t* d;
    uint32_t membase;
    uint32_t ral, rah;
    int i;
    if (idx < 0) {
        serial_puts("e1000: ausente\n");
        return -1;
    }
    d = pci_get(idx);
    pci_enable(idx);
    membase = d->bar[0] & ~0xFu;
    if ((d->bar[0] & 1) || membase == 0) {
        serial_puts("e1000: BAR invalida\n");
        return -1;
    }
    mmio = (volatile uint32_t*)(uintptr_t)membase;

    /* link up? */
    for (i = 0; i < 100000; i++) {
        if (rr(R_STATUS) & ST_LU) {
            break;
        }
    }
    if (!(rr(R_STATUS) & ST_LU)) {
        serial_puts("e1000: sem link\n");
        return -1;
    }

    /* MAC */
    ral = rr(R_RAL0);
    rah = rr(R_RAH0);
    mac[0] = (uint8_t)(ral & 0xFF);
    mac[1] = (uint8_t)((ral >> 8) & 0xFF);
    mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    mac[4] = (uint8_t)(rah & 0xFF);
    mac[5] = (uint8_t)((rah >> 8) & 0xFF);

    /* limpa MTA (sem multicast extra) */
    for (i = 0; i < 128; i++) {
        wr(R_MTA + i * 4, 0);
    }

    /* RX ring */
    memset(rx_ring, 0, sizeof(rx_ring));
    for (i = 0; i < RX_N; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    wr(R_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    wr(R_RDBAH, 0);
    wr(R_RDLEN, RX_N * 16);
    wr(R_RDH, 0);
    wr(R_RDT, RX_N - 1);
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048);

    /* TX ring */
    memset(tx_ring, 0, sizeof(tx_ring));
    wr(R_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    wr(R_TDBAH, 0);
    wr(R_TDLEN, TX_N * 16);
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    wr(R_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    wr(R_TIPG, 10 | (10u << 10) | (10u << 20));

    e1000_ok = 1;
    serial_puts("e1000: link ok MAC=");
    for (i = 0; i < 6; i++) {
        hexb(mac[i]);
        if (i < 5) {
            serial_putc(':');
        }
    }
    serial_puts("\n");
    return 0;
}

int e1000_present(void)
{
    return e1000_ok;
}

const uint8_t* e1000_mac(void)
{
    return mac;
}

/* Envia frame (ate 1514B). 0 ok, -1 falha. */
int e1000_send(const uint8_t* data, uint16_t len)
{
    tx_desc_t* d;
    int to;
    if (!e1000_ok || !data || len == 0 || len > 1514) {
        return -1;
    }
    d = &tx_ring[tx_cur];
    /* espera descritor livre */
    for (to = 0; to < 100000; to++) {
        if (d->status & TX_STA_DD) {
            break;
        }
        /* primeiro uso: status 0 + DD? inicializa como livre */
        if (to == 0 && !(d->status & TX_STA_DD) && rr(R_TDH) == (uint32_t)tx_cur) {
            break;
        }
    }
    {
        static uint8_t tx_buf[1600];
        if (len > sizeof(tx_buf)) {
            return -1;
        }
        memcpy(tx_buf, data, len);
        d->addr = (uint64_t)(uintptr_t)tx_buf;
        d->length = len;
        d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
        d->status = 0;
        d->cso = 0;
        d->css = 0;
        d->special = 0;
    }
    tx_cur = (tx_cur + 1) % TX_N;
    wr(R_TDT, (uint32_t)tx_cur);
    /* espera transmitir */
    for (to = 0; to < 100000; to++) {
        if (d->status & TX_STA_DD) {
            return 0;
        }
    }
    return -1;
}

/* Recebe frame (polling). Retorna tamanho ou 0 (nada), -1 erro. */
int e1000_recv(uint8_t* out, uint16_t cap, uint16_t* out_len)
{
    rx_desc_t* d = &rx_ring[rx_cur];
    if (!e1000_ok) {
        return -1;
    }
    if (!(d->status & RX_STA_DD)) {
        return 0;
    }
    if (d->errors) {
        d->status = 0;
        wr(R_RDT, (uint32_t)rx_cur);
        rx_cur = (rx_cur + 1) % RX_N;
        return 0;
    }
    {
        uint16_t n = d->length;
        if (n > cap) {
            n = cap;
        }
        /* remove CRC (4B) se presente */
        if (n >= 4 && !(rr(R_RCTL) & 0x04000000u)) {
            /* RCTL_SECRC nao ligado: hardware mantem CRC; tira */
            if (n > 60) {
                n -= 4;
            }
        }
        memcpy(out, rx_bufs[rx_cur], n);
        if (out_len) {
            *out_len = n;
        }
    }
    d->status = 0;
    wr(R_RDT, (uint32_t)rx_cur);
    rx_cur = (rx_cur + 1) % RX_N;
    return 1;
}
