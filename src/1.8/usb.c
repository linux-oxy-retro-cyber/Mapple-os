/* usb.c - Driver USB do mapple (host + pendrive).
 *
 * Escopo honesto:
 *  - Detecta controladoras UHCI/OHCI/EHCI/XHCI via PCI (classe 0C03 +
 *    prog-if 00/10/20/30) usando pci_find_class_pi().
 *  - Liga bus-master (pci_enable), le BARs (IO/MMIO) e sonda as PORTAS
 *    de cada controladora (conectado? velocidade?) por polling, sem IRQ
 *    e sem DMA.
 *  - Expoe tabela de portas/dispositivos p/ `usb`, app Discos e `drivers`.
 *  - Mass Storage (BOT/SCSI): stub honesto. Sem DMA por enquanto nao da
 *    p/ fazer transferencias de controle/bulk reais; se houver dispositivo
 *    conectado, `usbmount` cria /mnt/usb no overlay e marca montado (ponto
 *    de montagem p/ a futura pilha BOT). Sem dispositivo, recusa.
 *
 * Registros lidos (somente leitura, exceto port-reset com timeout):
 *  UHCI: base IO = BAR4&~3; portas em +0x10/+0x12 (16 bits cada):
 *        bit0 conectado, bit1 mudanca, bit2 enable.
 *  EHCI: MMIO BAR0; CAPLENGTH=[+0], HCSPARAMS=[+4] (nports = bits0-3);
 *        opbase = mmio+caplen; PORTSC[n] = op+0x44+n*4:
 *        bit0 conectado, bits10-11 linestatus.
 *  XHCI: MMIO BAR0; CAPLENGTH=[+0], HCSPARAMS1=[+4]
 *        (MaxPorts = bits 31:24! bits 7:0 sao MaxSlots);
 *        opbase = mmio+caplen; PORTSC[n] = op+0x400+n*0x10:
 *        bit0 conectado, bits10-13 speed ID (1 full,2 low,3 high,4+ super).
 *  OHCI: so presenca/BAR por enquanto (portas via HcRhPortStatus exigem
 *        init completo; reporta nports desconhecido, honesto).
 */
#include <stdint.h>
#include <stddef.h>

extern void serial_puts(const char* s);
extern void serial_putc(char c);
extern void console_putc(char c);
extern void console_puts(const char* s);
extern void console_print_u64(uint64_t v);
extern int pci_find_class_pi(uint8_t cc, uint8_t sc, uint8_t pi);
extern void pci_enable(int idx);
extern void drv_ready(const char* name, int ok);
extern void drv_absent(const char* name);
extern int fs_mkdir(const char* path);
extern int fs_isdir(const char* path);

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    uint32_t bar[6];
} pci_dev_usb_t;
extern const pci_dev_usb_t* pci_get(int idx);

/* --- IO helpers --- */
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
static inline uint32_t inl(uint16_t p)
{
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}
static inline void outw(uint16_t p, uint16_t v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p));
}

/* MMIO: SEMPRE acesso de 32 bits! Leituras de 1 byte em BAR MMIO
 * falham neste QEMU (retornam 0); o e1000.c faz o mesmo (inl/outl). */
static uint32_t mmio_rd32(uint32_t phys, uint32_t off)
{
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)(phys + off);
    return *p;
}

/* --- tabela publica --- */
#define USB_MAX_PORTS 16
typedef struct {
    char hc[8];      /* "uhci","ohci","ehci","xhci" */
    int port;        /* indice da porta na controladora */
    int connected;   /* 0/1 */
    char speed[8];   /* "low","full","high","super","?" */
    uint16_t vendor_hint; /* 0 = desconhecido (sem GET_DESCRIPTOR ainda) */
} usb_port_t;

static usb_port_t usb_ports[USB_MAX_PORTS];
static int usb_nports = 0;
static int usb_ndev = 0;
static int usb_mounted = 0;
static int usb_hcs = 0; /* nº de controladoras encontradas */

static void slog_hex16(uint16_t v)
{
    static const char* h = "0123456789ABCDEF";
    serial_putc(h[(v >> 12) & 0xF]);
    serial_putc(h[(v >> 8) & 0xF]);
    serial_putc(h[(v >> 4) & 0xF]);
    serial_putc(h[v & 0xF]);
}

static void add_port(const char* hc, int port, int conn, const char* spd)
{
    usb_port_t* p;
    int i;
    if (usb_nports >= USB_MAX_PORTS) {
        return;
    }
    p = &usb_ports[usb_nports++];
    i = 0;
    while (hc[i] && i < 7) {
        p->hc[i] = hc[i];
        i++;
    }
    p->hc[i] = '\0';
    p->port = port;
    p->connected = conn ? 1 : 0;
    i = 0;
    while (spd[i] && i < 7) {
        p->speed[i] = spd[i];
        i++;
    }
    p->speed[i] = '\0';
    p->vendor_hint = 0;
    if (conn) {
        usb_ndev++;
    }
}

/* --- sondas por tipo --- */
static void probe_uhci(int idx)
{
    const pci_dev_usb_t* d = pci_get(idx);
    uint32_t io;
    int p;
    if (!d) {
        return;
    }
    pci_enable(idx);
    io = d->bar[4] & ~3u;
    serial_puts("usb: uhci ");
    slog_hex16(d->vendor);
    serial_putc(':');
    slog_hex16(d->device);
    serial_puts(" iobase=");
    {
        char b[12];
        int n = 0;
        uint32_t v = io;
        char t[12];
        int k = 0;
        if (v == 0) {
            t[k++] = '0';
        }
        while (v > 0 && k < 12) {
            uint32_t d = v % 16;
            t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            v /= 16;
        }
        (void)b;
        while (k > 0 && n < 11) {
            b[n++] = t[--k];
        }
        b[n] = '\0';
        serial_puts("0x");
        serial_puts(b);
    }
    serial_puts("\n");
    if ((d->bar[4] & 1) == 0 || io == 0) {
        serial_puts("usb: uhci sem BAR4 IO valida\n");
        return;
    }
    /* UHCI tem 2 portas por padrao */
    for (p = 0; p < 2; p++) {
        uint16_t st = inw((uint16_t)(io + 0x10 + (uint32_t)p * 2));
        int conn = (st & 0x01) != 0;
        serial_puts(conn ? "usb: uhci porta conectada\n" : "usb: uhci porta vazia\n");
        add_port("uhci", p, conn, conn ? "full" : "?");
    }
    usb_hcs++;
}

static void probe_ohci(int idx)
{
    const pci_dev_usb_t* d = pci_get(idx);
    if (!d) {
        return;
    }
    pci_enable(idx);
    serial_puts("usb: ohci ");
    slog_hex16(d->vendor);
    serial_putc(':');
    slog_hex16(d->device);
    serial_puts(" (portas: init completo pendente)\n");
    /* sem tocar no MMIO antes do init: registra 1 porta desconhecida */
    add_port("ohci", 0, 0, "?");
    usb_hcs++;
}

static void probe_ehci(int idx)
{
    const pci_dev_usb_t* d = pci_get(idx);
    uint32_t phys;
    uint32_t caplen;
    uint32_t hcs;
    int nports, p;
    if (!d) {
        return;
    }
    pci_enable(idx);
    phys = d->bar[0] & ~0xFu;
    serial_puts("usb: ehci ");
    slog_hex16(d->vendor);
    serial_putc(':');
    slog_hex16(d->device);
    serial_puts("\n");
    if ((d->bar[0] & 1) || phys == 0) {
        serial_puts("usb: ehci sem BAR0 MMIO valida\n");
        add_port("ehci", 0, 0, "?");
        usb_hcs++;
        return;
    }
    caplen = mmio_rd32(phys, 0) & 0xFF;       /* CAPLENGTH */
    hcs = mmio_rd32(phys, 4);                 /* HCSPARAMS */
    nports = (int)(hcs & 0xF);                /* EHCI: N_PORTS = bits 3:0 */
    if (caplen < 0x08 || caplen > 0x40 || nports <= 0 || nports > 15) {
        serial_puts("usb: ehci regs suspeitos, assume 2 portas\n");
        nports = 2;
    }
    for (p = 0; p < nports; p++) {
        /* PORTSC[n] = opbase + 0x44 + n*4 (dword!) */
        uint32_t v = mmio_rd32(phys, caplen + 0x44 + (uint32_t)p * 4);
        int conn = (v & 0x01) != 0;
        /* EHCI conectado = high-speed (low/full ficam no companion) */
        serial_puts(conn ? "usb: ehci porta conectada (high)\n" : "usb: ehci porta vazia\n");
        add_port("ehci", p, conn, conn ? "high" : "?");
    }
    usb_hcs++;
}

static void probe_xhci(int idx)
{
    const pci_dev_usb_t* d = pci_get(idx);
    uint32_t phys;
    uint32_t caplen;
    uint32_t hcs1;
    int nports, p;
    if (!d) {
        return;
    }
    pci_enable(idx);
    phys = d->bar[0] & ~0xFu; /* BAR0 de 64 bits: low ja basta (<4GB) */
    serial_puts("usb: xhci ");
    slog_hex16(d->vendor);
    serial_putc(':');
    slog_hex16(d->device);
    serial_puts("\n");
    if ((d->bar[0] & 1) || phys == 0) {
        serial_puts("usb: xhci sem BAR0 MMIO valida\n");
        add_port("xhci", 0, 0, "?");
        usb_hcs++;
        return;
    }
    caplen = mmio_rd32(phys, 0) & 0xFF; /* CAPLENGTH (QEMU: 0x40) */
    hcs1 = mmio_rd32(phys, 4);          /* HCSPARAMS1 */
    /* xHCI: MaxPorts = bits 31:24 (bits 7:0 = MaxSlots, nao portas!) */
    nports = (int)((hcs1 >> 24) & 0xFF);
    if (caplen < 0x20 || caplen > 0x80 || nports <= 0 || nports > 16) {
        serial_puts("usb: xhci regs suspeitos, assume 2 portas\n");
        nports = 2;
    }
    if (nports > USB_MAX_PORTS - usb_nports) {
        nports = USB_MAX_PORTS - usb_nports;
    }
    for (p = 0; p < nports; p++) {
        /* PORTSC[n] = opbase + 0x400 + n*0x10 (dword!) */
        uint32_t v = mmio_rd32(phys, caplen + 0x400 + (uint32_t)p * 0x10);
        int conn = (v & 0x01) != 0; /* CCS */
        uint32_t spd = (v >> 10) & 0xF; /* Port Speed */
        const char* s = "?";
        if (conn) {
            if (spd == 2) {
                s = "low";
            } else if (spd == 1) {
                s = "full";
            } else if (spd == 3) {
                s = "high";
            } else if (spd >= 4 && spd <= 7) {
                s = "super";
            } else {
                s = "high";
            }
            serial_puts("usb: xhci porta conectada (");
            serial_puts(s);
            serial_puts(")\n");
        } else {
            serial_puts("usb: xhci porta vazia\n");
        }
        add_port("xhci", p, conn, conn ? s : "?");
    }
    usb_hcs++;
}

/* --- API publica --- */
void usb_init(void)
{
    int i;
    usb_nports = 0;
    usb_ndev = 0;
    usb_hcs = 0;
    usb_mounted = 0;
    /* varre ate 4 de cada tipo (QEMU tipico: 1-2) */
    for (i = 0; i < 4; i++) {
        /* pci_find_class_pi retorna o primeiro; para multiplos, faz
         * scan manual: tenta indices via re-scan simples chamando com
         * offsets diferentes nao disponivel -> chama uma vez por tipo.
         * Como pci.c nao tem iterador, 1 por tipo e o honesto aqui. */
        (void)i;
        break;
    }
    {
        int u = pci_find_class_pi(0x0C, 0x03, 0x00);
        int o = pci_find_class_pi(0x0C, 0x03, 0x10);
        int e = pci_find_class_pi(0x0C, 0x03, 0x20);
        int x = pci_find_class_pi(0x0C, 0x03, 0x30);
        if (u >= 0) {
            probe_uhci(u);
            drv_ready("usb-uhci", 1);
        } else {
            drv_absent("usb-uhci");
        }
        if (o >= 0) {
            probe_ohci(o);
            drv_ready("usb-ohci", 1);
        } else {
            drv_absent("usb-ohci");
        }
        if (e >= 0) {
            probe_ehci(e);
            drv_ready("usb-ehci", 1);
        } else {
            drv_absent("usb-ehci");
        }
        if (x >= 0) {
            probe_xhci(x);
            drv_ready("usb-xhci", 1);
        } else {
            drv_absent("usb-xhci");
        }
    }
    if (usb_hcs > 0) {
        drv_ready("usb-core", 1);
        serial_puts("usb: stack pronta (polling, sem DMA)\n");
    } else {
        drv_absent("usb-core");
        serial_puts("usb: nenhuma controladora (rode QEMU com -device usb-ehci/-xhci)\n");
    }
    if (usb_ndev > 0) {
        drv_ready("usb-msc", 1);
        serial_puts("usb: dispositivo(s) presente(s); BOT completo pendente\n");
    } else {
        drv_absent("usb-msc");
    }
}

int usb_port_count(void)
{
    return usb_nports;
}
int usb_dev_count(void)
{
    return usb_ndev;
}
int usb_mounted_now(void)
{
    return usb_mounted;
}

void usb_list(void)
{
    int i;
    if (usb_hcs == 0) {
        console_puts("usb: nenhuma controladora detectada.\n");
        console_puts("dica: qemu -device qemu-xhci -device usb-storage,drive=d\n");
        return;
    }
    console_puts("controladoras: ");
    console_print_u64((uint64_t)usb_hcs);
    console_puts("  portas: ");
    console_print_u64((uint64_t)usb_nports);
    console_puts("  dispositivos: ");
    console_print_u64((uint64_t)usb_ndev);
    console_putc('\n');
    for (i = 0; i < usb_nports; i++) {
        console_puts("  ");
        console_puts(usb_ports[i].hc);
        console_puts(" p");
        console_print_u64((uint64_t)usb_ports[i].port);
        console_puts(usb_ports[i].connected ? " [CONECTADO " : " [vazia ");
        console_puts(usb_ports[i].speed);
        console_puts("]\n");
    }
    if (usb_ndev > 0) {
        console_puts("MSC: BOT/SCSI pendente (leitura real no roadmap).\n");
        console_puts(usb_mounted ? "montado em /mnt/usb (stub).\n" : "use 'usbmount' p/ criar /mnt/usb.\n");
    }
}

/* Monta ponto stub /mnt/usb no overlay. 0 ok, -1 sem dispositivo. */
int usb_mount(void)
{
    if (usb_ndev == 0) {
        console_puts("usbmount: nenhum pendrive conectado.\n");
        return -1;
    }
    if (!fs_isdir("/mnt")) {
        fs_mkdir("/mnt");
    }
    if (!fs_isdir("/mnt/usb")) {
        fs_mkdir("/mnt/usb");
    }
    usb_mounted = 1;
    drv_ready("usb-msc", 1);
    console_puts("usbmount: /mnt/usb pronto (stub; I/O real pendente).\n");
    serial_puts("usb: /mnt/usb montado (stub)\n");
    return 0;
}
