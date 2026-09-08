/* linker.ld - ELF64 para kernel carregado pelo GRUB (Multiboot1) */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .multiboot : {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(0x1000) {
        *(.text)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata)
    }

    .data : ALIGN(0x1000) {
        *(.data)
    }

    .bss : ALIGN(0x1000) {
        __bss_start = .;
        *(COMMON)
        *(.bss)
    }

    _end = .;
}
