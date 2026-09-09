#!/usr/bin/env python3
"""Gera boot/grub/theme/mapple/bg.png (1024x768): fundo + logo MG + nome."""
import sys
from PIL import Image, ImageDraw

W, H = 1024, 768
out = sys.argv[1] if len(sys.argv) > 1 else "bg.png"

img = Image.new("RGB", (W, H))
px = img.load()
# gradiente vertical azul-noite
for y in range(H):
    t = y / H
    r = int(8 + 10 * t)
    g = int(10 + 12 * t)
    b = int(26 + 30 * t)
    for x in range(W):
        px[x, y] = (r, g, b)

d = ImageDraw.Draw(img)


def block_text(x, y, s, scale, fg):
    """Texto em blocos (5x7 por glifo, so A-Z 0-9 basicos)."""
    font5x7 = {
        'M': [0x11, 0x1B, 0x15, 0x11, 0x11],
        'G': [0x0E, 0x10, 0x17, 0x11, 0x0E],
        'A': [0x0E, 0x11, 0x1F, 0x11, 0x11],
        'P': [0x1E, 0x11, 0x1E, 0x10, 0x10],
        'L': [0x10, 0x10, 0x10, 0x10, 0x1F],
        'E': [0x1F, 0x10, 0x1E, 0x10, 0x1F],
        '0': [0x0E, 0x11, 0x13, 0x0D, 0x0E],
        '1': [0x04, 0x0C, 0x04, 0x04, 0x0E],
        '3': [0x1E, 0x02, 0x0E, 0x02, 0x1E],
        '.': [0x00, 0x00, 0x00, 0x00, 0x04],
        ' ': [0x00, 0x00, 0x00, 0x00, 0x00],
        'X': [0x11, 0x0A, 0x04, 0x0A, 0x11],
        '8': [0x0E, 0x11, 0x0E, 0x11, 0x0E],
        '6': [0x0E, 0x10, 0x1E, 0x11, 0x0E],
        '4': [0x02, 0x06, 0x0A, 0x1F, 0x02],
        '7': [0x1F, 0x02, 0x04, 0x08, 0x08],
        '2': [0x1E, 0x01, 0x0E, 0x10, 0x1F],
        '5': [0x1F, 0x10, 0x1E, 0x01, 0x1E],
        '9': [0x0E, 0x11, 0x0F, 0x01, 0x0E],
    }
    cx = x
    for ch in s.upper():
        g = font5x7.get(ch, font5x7[' '])
        for col in range(5):
            bits = g[col]
            for row in range(7):
                if bits & (1 << row):
                    d.rectangle([cx + col * scale, y + row * scale,
                                 cx + col * scale + scale - 1,
                                 y + row * scale + scale - 1], fill=fg)
        cx += 6 * scale


# logo MG gigante (verde mapple)
block_text(W // 2 - 6 * 14, 150, "MG", 14, (60, 220, 90))
# nome + versao
block_text(W // 2 - 6 * 5 * 3, 300, "MAPPLE 0.13.0", 5, (235, 235, 245))
block_text(W // 2 - 6 * 5 * 2, 350, "X86_64", 5, (120, 130, 160))

img.save(out)
print("bg:", out)
