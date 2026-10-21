#!/usr/bin/env python3
# build/fonts.bin (8x8 low, 8x8 high, 8x14, 8x16) -> C arrays for the video ROM
import sys

data = open(sys.argv[1], 'rb').read()
assert len(data) == 9728, len(data)
parts = [('vga_font_8x8', data[:2048]), ('vga_font_8x14', data[2048:2048 + 3584]),
         ('vga_font_8x16', data[2048 + 3584:])]
with open(sys.argv[2], 'w') as f:
    f.write('/* generated from build/fonts.bin by scripts/bin2c.py */\n#include <stdint.h>\n')
    for name, blob in parts:
        f.write('const uint8_t %s[%d] = {\n' % (name, len(blob)))
        for i in range(0, len(blob), 16):
            f.write('\t' + ', '.join('0x%02x' % b for b in blob[i:i + 16]) + ',\n')
        f.write('};\n')
