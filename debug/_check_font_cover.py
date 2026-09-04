# -*- coding: utf-8 -*-
"""校验点阵字库 cjk_font.c 对 UI 实际用到字符的覆盖率。
用法: python debug/_check_font_cover.py
"""
import re
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_font(src_path):
    src = open(src_path, encoding='utf-8', errors='replace').read()
    m = re.search(
        r'const\s+cjk_map_t\s+cjk_unicode_map\[\]\s*=\s*\{(.*?)\}\s*;', src, re.S)
    if not m:
        print("ERROR: 未找到 cjk_unicode_map 定义")
        return set(), 0
    # 注意: 表项形如 {0x0020, 0}, —— unicode 带 0x, idx 为十进制(无 0x)
    pairs = re.findall(
        r'\{\s*0x([0-9A-Fa-f]+)\s*,\s*(?:0x)?([0-9A-Fa-f]+)\s*\}', m.group(1))
    chars = set(int(u, 16) for u, _ in pairs)
    # 顺带取字形尺寸/字节数
    gw = re.search(r'const\s+int\s+cjk_font_w\s*=\s*(\d+)', src)
    gh = re.search(r'const\s+int\s+cjk_font_h\s*=\s*(\d+)', src)
    gb = re.search(r'const\s+int\s+cjk_font_glyph_bytes\s*=\s*(\d+)', src)
    print("字库: %d 个字符,  %sx%s,  %s bytes/glyph" % (
        len(chars),
        gw.group(1) if gw else '?',
        gh.group(1) if gh else '?',
        gb.group(1) if gb else '?'))
    return chars, len(pairs)


def strip_comments(s):
    """剥离 C/C++ 注释, 避免把注释里的中文误当作 UI 文案统计。"""
    out = []
    i, n = 0, len(s)
    state = 'code'          # code | line | block | str | chr
    while i < n:
        c = s[i]
        nxt = s[i + 1] if i + 1 < n else ''
        if state == 'code':
            if c == '/' and nxt == '/':
                state = 'line'; i += 2; continue
            if c == '/' and nxt == '*':
                state = 'block'; i += 2; continue
            if c == '"':
                state = 'str'; out.append(c); i += 1; continue
            if c == "'":
                state = 'chr'; out.append(c); i += 1; continue
            out.append(c); i += 1; continue
        if state == 'line':
            if c == '\n':
                state = 'code'; out.append(c)
            i += 1; continue
        if state == 'block':
            if c == '*' and nxt == '/':
                state = 'code'; i += 2; out.append(' '); continue
            if c == '\n':
                out.append('\n')
            i += 1; continue
        if state == 'str':
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(s[i + 1]); i += 2; continue
            if c == '"':
                state = 'code'
            i += 1; continue
        if state == 'chr':
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(s[i + 1]); i += 2; continue
            if c == "'":
                state = 'code'
            i += 1; continue
    return ''.join(out)


STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def collect_used(only_menu=False):
    """只统计真正会显示出来的字符串字面量中的字符(已剥离注释)。"""
    used = {}
    if only_menu:
        files = [os.path.join(ROOT, 'main', 'menu.cpp'),
                 os.path.join(ROOT, 'main', 'menu.h')]
    else:
        files = sorted(glob.glob(os.path.join(ROOT, 'main', '*.cpp')) +
                       glob.glob(os.path.join(ROOT, 'main', '*.h')) +
                       glob.glob(os.path.join(ROOT, 'main', '*.c')))
    for f in files:
        base = os.path.basename(f)
        if base == 'cjk_font.c' or base.startswith('_tmp_'):
            continue  # 字库自身数据 / 临时文件, 非 UI 文案
        try:
            raw = open(f, encoding='utf-8', errors='replace').read()
        except Exception:
            continue
        for lit in STR_RE.findall(strip_comments(raw)):
            for ch in lit:
                o = ord(ch)
                if o < 0x80:
                    continue  # ASCII 一般 LVGL 自带字体可画, 且不一定在点阵库
                used.setdefault(ch, set()).add(base)
    return used


def main():
    font_path = os.path.join(ROOT, 'main', 'cjk_font.c')
    if not os.path.exists(font_path):
        print("ERROR: 找不到 main/cjk_font.c")
        return 1
    font, npairs = parse_font(font_path)

    def report(used, title, show_detail):
        print("\n=== %s ===" % title)
        print("字符串字面量中的非 ASCII 字符种类: %d" % len(used))
        missing = sorted([c for c in used if ord(c) not in font])
        if not missing:
            print("[OK] 全部 %d 个字符字库均已覆盖。" % len(used))
            return []
        print("[缺失] %d 个字符字库中没有 (会显示为空白/方框):" % len(missing))
        print("   " + "".join(missing))
        if show_detail:
            for c in missing:
                print("    U+%04X %s  <- %s" % (
                    ord(c), c, ",".join(sorted(used[c]))))
        return missing

    # 1) 菜单专项 (主攻目标)
    menu_used = collect_used(only_menu=True)
    menu_missing = report(menu_used, "菜单文案 (menu.cpp/.h) -- 主攻目标",
                          show_detail=True)
    # 2) 全 UI 概览
    all_used = collect_used(only_menu=False)
    report(all_used, "全部 UI 文案 (main/*.cpp|h|c)", show_detail=False)

    # 常用标点/ASCII 覆盖抽查
    probe = list("0123456789:.%-/>»● ")
    have = [c for c in probe if ord(c) in font]
    print("\nASCII/符号抽查: %s" % " ".join(probe))
    print("  字库中已含: %s" % (" ".join(have) if have else "(无)"))

    # 字库样例
    sample = sorted(font)
    if sample:
        lo = [c for c in sample if c < 0x100]
        han = [c for c in sample if 0x4E00 <= c <= 0x9FFF]
        print("\n字库区间: 最小 U+%04X, 最大 U+%04X" % (sample[0], sample[-1]))
        print("  含 ASCII/符号段: %d 个" % len(lo))
        print("  含汉字: %d 个" % len(han))
        if han:
            print("  汉字样例: %s" % "".join(chr(c) for c in han[:60]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
