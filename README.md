# mapple — kernel x86_64 estilo Unix (C + C++ + Assembly, zero dependências)

Núcleo educacional/hobby em C, C++ e Assembly GAS, modo longo
(long mode), multiboot via GRUB, shell próprio, drivers próprios e
interface gráfica com gerenciador de janelas. Sem libc externa, sem
userspace Linux: tudo roda em anel 0 (CPL 0).

```
mapple 0.12.0 (x86_64, freestanding)
```

C++ é permitido com regras rígidas (ver `Makefile` e `contexto.txt`):
sem exceções/RTTI/heap, sem construtores globais (sem CRT), classes
com `init()` explícito e fronteira `extern "C"` (ex: `browser.cpp`,
`cxxsmoke.cpp`).

## Requisitos

| Ferramenta | Fedora | Debian/Ubuntu |
|---|---|---|
| `gcc`, `as`, `ld` | `gcc` | `gcc`, `binutils` |
| NASM | não precisa | não precisa |
| GRUB p/ ISO | `grub2-tools`, `xorriso` | `grub-pc-bin`, `grub-common`, `xorriso` |
| QEMU | `qemu-system-x86` | `qemu-system-x86` |
| KVM (recomendado) | `/dev/kvm` + grupo `kvm` | idem |

O Makefile detecta `grub2-mkrescue`/`grub-mkrescue` e liga `-accel kvm`
sozinho quando `/dev/kvm` existe (10–50x mais rápido que TCG).

## Como usar

```bash
make              # compila tudo -> build/mapple.iso
make run          # abre o QEMU (GTK + VMware SVGA + KVM se houver)
make RES=800x600  # ISO em outra resolucao (padrao 1024x768x32)
make run-serial   # headless 10s (serial no terminal, p/ CI/teste)
make clean        # limpa build/
```

No terminal gráfico (`isn_terminal`), `help` lista os 52 comandos.
Exemplos: `ls /bin`, `cat /etc/motd`, `cc /home/guest/demo.c`,
`edit /home/guest/notas.txt`, `winfo`, `fps`, `ping 10.0.2.2`,
`beep 440 200`, `browser /demo.html`.

Apps gráficos (menu/dock/ícones/`Alt+Tab` visual): `isn_terminal`,
`Notepad`, `Explorador`, `Tarefas`, `Discos`, `3ddd` (cubo 3D),
`Calc` (usa `cc_eval_expr`), `Sobre`, `Calendario`, `Relogio`,
`Snake` (setas/WASD, Enter reinicia), `Musica` (PC speaker),
`Navegador` (offline: HTML com CSS e scripts, `mapple://` +
arquivos do ramfs, links, voltar/avançar, `Ctrl+L`),
`Alt+T` devolve o foco ao terminal (saída de teclado do beco sem saída).
`Configuracoes` (papel de parede, resolução ao vivo, tamanho do
texto) e `Imagem` (visualizador BMP via `img arquivo.bmp`).

Rede cabeada: `e1000` + ARP/IPv4/ICMP; `netcfg` mostra o IP,
`ping 10.0.2.2` testa o gateway QEMU. Áudio: PC Speaker
(`beep`, `play`; HDA/AC97 com DMA ficam p/ depois).

## Layout do projeto

```
boot.s      bootstrap 32 bits -> long mode, GDT/IDT, page tables 4GB
linker.ld   ELF64, kernel em 0x100000, secoes multiboot/text/ro/data/bss
kernel.c    kmain(magic, mbi): console, IDT, drivers, shell/GUI
vga.c       texto VGA 80x25 (fallback) + cursor
keyboard.c  PS/2 set 1 (US, Shift/Caps/Ctrl/Alt, setas), foco por janela
pit.c       timer 100 Hz (uptime, pacing, timeouts)
fs.c        ramfs ROM + overlay gravavel (cd/rm/touch/mkdir, redirect >)
shell.c     built: 52 applets, readline moderno, PATH /bin:/usr/bin
libc.c      strlen/memcpy/... proprios (o GCC precisa deles)
edit.c      editor tela-cheia estilo nano (^O salva, ^X sair)
cc.c        compilador do subconjunto C (int, +-*/%, print, return)
browser.cpp Navegador offline em C++ (HTML+CSS+scripts, links, historico)
cxxsmoke.cpp prova de C++ no kernel (Makefile tem as regras)
pcspk.c     PC Speaker (beep/play via PIT; HDA/AC97 = roadmap)
bmp.c       BMP 24-bit (visualizador + wallpapers)
e1000.c     Intel 82540EM por polling (anéis TX/RX, sem IRQ)
net.c       ARP + IPv4 + ICMP echo (ping, responde ping)
pit.c       (acima)  timer
font.c      fonte bitmap 8x8 propria (96 glifos)
mouse.c     PS/2 IRQ12, pacotes de 3 bytes, anti-fantasma
gui.c       compositor: WM, apps, terminal, double buffer, dirty-rects
pci.c       enumeracao PCI (0xCF8/0xCFC)
ata.c       identify IDE/ATAPI (discos)
svga.c      VMware SVGA II (deteccao, VRAM direta, FIFO, cursor HW*)
rootfs/     arquivos empacotados no ramfs (bin, etc, home, root, usr)
```

\* FIFO/cursor HW detectados; RECT_FILL e cursor por hardware ficam
desabilitados após divergência empírica com este QEMU (fallback CPU
automático, sem perda funcional).

## Arquitetura (resumo)

```
firmware -> GRUB (VBE 1024x768x32) -> _start (paginacao 2MB, long mode)
  -> kmain -> drivers (PIC remapeado, PIT 100Hz, teclado, mouse, ATA, PCI)
  -> ramfs + overlay -> built (shell) <-> apps graficos (WM)
```

Detalhes completos em `contexto.txt`.

## Testes

Tudo é verificado sem monitor: QEMU headless (`-display none`)
com serial em arquivo + `screendump` (análise de pixels) + OCR com a
própria `font.c` + injeção de teclado/mouse via monitor HMP.
Exemplos em `/tmp/opencode/*.py` (harness local, não versionado).

## Roadmap honesto

1. `pmm/vmm` + heap (hoje: bump linear implícito + overlay fixo)
2. Syscalls + anel 3 + loader ELF (para userspace de verdade)
3. Driver ATA com leitura/escrita + FAT/ext2 somente-leitura
4. Rede (e1000 já enumerada no PCI) +mais apps

O que **não** vai acontecer aqui: BusyBox ELF real e ZFS
(ver `contexto.txt`, seção "Por que não").
