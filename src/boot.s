/* boot.s - Bootstrap x86_64 (sintaxe GAS, AT&T)
 *
 * Carregado pelo GRUB (Multiboot1) ja em protected mode 32-bit,
 * sem paginacao. Aqui:
 *  1. Monta page tables com paginas de 2MB (identidade, primeiros 2MB ou 1GB)
 *  2. Habilita PAE, LME (EFER), PG -> entra em long mode
 *  3. Carrega GDT64 e faz far-jump para codigo 64-bit
 *  4. Inicializa stack e chama kmain
 *  5. Stubs de interrupcao + lidt + set_idt_entry (64-bit)
 */

.set MB_MAGIC,    0x1BADB002
/* align + meminfo + VIDEO (pede framebuffer linear ao GRUB) */
.set MB_FLAGS,    (1<<0) | (1<<1) | (1<<2)
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM
/* campos de endereco (ignorados p/ ELF, mas o cabecalho precisa deles
 * quando o bit VIDEO esta ligado) */
.long 0, 0, 0, 0, 0
/* modo video: 0 = framebuffer linear; WxH ajustavel via
 * `as --defsym RES_W=.. --defsym RES_H=..` (padrao 1024x768x32) */
.ifndef RES_W
.set RES_W, 1024
.endif
.ifndef RES_H
.set RES_H, 768
.endif
.long 0
.long RES_W
.long RES_H
.long 32

/* Stack + page tables (BSS, 4096-aligned) */
.section .bss, "aw", @nobits
.align 4096
stack_bottom:
.skip 16384
stack_top:

.align 4096
pml4_table:
.skip 4096
pdpt_table:
.skip 4096
/* 4 tabelas PD: 4 x 512 x 2MB = 4GB identity-map (cobre RAM + MMIO) */
pd_table0:
.skip 4096
pd_table1:
.skip 4096
pd_table2:
.skip 4096
pd_table3:
.skip 4096
/* IDT: 256 entradas x 16 bytes = 4096 bytes */
.align 16
idt_start:
.skip 4096
idt_end:

/* magic (EAX) e ponteiro mbi (EBX) vindos do GRUB */
.align 4
mb_magic:
.skip 4
mb_info:
.skip 4

/* GDT64 + ponteiros (dados inicializados -> .rodata) */
.section .rodata, "a"
.align 8
gdt64_start:
.quad 0x0000000000000000          /* 0x00: null */
.quad 0x00209A0000000000          /* 0x08: code 64-bit exec/read */
.quad 0x0000920000000000          /* 0x10: data read/write */
gdt64_end:
gdt64_ptr:
.word gdt64_end - gdt64_start - 1
.quad gdt64_start

.align 16
idt_ptr:
.word idt_end - idt_start - 1
.quad idt_start

/* Codigo 32-bit de bootstrap */
.section .text, "ax"
.code32
.global _start
.type _start, @function
_start:
    cli

    /* Preserva EAX (magic) e EBX (mbi) na pilha do GRUB:
     * o loop de zeragem abaixo usa EAX. */
    pushl %eax
    pushl %ebx

    /* GRUB nao garante BSS zerada p/ ELF: zera [__bss_start, _end).
     * OBRIGATORIO antes de usar stack/tabelas (tudo mora no .bss). */
    cld
    movl $__bss_start, %edi
    movl $_end, %ecx
    subl %edi, %ecx
    xorb %al, %al
    rep stosb

    /* salva magic/mbi nas variaveis (BSS ja zerada) */
    popl %ebx
    popl %eax
    movl %eax, mb_magic
    movl %ebx, mb_info

    /* PML4[0] -> PDPT (present + writable) */
    movl $pml4_table, %eax
    movl $pdpt_table, %ebx
    movl %ebx, %edx
    orl $0x03, %edx
    movl %edx, (%eax)

    /* PDPT[0..3] -> PD0..PD3 */
    movl $pd_table0, %ecx
    movl %ecx, %edx
    orl $0x03, %edx
    movl %edx, (%ebx)
    movl $pd_table1, %ecx
    movl %ecx, %edx
    orl $0x03, %edx
    movl %edx, 8(%ebx)
    movl $pd_table2, %ecx
    movl %ecx, %edx
    orl $0x03, %edx
    movl %edx, 16(%ebx)
    movl $pd_table3, %ecx
    movl %ecx, %edx
    orl $0x03, %edx
    movl %edx, 24(%ebx)

    /* Cada PD[i] mapeia 1GB em paginas de 2MB: PD[i][j] -> i*1GB + j*2MB */
    movl $0, %esi                /* i = tabela (0..3) */
pd_outer:
    movl %esi, %eax
    shll $30, %eax               /* eax = i * 1GB (parte baixa) */
    movl $0, %edx                /* j = 0..511 */
pd_fill:
    movl %edx, %ecx
    shll $21, %ecx               /* ecx = j * 2MB */
    addl %eax, %ecx
    orl $0x83, %ecx              /* present|rw|ps */
    /* destino = pd_table(i) + j*8; pd_table1 = pd_table0+4096 ... */
    pushl %eax
    movl %esi, %eax
    shll $12, %eax               /* eax = i * 4096 */
    addl $pd_table0, %eax        /* eax = base da PD[i] */
    movl %ecx, (%eax, %edx, 8)
    movl $0, 4(%eax, %edx, 8)
    popl %eax
    incl %edx
    cmpl $512, %edx
    jne pd_fill
    incl %esi
    cmpl $4, %esi
    jne pd_outer

    /* CR3 = PML4 */
    movl $pml4_table, %eax
    movl %eax, %cr3

    /* PAE em CR4 (bit 5) */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    /* LME em EFER (MSR 0xC0000080, bit 8) */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    /* PG em CR0 (bit 31) -> ativa long mode */
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    /* GDT64 + far jump para long mode */
    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

/* Codigo 64-bit */
.code64
.global long_mode_entry
.type long_mode_entry, @function
long_mode_entry:
    /* segmentos de dados (em 64-bit sao quase ignorados) */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* stack */
    lea stack_top(%rip), %rsp
    movq %rsp, %rbp

    /* chama o kernel em C: kmain(magic, mbi) */
    movl mb_info(%rip), %esi
    movl mb_magic(%rip), %edi
    call kmain

    /* se kmain retornar, trava */
halt_loop:
    hlt
    jmp halt_loop

/* Stub generico: salva tudo, chama C, restaura, iretq */
.global default_isr_stub
.type default_isr_stub, @function
default_isr_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    call default_interrupt_handler
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq

.global keyboard_isr_stub
.type keyboard_isr_stub, @function
keyboard_isr_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    call keyboard_handler
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq

.global pit_isr_stub
.type pit_isr_stub, @function
pit_isr_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    call pit_handler
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq

.global mouse_isr_stub
.type mouse_isr_stub, @function
mouse_isr_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    call mouse_handler
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq

.global _load_idt
.type _load_idt, @function
_load_idt:
    lea idt_ptr(%rip), %rax
    lidt (%rax)
    ret

/* void set_idt_entry(uint8_t idx /*%rdi*\/, uint64_t handler /*%rsi*\/, uint8_t type /*%rdx*\/)
 * Layout do gate 64-bit (16 bytes):
 *  0-1: offset[0:15]  2-3: selector  4: ist  5: type_attr
 *  6-7: offset[16:31] 8-11: offset[32:63] 12-15: zero */
.global set_idt_entry
.type set_idt_entry, @function
set_idt_entry:
    pushq %rbp
    movq %rsp, %rbp

    movq %rdi, %rax
    shlq $4, %rax
    lea idt_start(%rip), %r10
    addq %rax, %r10              /* r10 = &idt[idx] */

    movw %si, (%r10)             /* offset 0:15 */
    movw $0x08, 2(%r10)          /* selector = code */
    movb $0, 4(%r10)             /* ist = 0 */
    movb %dl, 5(%r10)            /* type_attr */
    shrq $16, %rsi
    movw %si, 6(%r10)            /* offset 16:31 */
    shrq $16, %rsi
    movl %esi, 8(%r10)           /* offset 32:63 */
    movl $0, 12(%r10)            /* zero */

    movq %rbp, %rsp
    popq %rbp
    ret
