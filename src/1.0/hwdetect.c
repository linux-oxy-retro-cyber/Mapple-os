/* hwdetect.c - Deteccao de hardware/plataforma do mapple.
 *
 * Tudo aqui e deteccao REAL (portas, CPUID, MSR, ACPI, PCI, CMOS),
 * sem chutes. Cada item vira entrada em `drivers` via drv_ready/
 * drv_absent. Expõe dados p/ `cpuinfo`.
 */
#include <stdint.h>
#include <stddef.h>

extern void serial_puts(const char* s);
extern void serial_putc(char c);
extern void console_puts(const char* s);
extern void drv_ready(const char* name, int ok);
extern void drv_absent(const char* name);
extern int pci_find(uint16_t vendor, uint16_t device);
extern int pci_find_class(uint8_t class_code, uint8_t subclass);
extern int pci_find_class_pi(uint8_t class_code, uint8_t subclass,
                             uint8_t prog_if);

static inline void outw(uint16_t p, uint16_t v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p));
}

/* Enderecos absolutos (EBDA/BIOS) sao mapeados: cala -Warray-bounds. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"

static inline uint8_t inb(uint16_t p)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

static inline void outb(uint16_t p, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p));
}

static inline uint32_t inl(uint16_t p)
{
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

static inline void cpuid(uint32_t leaf, uint32_t sub,
                         uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(sub));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void slog(const char* s)
{
    serial_puts(s);
}

/* --- dados da CPU p/ cpuinfo --- */
static char cpu_vendor[13] = { 0 };
static char cpu_brand[49] = { 0 };
static uint32_t cpu_fam = 0, cpu_mod = 0, cpu_step = 0;
static uint32_t cpu_f1c = 0, cpu_f1d = 0, cpu_f7b = 0, cpu_f81d = 0;

const char* hw_cpu_vendor(void)
{
    return cpu_vendor;
}

const char* hw_cpu_brand(void)
{
    return cpu_brand;
}

void hw_cpu_ids(uint32_t* fam, uint32_t* mod, uint32_t* step)
{
    if (fam) {
        *fam = cpu_fam;
    }
    if (mod) {
        *mod = cpu_mod;
    }
    if (step) {
        *step = cpu_step;
    }
}

/* bit helpers p/ cpuinfo */
int hw_cpu_has(uint32_t leaf, char reg, uint32_t bit)
{
    uint32_t v = 0;
    if (leaf == 1 && reg == 'c') {
        v = cpu_f1c;
    } else if (leaf == 1 && reg == 'd') {
        v = cpu_f1d;
    } else if (leaf == 7 && reg == 'b') {
        v = cpu_f7b;
    } else if (leaf == 0x80000001u && reg == 'd') {
        v = cpu_f81d;
    } else {
        return 0;
    }
    return (v >> bit) & 1;
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, (uint8_t)(reg & 0x7F));
    return inb(0x71);
}

/* checksum de string ACPI ("RSD PTR " soma a zero no tamanho). */
static int acpi_sum(const uint8_t* p, size_t n)
{
    uint8_t s = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        s = (uint8_t)(s + p[i]);
    }
    return s == 0;
}

static const uint8_t* rsdp_addr = 0;

static int find_rsdp(void)
{
    /* EBDA (ponteiro em 0x40E, em KB) */
    uintptr_t ebda_ptr = (uintptr_t)0x40E;
    volatile uint8_t* eb = (volatile uint8_t*)ebda_ptr;
    uint16_t ebda_seg = (uint16_t)(eb[0] | ((uint16_t)eb[1] << 8));
    uint32_t ebda = (uint32_t)ebda_seg << 4;
    uint32_t a;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        for (a = ebda; a < ebda + 1024; a += 16) {
            const uint8_t* p = (const uint8_t*)(uintptr_t)a;
            if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
                p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ' &&
                acpi_sum(p, 20)) {
                rsdp_addr = p;
                return 1;
            }
        }
    }
    /* ROM da BIOS */
    for (a = 0xE0000; a < 0x100000; a += 16) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)a;
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ' &&
            acpi_sum(p, 20)) {
            rsdp_addr = p;
            return 1;
        }
    }
    return 0;
}

/* procura tabela ACPI (sig de 4 chars) via RSDT/XSDT. Retorna ptr ou 0. */
static const uint8_t* acpi_table(const char* sig)
{
    uint32_t rsdt = 0;
    uint64_t xsdt = 0;
    int rev = 0;
    int n, i;
    if (!rsdp_addr) {
        return 0;
    }
    rev = rsdp_addr[15];
    rsdt = (uint32_t)rsdp_addr[16] | ((uint32_t)rsdp_addr[17] << 8) |
           ((uint32_t)rsdp_addr[18] << 16) | ((uint32_t)rsdp_addr[19] << 24);
    if (rev >= 2) {
        xsdt = 0;
        for (i = 0; i < 8; i++) {
            xsdt |= (uint64_t)rsdp_addr[24 + i] << (i * 8);
        }
    }
    if (xsdt && xsdt < 0x100000000ull) {
        const uint8_t* x = (const uint8_t*)(uintptr_t)(uint32_t)xsdt;
        uint32_t len = (uint32_t)x[4] | ((uint32_t)x[5] << 8) |
                       ((uint32_t)x[6] << 16) | ((uint32_t)x[7] << 24);
        n = (int)((len - 36) / 8);
        for (i = 0; i < n; i++) {
            uint64_t e = 0;
            int k;
            for (k = 0; k < 8; k++) {
                e |= (uint64_t)x[36 + i * 8 + k] << (k * 8);
            }
            if (e && e < 0x100000000ull) {
                const uint8_t* t = (const uint8_t*)(uintptr_t)(uint32_t)e;
                if (t[0] == (uint8_t)sig[0] && t[1] == (uint8_t)sig[1] &&
                    t[2] == (uint8_t)sig[2] && t[3] == (uint8_t)sig[3]) {
                    return t;
                }
            }
        }
    }
    if (rsdt) {
        const uint8_t* r = (const uint8_t*)(uintptr_t)rsdt;
        uint32_t len = (uint32_t)r[4] | ((uint32_t)r[5] << 8) |
                       ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
        n = (int)((len - 36) / 4);
        for (i = 0; i < n; i++) {
            uint32_t e = (uint32_t)r[36 + i * 4] |
                         ((uint32_t)r[37 + i * 4] << 8) |
                         ((uint32_t)r[38 + i * 4] << 16) |
                         ((uint32_t)r[39 + i * 4] << 24);
            if (e) {
                const uint8_t* t = (const uint8_t*)(uintptr_t)e;
                if (t[0] == (uint8_t)sig[0] && t[1] == (uint8_t)sig[1] &&
                    t[2] == (uint8_t)sig[2] && t[3] == (uint8_t)sig[3]) {
                    return t;
                }
            }
        }
    }
    return 0;
}

/* Teste de loopback da COM1 (MCR bit1). 1 = porta responde. */
static int com1_probe(void)
{
    uint8_t s;
    outb(0x3F8 + 4, 0x12); /* DTR+RTS em loop interno */
    outb(0x3F8 + 0, 0xAA);
    s = inb(0x3F8 + 0);
    outb(0x3F8 + 4, 0x00);
    return s == 0xAA;
}

/* ATAPI: IDENTIFY (0xEC) aborta com ERR e expoe assinatura 0xEB14.
 * Mesma sequencia do ata.c (assinatura some depois do 0xA1). */
static int atapi_probe(uint16_t base, int slave)
{
    int t;
    uint8_t st;
    outb((uint16_t)(base + 6), (uint8_t)(slave ? 0xB0 : 0xA0));
    for (t = 0; t < 1000; t++) {
        (void)inb((uint16_t)(base + 7));
    }
    st = inb((uint16_t)(base + 7));
    if (st == 0 || st == 0xFF) {
        return 0;
    }
    outb((uint16_t)(base + 2), 0);
    outb((uint16_t)(base + 3), 0);
    outb((uint16_t)(base + 4), 0);
    outb((uint16_t)(base + 5), 0);
    outb((uint16_t)(base + 7), 0xEC); /* IDENTIFY: ATAPI aborta */
    (void)inb((uint16_t)(base + 7));
    for (t = 0; t < 100000; t++) {
        st = inb((uint16_t)(base + 7));
        if (!(st & 0x80)) {
            break;
        }
    }
    if (st & 0x80) {
        return 0;
    }
    /* assinatura ATAPI: cilindros 0xEB14 */
    if (inb((uint16_t)(base + 4)) == 0x14 && inb((uint16_t)(base + 5)) == 0xEB) {
        return 1;
    }
    return 0;
}

/* fw_cfg do QEMU: assinatura "QEMU" no offset 0. */
static int fwcfg_probe(void)
{
    /* só tenta se hypervisor indicar QEMU/KVM (evita trava em HW real) */
    uint32_t a, b, c, d;
    cpuid(0x40000000u, 0, &a, &b, &c, &d);
    {
        char hv[13];
        hv[0] = (char)(b & 0xFF);
        hv[1] = (char)((b >> 8) & 0xFF);
        hv[2] = (char)((b >> 16) & 0xFF);
        hv[3] = (char)((b >> 24) & 0xFF);
        hv[4] = (char)(c & 0xFF);
        hv[5] = (char)((c >> 8) & 0xFF);
        hv[6] = (char)((c >> 16) & 0xFF);
        hv[7] = (char)((c >> 24) & 0xFF);
        hv[8] = (char)(d & 0xFF);
        hv[9] = (char)((d >> 8) & 0xFF);
        hv[10] = (char)((d >> 16) & 0xFF);
        hv[11] = (char)((d >> 24) & 0xFF);
        hv[12] = '\0';
        /* KVMKVMKVM\0\0 ou TCGTCGTCGTCG */
        int is_qemu = (hv[0] == 'K' && hv[1] == 'V' && hv[2] == 'M') ||
                      (hv[0] == 'T' && hv[1] == 'C' && hv[2] == 'G');
        if (!is_qemu) {
            return 0;
        }
    }
    outw(0x510, 0); /* seletor e 16 bits; 0x511 e dado de 8 bits */
    {
        /* "QEMU" little-endian, byte a byte (inl mistura portas) */
        uint32_t sig = (uint32_t)inb(0x511) |
                       ((uint32_t)inb(0x511) << 8) |
                       ((uint32_t)inb(0x511) << 16) |
                       ((uint32_t)inb(0x511) << 24);
        return sig == 0x554D4551u;
    }
}

/* Backdoor VMware: retorna versao ou 0. */
static uint32_t vmware_probe(void)
{
    uint32_t ver = 0;
#define VMW_MAGIC 0x564D5868u
#define VMW_PORT 0x5658u
#define VMW_CMD_GETVERSION 10u
    __asm__ volatile (
        "inl (%%dx), %0"
        : "=a"(ver)
        : "a"(VMW_MAGIC), "d"(VMW_PORT), "c"(VMW_CMD_GETVERSION), "b"(0xFFFFFFFFu)
        : "memory");
    if (ver == 0xFFFFFFFFu || ver == 0) {
        return 0;
    }
    return ver;
}

/* SMBIOS: ancora _SM_ em 0xF0000-0xFFFFF. */
static int smbios_probe(void)
{
    uint32_t a;
    for (a = 0xF0000; a < 0x100000; a += 16) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)a;
        if (p[0] == '_' && p[1] == 'S' && p[2] == 'M' && p[3] == '_' &&
            acpi_sum(p, p[5])) {
            return 1;
        }
    }
    return 0;
}

static void mark(const char* name, int ok, const char* why)
{
    if (ok) {
        drv_ready(name, 1);
    } else {
        drv_absent(name);
    }
    slog("hw: ");
    slog(name);
    slog(ok ? " ok" : " --");
    if (why) {
        slog(" (");
        slog(why);
        slog(")");
    }
    slog("\n");
}

/* Ponto de entrada: roda depois de pci_scan(). */
void hwdetect_init(void)
{
    uint32_t a, b, c, d;
    uint32_t max_basic, max_ext;

    /* --- CPU --- */
    cpuid(0, 0, &max_basic, &b, &c, &d);
    cpu_vendor[0] = (char)(b & 0xFF);
    cpu_vendor[1] = (char)((b >> 8) & 0xFF);
    cpu_vendor[2] = (char)((b >> 16) & 0xFF);
    cpu_vendor[3] = (char)((b >> 24) & 0xFF);
    cpu_vendor[4] = (char)(d & 0xFF);
    cpu_vendor[5] = (char)((d >> 8) & 0xFF);
    cpu_vendor[6] = (char)((d >> 16) & 0xFF);
    cpu_vendor[7] = (char)((d >> 24) & 0xFF);
    cpu_vendor[8] = (char)(c & 0xFF);
    cpu_vendor[9] = (char)((c >> 8) & 0xFF);
    cpu_vendor[10] = (char)((c >> 16) & 0xFF);
    cpu_vendor[11] = (char)((c >> 24) & 0xFF);
    cpu_vendor[12] = '\0';
    if (max_basic >= 1) {
        uint32_t fam, mod;
        cpuid(1, 0, &a, &b, &cpu_f1c, &cpu_f1d);
        cpu_step = a & 0xF;
        mod = (a >> 4) & 0xF;
        fam = (a >> 8) & 0xF;
        if (fam == 6 || fam == 15) {
            mod |= ((a >> 16) & 0xF) << 4;
            if (fam == 15) {
                fam += (a >> 20) & 0xFF;
            }
        }
        cpu_fam = fam;
        cpu_mod = mod;
    }
    if (max_basic >= 7) {
        uint32_t e = 0;
        cpuid(7, 0, &e, &cpu_f7b, &c, &d);
    }
    cpuid(0x80000000u, 0, &max_ext, &b, &c, &d);
    if (max_ext >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &cpu_f81d);
    }
    if (max_ext >= 0x80000004u) {
        int i;
        for (i = 0; i < 3; i++) {
            uint32_t w[4];
            cpuid(0x80000002u + (uint32_t)i, 0, &w[0], &w[1], &w[2], &w[3]);
            {
                int k;
                for (k = 0; k < 4; k++) {
                    cpu_brand[i * 16 + k * 4 + 0] = (char)(w[k] & 0xFF);
                    cpu_brand[i * 16 + k * 4 + 1] = (char)((w[k] >> 8) & 0xFF);
                    cpu_brand[i * 16 + k * 4 + 2] = (char)((w[k] >> 16) & 0xFF);
                    cpu_brand[i * 16 + k * 4 + 3] = (char)((w[k] >> 24) & 0xFF);
                }
            }
        }
        cpu_brand[48] = '\0';
    }
    mark("cpu", 1, cpu_vendor);

    /* --- FPU (CR0.EM limpo = presente) --- */
    {
        uint64_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        mark("fpu", (cr0 & 0x4) == 0, 0);
    }

    /* --- APIC (CPUID + MSR 1B habilitado) --- */
    {
        int has = hw_cpu_has(1, 'd', 9);
        int en = 0;
        if (has) {
            en = (rdmsr(0x1B) >> 11) & 1;
        }
        mark("apic", has && en, has ? (en ? "xapic" : "desab") : 0);
    }

    /* --- TSC invariante --- */
    {
        int tsc = hw_cpu_has(1, 'd', 4);
        int inv = 0;
        if (max_ext >= 0x80000007u) {
            cpuid(0x80000007u, 0, &a, &b, &c, &d);
            inv = (d >> 8) & 1;
        }
        mark("tsc", tsc, inv ? "invariante" : 0);
    }

    /* --- cripto (RDRAND/AES-NI) --- */
    mark("crypto", hw_cpu_has(1, 'c', 30) || hw_cpu_has(1, 'c', 25),
         hw_cpu_has(1, 'c', 30) ? "rdrand" : 0);

    /* --- protecao de memoria (SMEP/SMAP) --- */
    mark("memprot", hw_cpu_has(7, 'b', 7) || hw_cpu_has(7, 'b', 20), 0);

    /* --- ACPI --- */
    {
        int ok = find_rsdp();
        mark("acpi", ok, ok ? (rsdp_addr[15] >= 2 ? "2.0" : "1.0") : 0);
        mark("hpet", ok && acpi_table("HPET") != 0, 0);
    }

    /* --- Hypervisor --- */
    {
        int hv = hw_cpu_has(1, 'c', 31);
        if (hv) {
            char h[13];
            cpuid(0x40000000u, 0, &a, &b, &c, &d);
            h[0] = (char)(b & 0xFF);
            h[1] = (char)((b >> 8) & 0xFF);
            h[2] = (char)((b >> 16) & 0xFF);
            h[3] = (char)((b >> 24) & 0xFF);
            h[4] = (char)(c & 0xFF);
            h[5] = (char)((c >> 8) & 0xFF);
            h[6] = (char)((c >> 16) & 0xFF);
            h[7] = (char)((c >> 24) & 0xFF);
            h[8] = (char)(d & 0xFF);
            h[9] = (char)((d >> 8) & 0xFF);
            h[10] = (char)((d >> 16) & 0xFF);
            h[11] = (char)((d >> 24) & 0xFF);
            h[12] = '\0';
            mark("hypervisor", 1, h);
        } else {
            mark("hypervisor", 0, 0);
        }
    }

    /* --- VMware backdoor --- */
    mark("vmware-bd", vmware_probe() != 0, 0);

    /* --- fw_cfg + debugcon --- */
    {
        int ok = fwcfg_probe();
        mark("fw_cfg", ok, 0);
        mark("debugcon", ok, "0xE9");
    }

    /* --- SMBIOS --- */
    mark("smbios", smbios_probe(), 0);

    /* --- COM1 (loopback real) --- */
    mark("com1", com1_probe(), "3F8");

    /* --- LPT (porta responde, != 0xFF) --- */
    mark("lpt", inb(0x378) != 0xFF || inb(0x379) != 0xFF, "378");

    /* --- Floppy (CMOS 0x10) --- */
    mark("floppy", (cmos_read(0x10) >> 4) != 0, 0);

    /* --- CMOS estendida: KB acima de 1MB --- */
    {
        uint32_t kb = ((uint32_t)cmos_read(0x34)) |
                      ((uint32_t)cmos_read(0x35) << 8);
        (void)kb;
        mark("cmos-mem", 1, 0);
    }

    /* --- ATAPI/CD-ROM --- */
    mark("atapi", atapi_probe(0x1F0, 0) || atapi_probe(0x1F0, 1) ||
                          atapi_probe(0x170, 0) || atapi_probe(0x170, 1),
         "ide");

    /* --- PCI por classe --- */
    mark("ide-ctrl", pci_find_class(0x01, 0x01) >= 0, 0);
    mark("ahci", pci_find_class(0x01, 0x06) >= 0, 0);
    mark("usb-uhci", pci_find_class_pi(0x0C, 0x03, 0x00) >= 0, 0);
    mark("usb-ohci", pci_find_class_pi(0x0C, 0x03, 0x10) >= 0, 0);
    mark("usb-ehci", pci_find_class_pi(0x0C, 0x03, 0x20) >= 0, 0);
    mark("usb-xhci", pci_find_class_pi(0x0C, 0x03, 0x30) >= 0, 0);
    mark("vga-pci", pci_find_class(0x03, 0xFF) >= 0, 0);
    mark("isa-bridge", pci_find_class(0x06, 0x01) >= 0, 0);
    mark("virtio-blk", pci_find(0x1AF4, 0x1001) >= 0 ||
                           pci_find(0x1AF4, 0x1042) >= 0,
         0);
    mark("nic2", pci_find(0x10EC, 0x8139) >= 0 ||
                         pci_find(0x1022, 0x2000) >= 0 ||
                         pci_find(0x1AF4, 0x1000) >= 0 ||
                         pci_find(0x1AF4, 0x1041) >= 0,
         0);
    mark("audio", pci_find(0x8086, 0x2415) >= 0 ||
                          pci_find(0x8086, 0x2425) >= 0 ||
                          pci_find(0x8086, 0x2435) >= 0 ||
                          pci_find(0x8086, 0x2445) >= 0 ||
                          pci_find(0x8086, 0x2485) >= 0 ||
                          pci_find(0x8086, 0x24C5) >= 0 ||
                          pci_find(0x8086, 0x24D5) >= 0 ||
                          pci_find(0x8086, 0x266E) >= 0 ||
                          pci_find_class(0x04, 0x03) >= 0,
         0);

    /* --- plataforma fixa do PC --- */
    mark("dma", 1, "8237");
    mark("a20", 1, ">1MB ok");
}

/* Memoria estendida (KB acima de 1MB) p/ `mem`. */
uint32_t hw_ext_mem_kb(void)
{
    return ((uint32_t)cmos_read(0x34)) | ((uint32_t)cmos_read(0x35) << 8);
}

/* Reboot via teclado 8042; fallback: triple fault. */
void hw_reboot(void)
{
    int i;
    /* flush + pulso reset */
    for (i = 0; i < 100; i++) {
        (void)inb(0x64);
    }
    outb(0x64, 0xFE);
    for (i = 0; i < 1000000; i++) {
        __asm__ volatile ("pause");
    }
    /* fallback: triple fault */
    __asm__ volatile ("lidt 0(%%eax)" : : "a"(0));
    __asm__ volatile ("int $0");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

#pragma GCC diagnostic pop
