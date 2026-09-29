#!/usr/bin/env python3
# 直接从 cjk_font.c 抽取指定 Unicode 的字形，逐字打印 ASCII 并做对比。
import sys, re

SRC = sys.argv[1] if len(sys.argv) > 1 else r"d:\zhutao\audio_player\main\cjk_font.c"

txt = open(SRC, encoding="utf-8", errors="replace").read()

# 1) 抽取 cjk_font_raw 字节
m = re.search(r"const uint8_t cjk_font_raw\[\]\s*=\s*\{(.*?)\};", txt, re.S)
raw_bytes = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))

# 2) 抽取 cjk_unicode_map
m2 = re.search(r"const cjk_map_t cjk_unicode_map\[\]\s*=\s*\{(.*?)\};", txt, re.S)
pairs = re.findall(r"\{(0x[0-9A-Fa-f]+),\s*(\d+)\}", m2.group(1))
umap = [(int(u, 16), int(i)) for u, i in pairs]
umap_sorted = sorted(umap, key=lambda x: x[0])
print("map entries:", len(umap), " sorted?", umap == umap_sorted)

# 3) glyph bytes
mb = re.search(r"const int cjk_font_glyph_bytes\s*=\s*(\d+)", txt)
gb = int(mb.group(1)) if mb else 32
print("glyph_bytes:", gb, " raw_len:", len(raw_bytes), " raw_glyphs:", len(raw_bytes)//gb)

def glyph_for_unicode(u):
    for cu, idx in umap:
        if cu == u:
            off = idx * gb
            return idx, raw_bytes[off:off+gb]
    return None, None

def print_glyph(name, u):
    idx, g = glyph_for_unicode(u)
    print(f"\n=== {name} U+{u:04X}  idx={idx} ===")
    if g is None:
        print("  (NOT IN FONT -> 缺字方框)")
        return None
    # 16x16, 每行2字节
    rows = []
    for r in range(16):
        b0 = g[r*2]; b1 = g[r*2+1]
        line = ""
        for j in range(8):
            line += "#" if (b0 & (0x80 >> j)) else "."
        for j in range(8):
            line += "#" if (b1 & (0x80 >> j)) else "."
        rows.append(line)
    print("\n".join(rows))
    return rows

targets = [
    ("睡", 0x7761), ("莲", 0x83B2), ("洋", 0x6D0B), ("空", 0x7A7A),
    ("之", 0x4E4B), ("城", 0x57CE), ("喜", 0x559C), ("耀", 0x8000), ("怀", 0x6000),
]
glyphs = {}
for name, u in targets:
    glyphs[u] = print_glyph(name, u)

# 对比：莲 是否等于 耀？洋 是否等于 怀？
def same(a, b):
    return glyphs.get(a) is not None and glyphs.get(b) is not None and glyphs[a] == glyphs[b]

print("\n--- 关键对比 ---")
print("莲(U+83B2) == 耀(U+8000) ?", same(0x83B2, 0x8000))
print("洋(U+6D0B) == 怀(U+6000) ?", same(0x6D0B, 0x6000))
