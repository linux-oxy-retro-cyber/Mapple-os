/* drv.c - Registro de drivers do mapple.
 *
 * Todo driver real se registra aqui (nome + init + status). `drivers`
 * lista tudo. Adicionar driver novo = 1 entrada + funcao init.
 * Drivers fisicos extras (USB, HDA, SATA AHCI...) exigem suas pilhas;
 * a infraestrutura (PCI scan, IRQ, DMA por polling) ja existe.
 */
#include <stdint.h>
#include <stddef.h>

extern void console_putc(char c);
extern void console_puts(const char* s);

/* inits (wrappers adaptam assinaturas void) */
extern void vga_init(void);
extern void vga_clear_screen(void);
extern void keyboard_init(void);
extern void pit_init(void);
extern void fs_init(void);
extern void ata_probe(void);
extern void pci_scan(void);
extern int svga_init(void);
extern int e1000_init(void);
extern void mouse_init(void);

static int w_vga(void)
{
    vga_init();
    vga_clear_screen();
    return 0;
}

static int w_keyboard(void)
{
    keyboard_init();
    return 0;
}

static int w_pit(void)
{
    pit_init();
    return 0;
}

static int w_fs(void)
{
    fs_init();
    return 0;
}

static int w_ata(void)
{
    ata_probe();
    return 0;
}

static int w_pci(void)
{
    pci_scan();
    return 0;
}

static int w_mouse(void)
{
    mouse_init();
    return 0;
}

static int w_null(void)
{
    return 0;
}

typedef struct {
    const char* name;
    const char* desc;
    int (*init)(void);
    int status; /* -1 nao iniciado, 0 ok, 1 falha/aviso */
} driver_t;

static driver_t drivers[] = {
    { "serial", "COM1 38400 8N1 (debug)", w_null, -1 },
    { "vga", "texto 80x25 + cursor", w_vga, -1 },
    { "pic", "8259 remapeado 0x20/0x28", w_null, -1 },
    { "pit", "timer 100Hz + TSC", w_pit, -1 },
    { "rtc", "CMOS data/hora", w_null, -1 },
    { "keyboard", "PS/2 set1 US + foco", w_keyboard, -1 },
    { "mouse", "PS/2 IRQ12 + anti-fantasma", w_mouse, -1 },
    { "ata", "IDE identify PIO", w_ata, -1 },
    { "pci", "scan bus 0 + BARs", w_pci, -1 },
    { "svga", "VMware SVGA II + VRAM", svga_init, -1 },
    { "e1000", "Intel 82540EM polling", e1000_init, -1 },
    { "net", "ARP/IPv4/ICMP sobre e1000", w_null, -1 },
    { "ramfs", "ROM embutida + overlay RW", w_fs, -1 },
    { "initramfs", "CPIO newc via modulo GRUB", w_null, -1 },
    { "vfs", "mounts /dev /proc /sys", w_null, -1 },
    { "installer", "install em HD (FAT16+GRUB)", w_null, -1 },
    { "fbcon", "framebuffer + compositor", w_null, -1 },
    { "pcspk", "speaker via PIT", w_null, -1 },
    { "font", "bitmap 8x8 proprio", w_null, -1 },
    { "cc", "subconjunto C (nucleo)", w_null, -1 },
    { "gui-wm", "janelas + apps", w_null, -1 },
    { "cpu", "CPUID vendor/modelo/flags", w_null, -1 },
    { "fpu", "x87 via CR0", w_null, -1 },
    { "apic", "local APIC (MSR 1B)", w_null, -1 },
    { "tsc", "timestamp counter", w_null, -1 },
    { "crypto", "RDRAND/AES-NI", w_null, -1 },
    { "memprot", "SMEP/SMAP", w_null, -1 },
    { "acpi", "RSDP/RSDT/XSDT", w_null, -1 },
    { "hpet", "tabela HPET", w_null, -1 },
    { "hypervisor", "folha 40000000h", w_null, -1 },
    { "vmware-bd", "backdoor 5658h", w_null, -1 },
    { "fw_cfg", "QEMU fw_cfg 510h", w_null, -1 },
    { "debugcon", "Bochs 0xE9", w_null, -1 },
    { "smbios", "ancora _SM_", w_null, -1 },
    { "com1", "UART 3F8 loopback", w_null, -1 },
    { "lpt", "paralela 378h", w_null, -1 },
    { "floppy", "CMOS tipo drive", w_null, -1 },
    { "cmos-mem", "RAM estendida CMOS", w_null, -1 },
    { "atapi", "CD-ROM identify packet", w_null, -1 },
    { "ide-ctrl", "PCI 0101", w_null, -1 },
    { "ahci", "PCI 0106 SATA", w_null, -1 },
    { "usb-uhci", "PCI 0C00", w_null, -1 },
    { "usb-ohci", "PCI 0C10", w_null, -1 },
    { "usb-ehci", "PCI 0C20", w_null, -1 },
    { "usb-xhci", "PCI 0C30", w_null, -1 },
    { "usb-core", "pilha USB (polling, sem DMA)", w_null, -1 },
    { "usb-msc", "mass storage (stub /mnt/usb)", w_null, -1 },
    { "vga-pci", "PCI classe 03", w_null, -1 },
    { "isa-bridge", "PCI 0601", w_null, -1 },
    { "virtio-blk", "1AF4:1001/1042", w_null, -1 },
    { "nic2", "rtl8139/pcnet/virtio-net", w_null, -1 },
    { "audio", "AC97/HDA PCI", w_null, -1 },
    { "mixer", "volume 0-100 + mute (pcspk)", w_null, -1 },
    { "backlight", "brilho 10-100 (framebuffer)", w_null, -1 },
    { "foon", "raycaster estilo Doom (Q10)", w_null, -1 },
    { "arcade", "pong + breakout + tetris", w_null, -1 },
    { "gpu3d", "torus + terreno + tunel", w_null, -1 },
    { "dma", "8237", w_null, -1 },
    { "a20", "linha A20", w_null, -1 },
};
#define NDRIVERS (sizeof(drivers) / sizeof(drivers[0]))

/* Marca ausente (hardware opcional nao detectado). */
void drv_absent(const char* name)
{
    size_t i;
    for (i = 0; i < NDRIVERS; i++) {
        const char* a = drivers[i].name;
        const char* b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            drivers[i].status = 2;
            return;
        }
    }
}

/* Marca status de driver sem init proprio (chamado pelo subsistema). */
void drv_ready(const char* name, int ok)
{
    size_t i;
    for (i = 0; i < NDRIVERS; i++) {
        const char* a = drivers[i].name;
        const char* b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            drivers[i].status = ok ? 0 : 1;
            return;
        }
    }
}

void drv_list(void)
{
    size_t i;
    int ok = 0;
    for (i = 0; i < NDRIVERS; i++) {
        console_puts("  [");
        console_puts(drivers[i].status == 0 ? "OK " : (drivers[i].status == 2 ? " -  " : (drivers[i].status < 0 ? "-- " : "FAIL")));
        console_puts("] ");
        console_puts(drivers[i].name);
        console_puts(" - ");
        console_puts(drivers[i].desc);
        console_putc('\n');
        if (drivers[i].status == 0) {
            ok++;
        }
    }
    console_puts("drivers ok: ");
    {
        extern void console_print_u64(uint64_t v);
        console_print_u64((uint64_t)ok);
        console_putc('/');
        console_print_u64((uint64_t)NDRIVERS);
        console_putc('\n');
    }
}

int drv_count(void)
{
    return (int)NDRIVERS;
}

int drv_ok_count(void)
{
    size_t i;
    int ok = 0;
    for (i = 0; i < NDRIVERS; i++) {
        if (drivers[i].status == 0) {
            ok++;
        }
    }
    return ok;
}
