/* mbr.s - MBR do mapple (512 bytes, 16-bit real mode).
 *
 * BIOS carrega em 0x7C00 com DL = drive. Mensagens vao DIRETO p/
 * a VRAM texto (0xB8000, sem INT 10h). Le o core.img do GRUB em
 * blocos de <=32 setores (LBA CORE_LBA.. -> 0x8000) via INT 13h
 * EDD e salta p/ ele (DL preservado). Erro = codigo hex na tela
 * e halt. Tabela de particao (446-509) e preenchida pelo
 * instalador/mkhd.py; assinatura 55AA no fim.
 *
 * Monta com: as --32 + .code16, ld -m elf_i386, objcopy -O binary.
 * CORE_LBA/CORE_N via --defsym (passados pelo Makefile).
 */
    .code16
    .text
    .globl _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp
    sti
    pushw %dx                 /* salva drive */
    movw $msg_banner, %si
    call puts
    popw %dx
    pushw %dx
    /* mostra DL (drive de boot) */
    movb %dl, %al
    shr $4, %al
    call puthex
    movb %dl, %al
    call puthex
    movb $' ', %al
    call putc

    /* le em blocos de <=32 setores p/ o bounce 0x1000 (sempre <64KB:
     * este SeaBIOS recusa transferencia terminando em >=0x10000) e
     * depois copia p/ o destino final (qualquer endereco). */
    movw $CORE_N, %cx         /* restantes */
    movw $CORE_LBA, cur_lba   /* LBA atual (<64K sempre p/ o core) */
    movw $0, dap_lba+2        /* metade alta do LBA: zero */
    movw $0x0800, cur_seg     /* destino final 0x0800:0 = 0x8000 */
read_loop:
    testw %cx, %cx
    jz read_done
    cmpw $32, %cx
    jbe have_n
    movw $32, %bx
    jmp do_read
have_n:
    movw %cx, %bx
do_read:
    movw %bx, saved_n
    movw %bx, dap_count
    movw $0x1000, dap_seg     /* bounce: linear 0x1000 */
    movw cur_lba, %ax
    movw %ax, dap_lba
    movw $dap, %si
    movb $0x42, %ah
    pushw %cx
    pushw %dx                 /* BIOS pode sujar DL: salva */
    int $0x13
    movb %ah, %bl             /* BL = status (mov nao mexe em CF) */
    popw %dx
    popw %cx
    jc disk_err               /* CF direto: sem salvar flags */
    /* BIOS pode ter sujado DS/ES: restaura */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    /* copia bounce -> destino final (destroi CX: salvo antes) */
    pushw %cx
    movw cur_seg, %ax
    movw %ax, %es
    movw $0x1000, %si
    xorw %di, %di
    movw saved_n, %cx
    shlw $8, %cx              /* n*256 words */
    cld
    rep movsw
    popw %cx
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movb $'.', %al
    call putc
    /* avanca: lba += n; seg += n*32 paragrafos; restantes -= n */
    movw saved_n, %bx
    movw cur_lba, %ax
    addw %bx, %ax
    movw %ax, cur_lba
    shlw $5, %bx
    addw %bx, cur_seg
    movw saved_n, %bx
    subw %bx, %cx
    jmp read_loop
read_done:
    popw %dx
    movw $msg_ok, %si
    call puts
    ljmp $0x0000, $0x8000     /* core.img do GRUB */

disk_err:
    movw $msg_err, %si
    call puts
    movb %bl, %al
    shr $4, %al
    call puthex               /* nibble alto */
    movb %bl, %al
    call puthex               /* nibble baixo (puthex mascara) */
    movw $msg_nl, %si
    call puts
hang:
    hlt
    jmp hang

/* --- saida direta na VRAM texto (sem BIOS) --- */
cursor_cell:
    .word 160                 /* celula (row1 col0; header SeaBIOS em cima) */

/* putc: AL -> VRAM no cursor (entende \r \n) */
putc:
    pushw %ax
    pushw %bx
    pushw %cx
    pushw %dx
    pushw %es
    movw $0xB800, %bx
    movw %bx, %es
    cmpb $0x0D, %al           /* \r: volta p/ coluna 0 */
    je putc_cr
    cmpb $0x0A, %al           /* \n: proxima linha */
    je putc_nl
    movw cursor_cell, %bx
    cmpw $2000, %bx
    jae putc_end
    movb %al, %es:(%bx)
    movb $0x07, %es:1(%bx)
    addw $2, %bx
    movw %bx, cursor_cell
    jmp putc_end
putc_cr:
    movw cursor_cell, %bx
    movw %bx, %ax
    shr $1, %ax               /* celula -> indice */
    movw $80, %cx
    xorw %dx, %dx
    divw %cx                  /* dx = coluna */
    subw %dx, %bx
    subw %dx, %bx
    movw %bx, cursor_cell
    jmp putc_end
putc_nl:
    movw cursor_cell, %bx
    addw $160, %bx
    cmpw $2000, %bx
    jb putc_nl_ok
    movw $1920, %bx           /* trava na ultima linha */
putc_nl_ok:
    movw %bx, cursor_cell
putc_end:
    popw %es
    popw %dx
    popw %cx
    popw %bx
    popw %ax
    ret

/* puts: SI -> string ASCIIZ */
puts:
    pushw %ax
puts_l:
    lodsb
    testb %al, %al
    jz puts_end
    call putc
    jmp puts_l
puts_end:
    popw %ax
    ret

/* puthex: nibble baixo de AL em hex */
puthex:
    pushw %ax
    andb $0x0F, %al
    cmpb $10, %al
    jb puthex_d
    addb $('A' - 10), %al
    jmp puthex_o
puthex_d:
    addb $'0', %al
puthex_o:
    call putc
    popw %ax
    ret

msg_banner:
    .ascii "MG mapple HD\r\n"
    .asciz "carregando..."
msg_ok:
    .asciz " ok\r\n"
msg_err:
    .asciz "\r\nerro de disco "
msg_nl:
    .asciz "\r\n"

    .align 4
dap:
    .byte 16, 0               /* tamanho, reservado */
dap_count:
    .word 0                   /* preenchido por bloco */
dap_off:
    .word 0x0000              /* offset 0 */
dap_seg:
    .word 0x0800              /* segmento (0x0800:0 = 0x8000) */
dap_lba:
    .long 0                   /* LBA (preenchido por bloco) */
    .long 0
cur_lba:
    .long 0
cur_seg:
    .word 0
saved_n:
    .word 0
saved_f:
    .word 0

    /* preenche ate 440 (codigo); 440-445 disco; 446-509 tabela */
    .org 440
    .fill 6, 1, 0
    /* tabela de particao: 64 bytes zerados (instalador preenche) */
    .fill 64, 1, 0
    .word 0xAA55
