#!/usr/bin/env python3
# 对比两个 cjk_font.c 的点阵字形 (用法: verify_font.py <path1> [<path2> ...])
import re, sys

TESTS = ["睡", "莲", "喜", "洋", "空", "之", "城", "天", "耀", "怀"]


def parse_bytes(path):
    txt = open(path, encoding="utf-8", errors="ignore").read()
    # 以 cjk_unicode_map 为界拆分，避免大数组内的括号干扰
    parts = txt.split("const cjk_map_t cjk_unicode_map")
    raw_part = parts[0]
    map_part = ("const cjk_map_t cjk_unicode_map" + parts[1]) if len(parts) > 1 else ""
    rm = re.search(r"cjk_font_raw\[\]\s*=\s*\{(.*?)\};", raw_part, re.S)
    raw = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", rm.group(1))]
    mm = re.search(r"=\s*\{(.*?)\};", map_part, re.S)
    pairs = re.findall(r"\{(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\}", mm.group(1))
    mmap = {int(u, 16): int(i) for u, i in pairs}
    return raw, mmap


def render(g):
    if g is None:
        return ["  (缺字)"]
    rows = []
    for r in range(16):
        b0, b1 = g[r * 2], g[r * 2 + 1]
        v = (b0 << 8) | b1
        rows.append("".join("#" if (v >> (15 - c)) & 1 else "." for c in range(16)))
    return rows


def show(path):
    raw, mmap = parse_bytes(path)
    print(f"\n##### {path}  (glyphs={len(raw)//32}, map={len(mmap)}) #####")
    for ch in TESTS:
        u = ord(ch)
        idx = mmap.get(u, -1)
        g = None if idx < 0 else raw[idx * 32: idx * 32 + 32]
        print(f"--- {ch} (U+{u:04X}) idx={idx} ---")
        for line in render(g):
            print("  " + line)


if __name__ == "__main__":
    paths = sys.argv[1:] or ["main/cjk_font.c"]
    for p in paths:
        show(p)
