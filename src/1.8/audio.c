/* audio.c - Controle de audio do mapple (mixer + deteccao AC97/HDA).
 *
 *  - Mixer de software: volume 0-100 (padrao 80) + mute. Vale p/ tudo
 *    que toca som (beep/play/Musica). PC Speaker nao tem amplitude
 *    analogica, entao o volume e emulado por "pulsing": cada fatia de
 *    20ms toca on = 20*vol/100 ms e pausa o resto. 100 = som continuo
 *    (igual ao pcspk puro), 0/mute = so pausa (silencio total).
 *  - Deteccao PCI: AC97 (8086:2415/2425/2435/2445/2485/24C5/24D5/266E)
 *    e HDA (classe 0403). Le BARs e reporta; streaming com DMA fica no
 *    roadmap (o mixer ja esta pronto p/ quando o backend existir).
 *  - Shell: `vol [0-100]`, `mute [on|off]`, `audioinfo`.
 *    GUI: app Musica mostra volume; Configuracoes pode chamar
 *    audio_set_volume() (mesma variavel, sem duplicar estado).
 */
#include <stdint.h>
#include <stddef.h>

extern void serial_puts(const char* s);
extern void console_putc(char c);
extern void console_puts(const char* s);
extern void console_print_u64(uint64_t v);
extern void drv_ready(const char* name, int ok);
extern void drv_absent(const char* name);
extern int pci_find(uint16_t vendor, uint16_t device);
extern int pci_find_class(uint8_t class_code, uint8_t subclass);
extern uint64_t pit_get_ticks(void);
extern void cpu_hlt(void);
extern void pcspk_beep(uint32_t hz, uint32_t ms);

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass;
    uint32_t bar[6];
} pci_dev_audio_t;
extern const pci_dev_audio_t* pci_get(int idx);

static int audio_vol = 80;
static int audio_mute = 0;
static int audio_ac97 = 0;
static int audio_hda = 0;

static void sleep_ms(uint64_t ms)
{
    uint64_t fim = pit_get_ticks() + (ms * 100 + 999) / 1000;
    if (fim == pit_get_ticks()) {
        fim++;
    }
    while (pit_get_ticks() < fim) {
        cpu_hlt();
    }
}

void audio_init(void)
{
    static const uint16_t ac97_devs[] = {
        0x2415, 0x2425, 0x2435, 0x2445, 0x2485, 0x24C5, 0x24D5, 0x266E
    };
    size_t i;
    audio_ac97 = 0;
    audio_hda = 0;
    for (i = 0; i < sizeof(ac97_devs) / sizeof(ac97_devs[0]); i++) {
        if (pci_find(0x8086, ac97_devs[i]) >= 0) {
            audio_ac97 = 1;
            break;
        }
    }
    if (pci_find_class(0x04, 0x03) >= 0) {
        audio_hda = 1;
    }
    if (audio_ac97 || audio_hda) {
        drv_ready("audio", 1);
    } else {
        drv_absent("audio");
    }
    drv_ready("mixer", 1);
    drv_ready("pcspk", 1);
    serial_puts("audio: mixer vol=80 mute=off (pcspk base");
    serial_puts(audio_ac97 ? ", ac97 detectado" : "");
    serial_puts(audio_hda ? ", hda detectado" : "");
    serial_puts(")\n");
}

int audio_get_volume(void)
{
    return audio_vol;
}
int audio_is_muted(void)
{
    return audio_mute;
}

void audio_set_volume(int v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    audio_vol = v;
}

void audio_set_mute(int m)
{
    audio_mute = m ? 1 : 0;
}

/* Toca com volume (usado por beep/play/Musica). */
void audio_beep(uint32_t hz, uint32_t ms)
{
    uint32_t left;
    if (audio_mute || audio_vol <= 0) {
        sleep_ms(ms);
        return;
    }
    if (hz < 20 || hz > 20000) {
        sleep_ms(ms);
        return;
    }
    if (audio_vol >= 100) {
        pcspk_beep(hz, ms);
        return;
    }
    /* pulsing em fatias de 20ms */
    left = ms;
    while (left > 0) {
        uint32_t sl = left > 20 ? 20 : left;
        uint32_t on = (sl * (uint32_t)audio_vol) / 100;
        if (on > 0) {
            pcspk_beep(hz, on);
        }
        if (sl > on) {
            sleep_ms(sl - on);
        }
        left -= sl;
    }
}

/* Toca sequencia "freq:ms,..." com volume (irma do pcspk_play). */
void audio_play(const char* seq)
{
    while (*seq) {
        uint32_t f = 0, ms = 0;
        while (*seq >= '0' && *seq <= '9') {
            f = f * 10 + (uint32_t)(*seq - '0');
            seq++;
        }
        if (*seq == ':') {
            seq++;
        }
        while (*seq >= '0' && *seq <= '9') {
            ms = ms * 10 + (uint32_t)(*seq - '0');
            seq++;
        }
        if (ms == 0) {
            ms = 150;
        }
        audio_beep(f, ms);
        if (*seq == ',') {
            seq++;
        } else if (*seq && *seq != ' ') {
            break;
        }
        while (*seq == ' ') {
            seq++;
        }
    }
}

void audio_info(void)
{
    console_puts("mixer: vol=");
    console_print_u64((uint64_t)audio_vol);
    console_puts(audio_mute ? " mute=on" : " mute=off");
    console_puts(" (base: pcspk via PIT ch2)\n");
    console_puts(audio_ac97 ? "ac97: detectado (DMA pendente)\n" : "ac97: ausente\n");
    console_puts(audio_hda ? "hda: detectado (DMA/codec pendente)\n" : "hda: ausente\n");
    console_puts("dica QEMU p/ ouvir: -audiodev pa,id=a -machine pcspk-audiodev=a\n");
}
