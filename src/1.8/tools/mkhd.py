#!/usr/bin/env python3
"""mkhd.py - gera imagem HD bootavel do mapple (sem root).

Usa o proprio GRUB como bootloader (boot.img + core + normal.mod),
instalado com grub2-bios-setup sobre arquivo-imagem. O FAT16 e
preenchido com mtools. Resultado: menu grafico mapple (logo+nome)
e boot do kernel+initramfs a partir do HD.

Uso: mkhd.py CORE KERNEL INITRD GRUBDIR THEMETXT BGPNG PF2 OUT [MB]
  CORE     core.img (grub2-mkimage, com prefixo p/ (hd0,msdos1))
  GRUBDIR  /usr/lib/grub/i386-pc (boot.img + *.mod)
"""
import os
import struct
import subprocess
import sys

PART_LBA = 2048


def run(*args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("FALHOU:", " ".join(args))
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        sys.exit(1)
    return r


def main():
    core_p, kern_p, initrd_p, grubdir, themetxt, bgpng, pf2, grubcfg, out = \
        sys.argv[1:10]
    mb = int(sys.argv[10]) if len(sys.argv) > 10 else 128
    total = mb * 1024 * 1024

    with open(out, "wb") as f:
        f.truncate(total)
    with open(out, "r+b") as f:
        # MBR: boot.img do GRUB (kernel_sector=1 por padrao) + tabela
        boot = open(os.path.join(grubdir, "boot.img"), "rb").read()
        assert len(boot) == 512 and boot[510] == 0x55 and boot[511] == 0xAA
        m = bytearray(boot[:440]) + b"\x00" * 6
        nsec = total // 512 - PART_LBA
        e = struct.pack("<B3sB3sII", 0x80, b"\x20\x21\x00", 0x06,
                        b"\xFE\xFF\xFF", PART_LBA, nsec)
        m += e + b"\x00" * 48 + bytes([0x55, 0xAA])
        assert len(m) == 512
        f.seek(0)
        f.write(m)
        # core contiguo a partir do LBA 1
        with open(core_p, "rb") as cf:
            core = cf.read()
        f.seek(512)
        f.write(core)

    off = "@@%d" % (PART_LBA * 512)
    dev = out + off
    run("mformat", "-i", dev, "-H", str(PART_LBA), "-v", "MAPPLE")
    for d in ("::/boot", "::/boot/grub", "::/boot/grub/i386-pc",
              "::/boot/grub/theme", "::/boot/grub/theme/mapple",
              "::/boot/grub/fonts"):
        run("mmd", "-i", dev, d)
    run("mcopy", "-i", dev, kern_p, "::/boot/kernel.elf")
    run("mcopy", "-i", dev, initrd_p, "::/boot/initrd.cpio")

    # modulos GRUB (todos: robusto; ~3MB cabem folgados)
    mods = sorted(f for f in os.listdir(grubdir) if f.endswith(".mod"))
    for m in mods:
        run("mcopy", "-i", dev, os.path.join(grubdir, m),
            "::/boot/grub/i386-pc/" + m)
    for extra in ("boot.img",):
        p = os.path.join(grubdir, extra)
        if os.path.exists(p):
            run("mcopy", "-i", dev, p, "::/boot/grub/i386-pc/" + extra)
    # tema + fonte + cfg
    run("mcopy", "-i", dev, themetxt,
        "::/boot/grub/theme/mapple/theme.txt")
    run("mcopy", "-i", dev, bgpng,
        "::/boot/grub/theme/mapple/bg.png")
    run("mcopy", "-i", dev, pf2,
        "::/boot/grub/fonts/mapple.pf2")
    with open(grubcfg, "r") as f:
        cfg = f.read()
    with open("build/hd-grub.cfg", "w") as f:
        f.write(cfg)
    run("mcopy", "-i", dev, "build/hd-grub.cfg", "::/boot/grub/grub.cfg")

    # boot.img le o core contiguo do LBA 1 (kernel_sector=1 padrao);
    # o core com config embutida nao precisa de mais nada no MBR.
    # (grub2-bios-setup exigiria disco real; aqui o layout e manual.)
    print("hd: %s %dMB mods=%d kernel+initrd+tema ok" %
          (out, mb, len(mods)))


if __name__ == "__main__":
    main()
