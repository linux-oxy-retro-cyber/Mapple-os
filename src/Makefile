# Makefile - Kernel minimalista x86_64 "mapple"
#
# Uso:
#   make            -> compila tudo e gera build/mapple.iso
#   make iso        -> (re)gera a ISO
#   make run        -> abre o QEMU com janela GTK
#   make run-serial -> roda headless (serial no terminal, 10s)
#   make clean      -> limpa build/

CC      := gcc
CXX     := g++
AS      := as
LD      := ld
BUILD   := build
ISO_DIR := $(BUILD)/iso

# C++ freestanding: sem excecoes/RTTI/threads, sem heap (sem new/delete),
# sem construtores globais (sem CRT: .init_array NAO roda!). So classes
# com init explicito + extern "C" na fronteira com C/Assembly.
CXXFLAGS := $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit

# grub-mkrescue (Fedora: grub2-mkrescue | Debian: grub-mkrescue)
GRUB_MKRESCUE := $(shell command -v grub2-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null)
GRUB_FILE     := $(shell command -v grub2-file 2>/dev/null || command -v grub-file 2>/dev/null)

# Resolucao do framebuffer (ex: make RES=800x600)
RES ?= 1024x768
RES_W := $(word 1,$(subst x, ,$(RES)))
RES_H := $(word 2,$(subst x, ,$(RES)))
ASFLAGS := --64 --defsym RES_W=$(RES_W) --defsym RES_H=$(RES_H)

CFLAGS := -ffreestanding -m64 -nostdlib -nostartfiles -nodefaultlibs \
          -fno-builtin -fno-stack-protector -mno-red-zone \
          -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -O2

# rootfs/ -> empacotado em $(BUILD)/ramfs_data.c (byte arrays + tabela)
ROOTFS_FILES := $(shell find rootfs -type f | sort)
RAMFS_GEN    := $(BUILD)/ramfs_data.c

SRCS_C := kernel.c vga.c keyboard.c shell.c pit.c fs.c libc.c edit.c cc.c \
          font.c mouse.c gui.c ata.c pci.c svga.c
OBJS   := $(BUILD)/boot.o $(BUILD)/kernel.o $(BUILD)/vga.o \
          $(BUILD)/keyboard.o $(BUILD)/shell.o $(BUILD)/pit.o \
          $(BUILD)/fs.o $(BUILD)/libc.o $(BUILD)/edit.o $(BUILD)/cc.o \
          $(BUILD)/font.o $(BUILD)/mouse.o $(BUILD)/gui.o $(BUILD)/ata.o \
          $(BUILD)/pci.o $(BUILD)/svga.o $(BUILD)/cxxsmoke.o \
          $(BUILD)/browser.o $(BUILD)/pcspk.o $(BUILD)/bmp.o \
          $(BUILD)/e1000.o $(BUILD)/net.o $(BUILD)/rtc.o $(BUILD)/drv.o \
          $(BUILD)/hwdetect.o $(BUILD)/ramfs_data.o

KERNEL := $(BUILD)/kernel.elf
ISO    := $(BUILD)/mapple.iso

.PHONY: all iso run run-serial clean check-tools

all: $(ISO)

check-tools:
	@test -n "$(GRUB_MKRESCUE)" || (echo "ERRO: grub-mkrescue/grub2-mkrescue nao encontrado" && exit 1)
	@test -n "$(GRUB_FILE)" || (echo "ERRO: grub-file/grub2-file nao encontrado" && exit 1)
	@command -v qemu-system-x86_64 >/dev/null || (echo "ERRO: qemu-system-x86_64 nao encontrado" && exit 1)

$(BUILD):
	mkdir -p $(BUILD)

# boot.o depende da resolucao (evita header velho apos make RES=...).
$(BUILD)/res.txt: | $(BUILD)
	@echo "$(RES)" > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm $@.tmp; fi

$(BUILD)/boot.o: boot.s $(BUILD)/res.txt | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/ramfs_data.o: $(RAMFS_GEN) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Lista de diretorios (p/ ramfs incluir dirs vazios)
ROOTFS_DIRS := $(shell find rootfs -mindepth 1 -type d | sort)

# Gera C a partir de rootfs/: um array por arquivo + tabela nome/dados/tamanho
$(RAMFS_GEN): $(ROOTFS_FILES) | $(BUILD)
	@echo "/* Gerado pelo Makefile a partir de rootfs/ - nao editar */" > $@
	@echo "#include <stdint.h>" >> $@
	@echo "typedef struct ramfs_entry {" >> $@
	@echo "    const char *name;" >> $@
	@echo "    const uint8_t *data;" >> $@
	@echo "    uint32_t size;" >> $@
	@echo "} ramfs_entry_t;" >> $@
	@n=0; \
	for f in $(ROOTFS_FILES); do \
	  name="/$${f#rootfs/}"; \
	  echo "static const uint8_t ramfs_f$$n[] = {" >> $@; \
	  od -An -tx1 "$$f" | sed -E 's/[[:space:]]+([0-9a-f]+)/0x\1,/g' >> $@; \
	  echo "};" >> $@; \
	  echo "static const char ramfs_n$$n[] = \"$$name\";" >> $@; \
	  n=$$((n+1)); \
	done; \
	echo "const ramfs_entry_t ramfs_table[] = {" >> $@; \
	n=0; \
	for f in $(ROOTFS_FILES); do \
	  echo "  { ramfs_n$$n, ramfs_f$$n, (uint32_t)sizeof(ramfs_f$$n) }," >> $@; \
	  n=$$((n+1)); \
	done; \
	echo "};" >> $@; \
	echo "const uint32_t ramfs_count = $$n;" >> $@; \
	echo "const char *ramfs_dirs[] = {" >> $@; \
	for d in $(ROOTFS_DIRS); do \
	  name="/$${d#rootfs/}"; \
	  echo "  \"$$name\"," >> $@; \
	done; \
	echo "};" >> $@; \
	echo "const uint32_t ramfs_ndirs = sizeof(ramfs_dirs)/sizeof(ramfs_dirs[0]);" >> $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) -n -T linker.ld -o $@ $(OBJS)

$(ISO): $(KERNEL) check-tools
	$(GRUB_FILE) --is-x86-multiboot $(KERNEL) && echo "MULTIBOOT_OK"
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	printf 'set timeout=0\nset default=0\nmenuentry "mapple" {\n  multiboot /boot/kernel.elf\n  boot\n}\n' > $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

iso: $(ISO)

# Aceleracao KVM quando disponivel (10-50x mais rapido que TCG).
QEMU_ACCEL := $(shell test -c /dev/kvm && echo "-accel kvm" || echo "")
$(info QEMU accel: $(if $(QEMU_ACCEL),kvm (rapido),tcg (lento: sem /dev/kvm)))

# Janela local com GPU virtual VMware (aceleracao + cursor HW).
# Headless/CI continua com VGA padrao (fallback CPU testado).
run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -boot d -m 256 -display gtk -serial none -vga vmware $(QEMU_ACCEL)

run-serial: $(ISO)
	timeout 10 qemu-system-x86_64 -cdrom $(ISO) -boot d -m 256 \
		-display none -serial stdio -no-reboot || true

clean:
	rm -rf $(BUILD)
