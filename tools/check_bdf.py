#!/usr/bin/env python3
# 从 simsun.bdf 取出指定 unicode 的 16x16 位图, 与 cjk_font.c 中的字形对比
import re, sys

BDF = r"D:\zhutao\audio_player\main\simsun.bdf"

def get_bdf_glyph(unicode_val):
    txt = open(BDF, encoding="utf-8", errors="ignore").read()
    # 找 ENCODING <val> 的字形块
    pat = re.compile(r"STARTCHAR .*?\nENCODING\s+%d\n(.*?)ENDCHAR" % unicode_val, re.S)
    m = pat.search(txt)
    if not m:
        return None
    block = m.group(1)
    bm = re.search(r"BITMAP\s*(.*?)\nENDCHAR", block, re.S)
    rows = bm.group(1).strip().splitlines()
    # 每个 hex 行是 16 位 (2 字节) -> 取低 16 位
    grid = []
    for line in rows:
        v = int(line.strip(), 16)
        bits = "".join("#" if (v >> (15 - c)) & 1 else "." for c in range(16))
        grid.append(bits)
    return grid

def get_regen_glyph(ch):
    path = "main/cjk_font.c"
    txt = open(path, encoding="utf-8", errors="ignore").read()
    parts = txt.split("const cjk_map_t cjk_unicode_map")
    raw_part = parts[0]
    map_part = ("const cjk_map_t cjk_unicode_map" + parts[1]) if len(parts) > 1 else ""
    rm = re.search(r"cjk_font_raw\[\]\s*=\s*\{(.*?)\};", raw_part, re.S)
    raw = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", rm.group(1))]
    mm = re.search(r"=\s*\{(.*?)\};", map_part, re.S)
    pairs = re.findall(r"\{(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\}", mm.group(1))
    mmap = {int(u, 16): int(i) for u, i in pairs}
    idx = mmap.get(ord(ch), -1)
    if idx < 0:
        return None
    g = raw[idx * 32: idx * 32 + 32]
    grid = []
    for r in range(16):
        b0, b1 = g[r * 2], g[r * 2 + 1]
        v = (b0 << 8) | b1
        grid.append("".join("#" if (v >> (15 - c)) & 1 else "." for c in range(16)))
    return grid

for ch in ["莲", "耀", "睡", "喜", "洋", "空", "之", "城", "天"]:
    u = ord(ch)
    bdf = get_bdf_glyph(u)
    regen = get_regen_glyph(ch)
    print(f"\n===== {ch} (U+{u:04X}) =====")
    print("-- simsun.bdf --")
    if bdf:
        for r in bdf: print("  " + r)
    else:
        print("  (bdf 中无此字)")
    print("-- regen cjk_font.c --")
    if regen:
        for r in regen: print("  " + r)
    else:
        print("  (regen 中无此字)")
    if bdf and regen:
        same = all(a == b for a, b in zip(bdf, regen))
        print(">> 是否一致:", same)
