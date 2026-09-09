# Jogos + `rm -rf` — o que foi feito (mapple 0.13.0)

Data: 2026-09-08. Alterados: `gui.c`, `shell.c`, `keyboard.c`, `fs.c`,
`drv.c` (+ este doc, `README.md`, `contexto.txt`). Shell foi de 71 para
**78 comandos**. Testado em QEMU headless (serial + HMP `sendkey` +
`screendump`) e com KVM.

## 1. Foon — raycaster estilo Doom (`foon`)

- **Comando:** `foon` (menu Iniciar, 2ª coluna). Requer modo gráfico.
- **Como joga:** `WASD`/setas andam e viram, `Esp` atira, `q`/`Esc` sai,
  `Enter` reinicia após morte/vitória.
- **Motor (só inteiros, Q10):** mapa 16x12, DDA por coluna (1 raio a cada
  4 px, ~150 raios), distância **perpendicular** (sem fish-eye),
  sombreamento por distância + lado da parede, teto/chão chapados.
- **Inimigos:** 4 demônios (billboards ordenados longe→perto, com olhos
  e barra de vida) que perseguem o jogador; tiro hitscan no raio central
  (limitado pela parede); encostou = dano; 400 pts = fase limpa.
- **HUD:** HP, pontos, mira, minimapa. Sons via mixer (`audio_beep`,
  respeita `vol`/`mute`).
- **Driver em `drivers`:** `foon`.

## 2. Três jogos arcade

| Jogo | Comando | Controles | Regras |
|---|---|---|---|
| Pong | `pong` | `W`/`S` ou setas | você (esq) x CPU (dir), 5 pontos vencem, `Enter` reinicia |
| Breakout | `breakout` | `A`/`D` ou setas | 60 tijolos (6 cores), 3 vidas, `Enter` reinicia |
| Tetris | `tetris` | setas movem, cima gira, `Esp` cai de uma vez | 10x20, 7 peças, pontos por linha (40/100/300/1200 × nível), nível sobe a cada 10 linhas (máx 10) |

Todos seguem o padrão do Snake: loop modal com leitura não-bloqueante
da `appq` + tick por PIT + `q`/`Esc` fecha e devolve o foco ao terminal.
**Driver em `drivers`:** `arcade` (um para os três).

Infra nova: `gui_game_modal()` + `game_run_loop()` (despacha pelo tipo
da janela focada) com **um único** bloco de pump em `shell.c` e em
`keyboard.c` (em vez de 4 blocos em cada).

## 3. Três demos 3D para testar a GPU

Abrem e animam sozinhas (padrão do `3ddd`: sem loop modal, invalidadas
todo frame em `gui_poll`). Feche pelo `[X]` (ou `Alt+T` p/ devolver o
foco ao terminal, como no `3ddd`).

| App | Comando | O que testa |
|---|---|---|
| Torus | `torus` | donut paramétrico 12x8 = 96 quads (`fill_tri` + painter + flat shading com normal analítica) |
| Terreno | `terreno` | heightfield 14x14 em wireframe (`draw_line`, 196 nós, cor por altura) |
| Tunel | `tunel` | 12 anéis quadrados em perspectiva voando + poeira (fill-rate) |

Tudo em ponto fixo Q10 reaproveitando `fsin`/`fcos`/`fill_tri`/
`draw_line` do `3ddd`. **Driver em `drivers`:** `gpu3d`.

## 4. Comandos base de arquivos

`cp`, `mv` e `touch` (`toch` foi erro de digitação — o comando é
`touch`) já existiam e continuam iguais. O `rm` foi estendido:

```
rm [-r] [-f] <arq/dir>...
```

- sem flag: só arquivo (diretório agora avisa "use -r" em vez de
  "não encontrado");
- `-r`/`-rf`: recursivo (`fs_rm_recursive` em `fs.c`: apaga arquivos e
  dirs do overlay no caminho e abaixo, contando ambos);
- `-f`: não reclama de caminho inexistente;
- ROM nunca é tocada (`-2` = somente leitura, como antes); `/` é
  recusado; `man rm` documenta.

## 5. Menu em 2 colunas

O menu tinha 17 itens (coluna única). Com +7 jogos foi para 24 e virou
grade de **2 colunas x 12 linhas** (`menu_rows()`, `menu_pos()`), o que
inclusive conserta o estouro vertical em 640x400. Clique e desenho usam
a mesma matemática (verificado por screenshot: ícones + texto nas duas
colunas).

## 6. Bugs caçados (não reintroduzir!)

1. **Parede preta no foon:** brilho `255 - perp/16` zerava com 4 células
   + "correção fish-eye" com cosseno era dupla correção errada (o DDA já
   devolve distância perpendicular). Agora `255 - perp/40`, sem hack.
2. **`sin_build()` faltando no túnel:** tabela de senos zerada = sem
   balanço (anéis parados). Todo draw que usa `fsin`/`fcos` precisa
   chamar `sin_build()` (é idempotente).
3. **Túnel sem fluxo:** fração de profundidade só variava 0–1023 dentro
   do mesmo slot (divisão inteira) = anéis parados. Agora `ph` percorre
   o ciclo completo (`tun_t % (TU_NR*1024)`) com reciclagem e conectores
   pulados no salto.
4. **Projeção do terreno 21x pequena:** `x*half/den` em vez do padrão do
   cubo (`x*D/den` e depois `*half/1024`).
5. **Armadilha de teste:** screenshots comparados com TCG lento parecem
   "congelados"; usar `-accel kvm` + diff de pixels p/ provar animação.
   Apps não-modais roubam o foco: nos testes, `Alt+T` (`sendkey alt-t`)
   antes de digitar o próximo comando.

## 7. Verificado (QEMU + KVM)

- `rm -rf /tmp/test` (com subdir + arquivo) remove tudo; `ls /tmp` vazio.
- Foon: paredes + demônio no screenshot, tiro toca `beep`, `q` volta.
- Pong/Breakout/Tetris: sprites/tijolos/peças no screenshot; hard drops
  no Tetris empilham (22k px coloridos).
- Torus (furo visível), Terreno (wireframe), Tunel (anéis): todos com
  diffs de animação entre shots (1360 / 3337 / 4278 px).
- Menu 2 colunas desenhando 24 itens; `drivers` lista `foon`, `arcade`,
  `gpu3d` como OK.

## 8. Roadmap

- Foon: texturas nas paredes, sprites bitmap, porta/saída, mais fases,
  som de passos/ambiente via mixer.
- Arcade: placar salvo no FS, 2 jogadores no Pong (outra tecla), níveis
  no Breakout.
- 3D: backface culling no torus (hoje só painter), modo arame sobreposto,
  FPS por app.
