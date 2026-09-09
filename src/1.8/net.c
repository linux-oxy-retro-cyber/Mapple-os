/* net.c - Pilha minima: ARP + IPv4 + ICMP echo (ping).
 *
 * IP estatico (padrao QEMU user-net: 10.0.2.15/24, GW 10.0.2.2).
 * `netcfg [ip]` troca o IP; `ping <ip> [n]` testa. Responde ping.
 */
#include <stdint.h>
#include <stddef.h>

extern size_t strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern void* memset(void* dst, int c, size_t n);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void console_putc(char c);
extern void console_puts(const char* s);
extern void console_print_u64(uint64_t v);
extern uint64_t pit_get_ticks(void);
extern void cpu_hlt(void);
extern int e1000_present(void);
extern const uint8_t* e1000_mac(void);
extern int e1000_send(const uint8_t* data, uint16_t len);
extern int e1000_recv(uint8_t* out, uint16_t cap, uint16_t* out_len);

static uint8_t net_ip[4] = { 10, 0, 2, 15 };
static const uint8_t net_gw[4] = { 10, 0, 2, 2 };
static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define ARP_N 8
typedef struct {
    int used;
    uint8_t ip[4];
    uint8_t mac[6];
} arp_ent_t;
static arp_ent_t arp_tab[ARP_N];

static uint16_t ip_checksum(const uint8_t* p, size_t n)
{
    uint32_t s = 0;
    size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        s += ((uint16_t)p[i] << 8) | p[i + 1];
    }
    if (i < n) {
        s += (uint16_t)p[i] << 8;
    }
    while (s >> 16) {
        s = (s & 0xFFFF) + (s >> 16);
    }
    return (uint16_t)~s;
}

static int ip_eq(const uint8_t* a, const uint8_t* b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void arp_learn(const uint8_t* ip, const uint8_t* mac)
{
    int i, free = -1;
    for (i = 0; i < ARP_N; i++) {
        if (arp_tab[i].used && ip_eq(arp_tab[i].ip, ip)) {
            memcpy(arp_tab[i].mac, mac, 6);
            return;
        }
        if (!arp_tab[i].used && free < 0) {
            free = i;
        }
    }
    if (free < 0) {
        free = 0;
    }
    arp_tab[free].used = 1;
    memcpy(arp_tab[free].ip, ip, 4);
    memcpy(arp_tab[free].mac, mac, 6);
}

static const uint8_t* arp_find(const uint8_t* ip)
{
    int i;
    for (i = 0; i < ARP_N; i++) {
        if (arp_tab[i].used && ip_eq(arp_tab[i].ip, ip)) {
            return arp_tab[i].mac;
        }
    }
    return 0;
}

/* Monta Ethernet+ARP. op: 1 request, 2 reply. */
static void arp_send(int op, const uint8_t* dst_mac, const uint8_t* t_ip)
{
    uint8_t f[42];
    const uint8_t* m = e1000_mac();
    memset(f, 0, sizeof(f));
    memcpy(f + 0, dst_mac, 6);
    memcpy(f + 6, m, 6);
    f[12] = 0x08;
    f[13] = 0x06;
    f[14] = 0x00;
    f[15] = 0x01;
    f[16] = 0x08;
    f[17] = 0x00;
    f[18] = 0x06;
    f[19] = 0x04;
    f[20] = 0x00;
    f[21] = (uint8_t)op;
    memcpy(f + 22, m, 6);
    memcpy(f + 28, net_ip, 4);
    memcpy(f + 32, op == 1 ? bcast : dst_mac, 6);
    if (op == 1) {
        memset(f + 32, 0, 6);
    }
    memcpy(f + 38, t_ip, 4);
    e1000_send(f, sizeof(f));
}

/* IPv4 + ICMP echo. id/seq, payload padrao. */
static void icmp_send(const uint8_t* dst_mac, const uint8_t* dst_ip,
                      uint16_t id, uint16_t seq, uint8_t type)
{
    uint8_t f[128];
    const uint8_t* m = e1000_mac();
    uint16_t ck;
    memset(f, 0, sizeof(f));
    memcpy(f + 0, dst_mac, 6);
    memcpy(f + 6, m, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    /* IPv4 */
    f[14] = 0x45;
    f[16] = 0x00;
    f[17] = 48; /* total: 20 + 28 */
    f[18] = 0x12;
    f[19] = 0x34;
    f[20] = 0x40;
    f[21] = 0x00;
    f[22] = 64; /* TTL */
    f[23] = 0x01; /* ICMP */
    memcpy(f + 26, net_ip, 4);
    memcpy(f + 30, dst_ip, 4);
    ck = ip_checksum(f + 14, 20);
    f[24] = (uint8_t)(ck >> 8);
    f[25] = (uint8_t)(ck & 0xFF);
    /* ICMP: tipo[34] codigo[35] checksum[36-37] id[38-39] seq[40-41] */
    f[34] = type; /* 8 echo, 0 reply */
    f[35] = 0;
    f[36] = 0;
    f[37] = 0;
    f[38] = (uint8_t)(id >> 8);
    f[39] = (uint8_t)(id & 0xFF);
    f[40] = (uint8_t)(seq >> 8);
    f[41] = (uint8_t)(seq & 0xFF);
    {
        int i;
        for (i = 0; i < 20; i++) {
            f[42 + i] = (uint8_t)(0xA0 + i);
        }
    }
    ck = ip_checksum(f + 34, 28);
    f[36] = (uint8_t)(ck >> 8);
    f[37] = (uint8_t)(ck & 0xFF);
    e1000_send(f, 62);
}

/* Processa 1 frame recebido. Retorna tipo: 0 nada, 1 arp, 2 icmp-echo,
 * 3 icmp-reply (com id e seq em o1/o2). */
static int net_poll_one(uint16_t* o1, uint16_t* o2)
{
    static uint8_t f[2048];
    uint16_t n = 0;
    int r = e1000_recv(f, sizeof(f), &n);
    uint16_t eth;
    if (r <= 0) {
        return 0;
    }
    if (n < 14) {
        return 0;
    }
    eth = ((uint16_t)f[12] << 8) | f[13];
    if (eth == 0x0806 && n >= 42) {
        /* ARP */
        uint16_t op = ((uint16_t)f[20] << 8) | f[21];
        arp_learn(f + 28, f + 22);
        if (op == 1 && ip_eq(f + 38, net_ip)) {
            arp_send(2, f + 22, f + 28); /* reply p/ quem perguntou */
        }
        return 1;
    }
    if (eth == 0x0800 && n >= 62) {
        /* IPv4/ICMP */
        if ((f[14] >> 4) != 4 || f[23] != 0x01) {
            return 0;
        }
        if (!ip_eq(f + 30, net_ip)) {
            return 0;
        }
        {
            uint8_t type = f[34];
            uint16_t id = ((uint16_t)f[38] << 8) | f[39];
            uint16_t seq = ((uint16_t)f[40] << 8) | f[41];
            arp_learn(f + 26, f + 6);
            if (type == 8) {
                icmp_send(f + 6, f + 26, id, seq, 0); /* responde ping */
                return 2;
            }
            if (type == 0) {
                if (o1) {
                    *o1 = id;
                }
                if (o2) {
                    *o2 = seq;
                }
                return 3;
            }
        }
    }
    return 0;
}

static int parse_ip(const char* s, uint8_t out[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t v = 0;
        int nd = 0;
        if (!*s) {
            return -1;
        }
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255) {
                return -1;
            }
            s++;
            nd++;
        }
        if (!nd) {
            return -1;
        }
        out[i] = (uint8_t)v;
        if (i < 3) {
            if (*s != '.') {
                return -1;
            }
            s++;
        }
    }
    return (*s == '\0' || *s == ' ') ? 0 : -1;
}

void netcfg_cmd(const char* args)
{
    uint8_t ip[4];
    if (!*args) {
        console_puts("ip ");
        console_print_u64(net_ip[0]);
        console_putc('.');
        console_print_u64(net_ip[1]);
        console_putc('.');
        console_print_u64(net_ip[2]);
        console_putc('.');
        console_print_u64(net_ip[3]);
        console_puts(" gw ");
        console_print_u64(net_gw[0]);
        console_putc('.');
        console_print_u64(net_gw[1]);
        console_putc('.');
        console_print_u64(net_gw[2]);
        console_putc('.');
        console_print_u64(net_gw[3]);
        console_putc('\n');
        return;
    }
    if (parse_ip(args, ip) == 0) {
        memcpy(net_ip, ip, 4);
        console_puts("ip ok\n");
    } else {
        console_puts("uso: netcfg [a.b.c.d]\n");
    }
}

/* ping <ip> [n]: ARP + echo, mostra ms (ticks de 10ms). */
void ping_cmd(const char* args)
{
    uint8_t dst[4];
    int count = 4, sent = 0, got = 0, i;
    const char* p = args;
    if (!e1000_present()) {
        console_puts("ping: sem NIC (e1000 ausente)\n");
        return;
    }
    while (*p && *p != ' ') {
        p++;
    }
    {
        char ipb[32];
        size_t k = 0;
        while (args < p && k < sizeof(ipb) - 1) {
            ipb[k++] = *args++;
        }
        ipb[k] = '\0';
        if (parse_ip(ipb, dst) != 0) {
            console_puts("uso: ping <a.b.c.d> [n]\n");
            return;
        }
    }
    while (*p == ' ') {
        p++;
    }
    if (*p) {
        count = 0;
        while (*p >= '0' && *p <= '9') {
            count = count * 10 + (*p - '0');
            p++;
        }
        if (count <= 0 || count > 20) {
            count = 4;
        }
    }
    for (i = 0; i < count; i++) {
        const uint8_t* mac = arp_find(dst);
        uint64_t t0;
        if (!mac) {
            uint64_t t = pit_get_ticks() + 100; /* 1s p/ ARP */
            arp_send(1, bcast, dst);
            while (pit_get_ticks() < t) {
                cpu_hlt();
                net_poll_one(0, 0);
                mac = arp_find(dst);
                if (mac) {
                    break;
                }
            }
            if (!mac) {
                console_puts("sem resposta ARP\n");
                continue;
            }
        }
        t0 = pit_get_ticks();
        icmp_send(mac, dst, 0xBEEF, (uint16_t)i, 8);
        sent++;
        {
            int ok = 0;
            uint64_t t = pit_get_ticks() + 100;
            while (pit_get_ticks() < t) {
                cpu_hlt();
                uint16_t id = 0, seq = 0;
                int r = net_poll_one(&id, &seq);
                if (r == 3 && id == 0xBEEF && seq == (uint16_t)i) {
                    uint64_t ms = (pit_get_ticks() - t0) * 10;
                    console_puts("resposta de ");
                    console_print_u64(dst[0]);
                    console_putc('.');
                    console_print_u64(dst[1]);
                    console_putc('.');
                    console_print_u64(dst[2]);
                    console_putc('.');
                    console_print_u64(dst[3]);
                    console_puts(": tempo=");
                    console_print_u64(ms);
                    console_puts("ms\n");
                    got++;
                    ok = 1;
                    break;
                }
            }
            if (!ok) {
                console_puts("timeout\n");
            }
        }
        {
            uint64_t t = pit_get_ticks() + 20; /* 200ms entre pings */
            while (pit_get_ticks() < t) {
                cpu_hlt();
                net_poll_one(0, 0);
            }
        }
    }
    console_puts("enviados=");
    console_print_u64((uint64_t)sent);
    console_puts(" recebidos=");
    console_print_u64((uint64_t)got);
    console_putc('\n');
}
