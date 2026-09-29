import re
t = open('main/cjk_font.c', encoding='utf-8', errors='replace').read()
m = re.search(r'const cjk_map_t cjk_unicode_map\[\]\s*=\s*\{(.*?)\};', t, re.S)
body = m.group(1)
entries = re.findall(r'\{(0x[0-9A-Fa-f]+),\s*(\d+)\}', body)
print('entries:', len(entries))
prev = -1
bad = []
for i,(u,idx) in enumerate(entries):
    uu = int(u,16)
    if uu < prev:
        bad.append((i, hex(uu), prev))
    prev = uu
print('sorted:', len(bad)==0)
if bad:
    print('first out-of-order at index', bad[0][0])
    for j in range(max(0,bad[0][0]-3), min(len(entries), bad[0][0]+4)):
        print(j, entries[j])
