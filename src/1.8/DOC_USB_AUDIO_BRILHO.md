# USB + Áudio + Brilho — o que foi feito (mapple 0.13.0)

Data: 2026-09-08. Arquivos novos: `usb.c`, `audio.c`, `brightness.c`.
Alterados: `Makefile`, `kernel.c`, `drv.c`, `shell.c`, `gui.c`
(+ este doc, `README.md`, `contexto.txt`).

Tudo verificado em QEMU headless (`-display none`, serial + HMP
`sendkey`/`screendump`). Sem KVM aqui? Continua igual, só mais lento.

## 1. Driver USB (`usb.c`)

**O que faz (honesto):** detecta controladoras UHCI/OHCI/EHCI/XHCI via PCI
(classe `0C03` + prog-if `00/10/20/30`, usando o `pci_find_class_pi()` que
já existia), liga bus-master (`pci_enable`), lê os BARs e sonda **porta a
porta** se há dispositivo conectado e em qual velocidade — tudo por polling,
sem IRQ e sem DMA.

| Controladora | Como sonda as portas |
|---|---|
| UHCI | IO `BAR4&~3`; `PORTSC` em `+0x10/+0x12` (16 bits, bit0 = conectado) |
| OHCI | só presença/BAR (portas exigem init completo — reporta `?`) |
| EHCI | MMIO `BAR0`; `CAPLENGTH=[+0]`, `HCSPARAMS=[+4]` (`N_PORTS` = bits 3:0); `PORTSC[n] = op+0x44+n*4`. Conectado = `high` (low/full ficam no companion) |
| XHCI | MMIO `BAR0`; `CAPLENGTH=[+0]` (QEMU: `0x40`), `HCSPARAMS1=[+4]` (**MaxPorts = bits 31:24**); `PORTSC[n] = op+0x400+n*0x10`, velocidade nos bits 10–13 (1 full, 2 low, 3 high, 4–7 super) |

**Mass Storage:** stub honesto. Fazer BOT/SCSI de verdade exige DMA +
transfers de controle/bulk (roadmap). Se há dispositivo conectado,
`usbmount` cria `/mnt/usb` no overlay (ponto de montagem pronto) e marca
`usb-msc` OK; sem dispositivo, recusa com mensagem.

**Comandos:** `usb` (lista controladoras/portas/velocidades), `usbmount`.

**Drivers novos em `drivers`:** `usb-core` (pilha), `usb-msc` (stub).

**Testar:**
```bash
# sem nada: "nenhuma controladora" (ausência honesta)
# com pendrive emulado:
qemu-system-x86_64 -cdrom build/mapple.iso -boot d -m 256 \
  -device qemu-xhci -device usb-storage,drive=p \
  -drive id=p,if=none,format=raw,file=/dev/null
# no shell: usb  ->  xhci p0 [CONECTADO super]
#           usbmount  ->  /mnt/usb pronto
```

## 2. Controle de áudio (`audio.c`)

**Mixer de software:** volume `0–100` (padrão 80) + mute. Vale para tudo
que toca (`beep`, `play`, app Musica). O PC Speaker não tem amplitude
analógica, então o volume é emulado por **pulsing**: cada fatia de 20 ms
toca `20*vol/100` ms e pausa o resto. `vol=100` = som contínuo (igual ao
`pcspk` puro); `vol=0`/mute = silêncio (só espera).

**Detecção PCI:** AC97 (`8086:2415/2425/2435/2445/2485/24C5/24D5/266E`) e
HDA (classe `0403`). Só reporta por enquanto — streaming exige DMA+codec
(roadmap); o mixer já está pronto para quando o backend existir.

**Comandos:** `vol [0-100]` (sem arg mostra), `mute [on|off]` (sem arg
alterna), `audioinfo` (mixer + AC97/HDA). `beep`/`play` agora passam pelo
mixer (`audio_beep`/`audio_play`).

**Drivers em `drivers`:** `mixer` (sempre OK), `audio` (OK só se AC97/HDA
presente — ausente no QEMU padrão, o que é honesto).

**Ouvir de verdade no QEMU:**
```bash
qemu-system-x86_64 -cdrom build/mapple.iso -boot d -m 256 -display gtk \
  -vga vmware -audiodev pa,id=a -machine pcspk-audiodev=a
# no shell: vol 80  |  beep 440 200  |  play 440:150,0:50,660:200
```

## 3. Controle de brilho (`brightness.c` + hook em `gui.c`)

**Nível `10–100`** (padrão 100 = sem atenuação). `bright_apply()` escala
cada pixel XRGB8888 por `nível/100` no `present` (back→front) em
`fb_present_rect()`. Se nível = 100, o caminho rápido (cópia `u64`) é
usado sem custo; senão, pixel a pixel. No modo texto VGA o valor é só
registrado (paleta do hardware — roadmap).

**Comandos:** `brilho [10-100]` (alias `brightness`; sem arg mostra). Em
modo gráfico invalida a tela para aplicar na hora.

**Driver em `drivers`:** `backlight` (sempre OK).

**Verificado:** `screendump` antes/depois de `brilho 60` — pixel médio
55.0 → 32.8 (≈ 60%, exato).

**Limitação conhecida:** no backend GPU-direto da SVGA (desenho direto na
VRAM, sem back buffer) o brilho é desviado — documentado no código; o
QEMU padrão aqui usa back buffer, então funciona.

## 4. Dois bugs clássicos novos (não reintroduzir!)

1. **MMIO só com acesso de 32 bits.** As primeiras sondas EHCI/XHCI usavam
   leitura de 1 byte (`volatile uint8_t*`) e liam sempre 0 — portas sempre
   "vazias" mesmo com pendrive anexado. MMIO neste QEMU exige dword
   (igual ao `e1000.c`, que usa `inl`). Helper `mmio_rd32()` em `usb.c`.
2. **XHCI MaxPorts = bits 31:24 de HCSPARAMS1** (bits 7:0 são MaxSlots).
   Ler bits 7:0 dá 64 "portas" e o fallback escondia o erro. Confirmado via
   HMP `info usb` (Port 1, 5000 Mb/s) + `xp` no MMIO.

## 5. Próximos passos (roadmap)

- USB: DMA + transfers de controle (GET_DESCRIPTOR), endereçamento,
  BOT/SCSI para ler o pendrive de verdade; iterador PCI para 2+ placas do
  mesmo tipo (hoje: 1 por tipo).
- Áudio: backend AC97/HDA com DMA; `vol` passar a controlar PCM de verdade.
- Brilho: aplicar também no path GPU-direto e no texto VGA; salvar nível
  no FS (`/etc/brilho`).
