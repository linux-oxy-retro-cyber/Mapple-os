/* pci.c - Enumeracao PCI via espaco de configuracao (0xCF8/0xCFC).
 *
 * Varre barramento 0 (suficiente no QEMU) e cataloga dispositivos.
 * Base para o driver de GPU (VMware SVGA) e o gerenciador de discos.
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern void* memset(void* dst, int c, size_t n);
extern void serial_puts(const char* s);
extern void serial_putc(char c);

#define PCI_MAX_DEVS 16

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    uint32_t bar[6];
} pci_dev_t;

static pci_dev_t pci_devs[PCI_MAX_DEVS];
static int pci_count = 0;

static inline void outl(uint16_t p, uint32_t v)
{
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p));
}

static inline uint32_t inl(uint16_t p)
{
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 31) << 11) | ((uint32_t)(func & 7) << 8) |
                    (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 31) << 11) | ((uint32_t)(func & 7) << 8) |
                    (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

/* Habilita I/O + memoria + bus master no COMMAND do dispositivo. */
void pci_enable(int idx)
{
    const pci_dev_t* d;
    uint32_t cmd;
    if (idx < 0 || idx >= pci_count) {
        return;
    }
    d = &pci_devs[idx];
    cmd = pci_cfg_read(d->bus, d->dev, d->func, 0x04);
    cmd |= 0x07u;
    pci_cfg_write(d->bus, d->dev, d->func, 0x04, cmd);
}

static void pci_hex16(uint16_t v)
{
    static const char* h = "0123456789ABCDEF";
    serial_putc(h[(v >> 12) & 0xF]);
    serial_putc(h[(v >> 8) & 0xF]);
    serial_putc(h[(v >> 4) & 0xF]);
    serial_putc(h[v & 0xF]);
}

void pci_scan(void)
{
    pci_count = 0;
    for (uint8_t dev = 0; dev < 32 && pci_count < PCI_MAX_DEVS; dev++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_cfg_read(0, dev, func, 0);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            if (vendor == 0xFFFF || vendor == 0x0000) {
                if (func == 0) {
                    break;
                }
                continue;
            }
            /* funcao 0 sem multifuncao: so func 0 */
            if (func > 0) {
                uint32_t hdr = pci_cfg_read(0, dev, 0, 0x0C);
                if (!(hdr & 0x00800000)) {
                    break;
                }
            }
            pci_dev_t* d = &pci_devs[pci_count++];
            d->bus = 0;
            d->dev = dev;
            d->func = func;
            d->vendor = vendor;
            d->device = (uint16_t)((id >> 16) & 0xFFFF);
            {
                uint32_t cc = pci_cfg_read(0, dev, func, 0x08);
                d->prog_if = (uint8_t)((cc >> 8) & 0xFF);
                d->subclass = (uint8_t)((cc >> 16) & 0xFF);
                d->class_code = (uint8_t)((cc >> 24) & 0xFF);
            }
            for (int b = 0; b < 6; b++) {
                d->bar[b] = pci_cfg_read(0, dev, func, 0x10 + b * 4);
            }
            serial_puts("pci: ");
            pci_hex16(vendor);
            serial_putc(':');
            pci_hex16(d->device);
            serial_puts(" class=");
            pci_hex16(((uint16_t)d->class_code << 8) | d->subclass);
            serial_puts("\n");
        }
    }
}

/* Procura por vendor/device. Retorna indice ou -1. */
int pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < pci_count; i++) {
        if (pci_devs[i].vendor == vendor && pci_devs[i].device == device) {
            return i;
        }
    }
    return -1;
}

const pci_dev_t* pci_get(int idx)
{
    if (idx < 0 || idx >= pci_count) {
        return 0;
    }
    return &pci_devs[idx];
}

int pci_count_all(void)
{
    return pci_count;
}

/* Procura por classe/subclasse PCI. sub == 0xFF ignora subclasse. */
int pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < pci_count; i++) {
        if (pci_devs[i].class_code == class_code &&
            (subclass == 0xFF || pci_devs[i].subclass == subclass)) {
            return i;
        }
    }
    return -1;
}

/* Procura por classe/subclasse/prog-if (USB usa prog-if p/ UHCI...). */
int pci_find_class_pi(uint8_t class_code, uint8_t subclass, uint8_t prog_if)
{
    for (int i = 0; i < pci_count; i++) {
        if (pci_devs[i].class_code == class_code &&
            pci_devs[i].subclass == subclass &&
            pci_devs[i].prog_if == prog_if) {
            return i;
        }
    }
    return -1;
}
