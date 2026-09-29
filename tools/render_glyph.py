import re, sys

def render(idx):
    t = open('main/cjk_font.c', encoding='utf-8', errors='replace').read()
    m = re.search(r'const uint8_t cjk_font_raw\[\]\s*=\s*\{(.*?)\};', t, re.S)
    b = bytes(int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))
    g = b[idx*32 : idx*32+32]
    print("=== idx %d ===" % idx)
    for r in range(16):
        b0, b1 = g[r*2], g[r*2+1]
        line = ''
        for c in range(8):
            line += '#' if (b0 & (0x80 >> c)) else '.'
        for c in range(8):
            line += '#' if (b1 & (0x80 >> c)) else '.'
        print(line)

for idx in (5498, 5077, 4421):
    render(idx)
