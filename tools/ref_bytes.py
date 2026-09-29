import re
t = open('main/cjk_font.c', encoding='utf-8', errors='replace').read()
m = re.search(r'const uint8_t cjk_font_raw\[\]\s*=\s*\{(.*?)\};', t, re.S)
b = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))
for idx, name in [(5498, 'LIAN'), (5077, 'YAO'), (3601, 'YANG'), (4421, 'SHUI'), (1425, 'TIAN')]:
    print(name, 'idx', idx, ' '.join('%02X' % x for x in b[idx*32:idx*32+16]))
