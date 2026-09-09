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
# (fallback; o boot normal usa initrd.cpio como modulo GRUB)
ROOTFS_FILES := $(shell find rootfs -type f | sort)
RAMFS_GEN    := $(BUILD)/ramfs_data.c
INITRD       := $(BUILD)/initrd.cpio

SRCS_C := kernel.c vga.c keyboard.c shell.c pit.c fs.c libc.c edit.c cc.c \
          font.c mouse.c gui.c ata.c pci.c svga.c usb.c audio.c brightness.c \
          initramfs.c install.c
OBJS   := $(BUILD)/boot.o $(BUILD)/kernel.o $(BUILD)/vga.o \
          $(BUILD)/keyboard.o $(BUILD)/shell.o $(BUILD)/pit.o \
          $(BUILD)/fs.o $(BUILD)/libc.o $(BUILD)/edit.o $(BUILD)/cc.o \
          $(BUILD)/font.o $(BUILD)/mouse.o $(BUILD)/gui.o $(BUILD)/ata.o \
          $(BUILD)/pci.o $(BUILD)/svga.o $(BUILD)/cxxsmoke.o \
          $(BUILD)/browser.o $(BUILD)/pcspk.o $(BUILD)/bmp.o \
          $(BUILD)/e1000.o $(BUILD)/net.o $(BUILD)/rtc.o $(BUILD)/drv.o \
          $(BUILD)/hwdetect.o $(BUILD)/ramfs_data.o $(BUILD)/usb.o \
          $(BUILD)/audio.o $(BUILD)/brightness.o $(BUILD)/initramfs.o \
          $(BUILD)/install.o $(BUILD)/payload_data.o

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
	  od -An -tx1 -v "$$f" | sed -E 's/[[:space:]]+([0-9a-f]+)/0x\1,/g' >> $@; \
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

# initramfs: rootfs/ em CPIO newc (o GRUB entrega como modulo).
$(INITRD): $(ROOTFS_FILES) | $(BUILD)
	cd rootfs && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc --quiet > ../$@

$(KERNEL): $(OBJS) linker.ld
	$(LD) -n -T linker.ld -o $@ $(OBJS)

# --- Instalador: payload (kernel+initrd+core+boot) embutido ---
# stage1 = kernel sem payload (com stub); payload_data.c empacota
# stage1+initrd+core+grubfiles p/ o kernel final.
OBJS_CORE := $(filter-out $(BUILD)/payload_data.o,$(OBJS))
PAYLOAD_GEN := $(BUILD)/payload_data.c
GRUB_BOOT_IMG := /usr/lib/grub/i386-pc/boot.img
PAYLOAD_STUB := $(BUILD)/payload_stub.c
STAGE_OBJS := $(OBJS_CORE) $(BUILD)/payload_stub.o

$(PAYLOAD_STUB): | $(BUILD)
	@echo "/* stub p/ stage1 (payload real so no final) */" > $@
	@echo "#include <stdint.h>" >> $@
	@for s in payload_kernel payload_initrd payload_core payload_boot payload_grub; do \
	  echo "const uint8_t $${s}[1] = { 0 };" >> $@; \
	  echo "const uint32_t $${s}_len = 0;" >> $@; \
	done

$(BUILD)/payload_stub.o: $(PAYLOAD_STUB) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# grubfiles.cpio: tudo que o instalador grava no /boot/grub do alvo
# (mods + tema + fonte + cfg). Estrutura interna com prefixo boot/.
GRUB_STAGE := $(BUILD)/grubstage
GRUB_CPIO  := $(BUILD)/grubfiles.cpio

$(GRUB_STAGE): $(THEME_SRC) $(THEME_BG) $(THEME_PF2) boot/grub.cfg.hd | $(BUILD)
	rm -rf $@
	mkdir -p $@/boot/grub/i386-pc $@/boot/grub/theme/mapple $@/boot/grub/fonts
	cp /usr/lib/grub/i386-pc/*.mod $@/boot/grub/i386-pc/
	cp boot/grub.cfg.hd $@/boot/grub/grub.cfg
	cp $(THEME_SRC) $@/boot/grub/theme/mapple/theme.txt
	cp $(THEME_BG) $@/boot/grub/theme/mapple/bg.png
	cp $(THEME_PF2) $@/boot/grub/fonts/mapple.pf2

$(GRUB_CPIO): $(GRUB_STAGE)
	cd $(GRUB_STAGE) && find boot -mindepth 1 | LC_ALL=C sort | cpio -o -H newc --quiet > ../grubfiles.cpio

$(BUILD)/kernel.stage1.elf: $(STAGE_OBJS) linker.ld
	$(LD) -n -T linker.ld -o $@ $(STAGE_OBJS)

$(PAYLOAD_GEN): $(BUILD)/kernel.stage1.elf $(INITRD) $(CORE_IMG) $(GRUB_BOOT_IMG) $(GRUB_CPIO) | $(BUILD)
	@echo "/* Payload do instalador - gerado, nao editar */" > $@
	@echo "#include <stdint.h>" >> $@
	@for f in "$(BUILD)/kernel.stage1.elf:payload_kernel" \
	           "$(INITRD):payload_initrd" \
	           "$(CORE_IMG):payload_core" \
	           "$(GRUB_BOOT_IMG):payload_boot" \
	           "$(GRUB_CPIO):payload_grub"; do \
	  src="$${f%%:*}"; sym="$${f##*:}"; \
	  echo "const uint8_t $${sym}[] = {" >> $@; \
	  od -An -tx1 -v "$$src" | sed -E 's/[[:space:]]+([0-9a-f]+)/0x\1,/g' >> $@; \
	  echo "};" >> $@; \
	  echo "const uint32_t $${sym}_len = (uint32_t)sizeof($${sym});" >> $@; \
	done

$(BUILD)/payload_data.o: $(PAYLOAD_GEN) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Tema GRUB (bootloader grafico: logo + nome).
THEME_SRC  := boot/grub/theme/mapple/theme.txt
THEME_BG   := $(BUILD)/theme-bg.png
THEME_PF2  := $(BUILD)/mapple.pf2
FONT_TTF   := /usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf

$(THEME_BG): tools/mkbg.py | $(BUILD)
	python3 tools/mkbg.py $@

$(THEME_PF2): | $(BUILD)
	grub2-mkfont -o $@ -s 16 $(FONT_TTF)

$(ISO): $(KERNEL) $(INITRD) $(THEME_BG) $(THEME_PF2) check-tools
	$(GRUB_FILE) --is-x86-multiboot $(KERNEL) && echo "MULTIBOOT_OK"
	mkdir -p $(ISO_DIR)/boot/grub/fonts $(ISO_DIR)/boot/grub/theme/mapple
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp $(INITRD) $(ISO_DIR)/boot/initrd.cpio
	cp $(THEME_SRC) $(ISO_DIR)/boot/grub/theme/mapple/theme.txt
	cp $(THEME_BG) $(ISO_DIR)/boot/grub/theme/mapple/bg.png
	cp $(THEME_PF2) $(ISO_DIR)/boot/grub/fonts/mapple.pf2
	printf 'set timeout=3\nset default=0\nset timeout_style=menu\ninsmod all_video\nset gfxmode=1024x768,800x600,auto\ninsmod gfxterm\ninsmod png\ninsmod gfxmenu\ninsmod font\nloadfont /boot/grub/fonts/mapple.pf2\nset theme=/boot/grub/theme/mapple/theme.txt\nexport theme\nterminal_output gfxterm\ninsmod multiboot\nmenuentry "mapple 0.13.0" {\n  multiboot /boot/kernel.elf\n  module /boot/initrd.cpio\n  boot\n}\n' > $(ISO_DIR)/boot/grub/grub.cfg
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

# --- HD: GRUB de verdade (boot.img + core + mods via bios-setup) ---
EARLY_CFG := boot/early.cfg
CORE_IMG  := $(BUILD)/core.img
HD_IMG    := $(BUILD)/hd.img
HD_MB     ?= 128
GRUB_PC   := /usr/lib/grub/i386-pc
GRUB_MODS_HD := biosdisk part_msdos fat normal boot multiboot search \
                search_fs_file echo gfxterm all_video png gfxmenu font \
                configfile ls

$(CORE_IMG): $(EARLY_CFG) | $(BUILD)
	grub2-mkimage -O i386-pc -o $@ -p '(hd0,msdos1)/boot/grub' -c $(EARLY_CFG) $(GRUB_MODS_HD)

$(HD_IMG): $(CORE_IMG) $(KERNEL) $(INITRD) $(THEME_BG) $(THEME_PF2) boot/grub.cfg.hd tools/mkhd.py
	python3 tools/mkhd.py $(CORE_IMG) $(KERNEL) $(INITRD) $(GRUB_PC) \
	  $(THEME_SRC) $(THEME_BG) $(THEME_PF2) boot/grub.cfg.hd $@ $(HD_MB)

hd: $(HD_IMG)

run-hd: $(HD_IMG)
	qemu-system-x86_64 -hda $(HD_IMG) -boot c -m 256 -display gtk -serial none -vga vmware $(QEMU_ACCEL)

clean:
	rm -rf $(BUILD)
