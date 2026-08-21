#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 TapeBook UI 用的中文子集 LVGL 字体 (12/14/16 px) + 独立分区用中文 TTF (cjk.ttf)。

1) 子集 C 字体 (ui_font_12/14/16.c)
   - 自动扫描 main/*.cpp / main/*.h 中出现的所有中文字符 + 常用标点 + 可打印 ASCII，
     不再手写字表，彻底解决「菜单项部分字缺字形变灰」问题。
   - bpp=4，保证 UI 文字清晰。

2) cjk.ttf (tools/cjk.ttf)
   - 从 Windows 黑体子集化出全量 CJK 统一表意文字 (U+4E00-U+9FFF) + 常用标点 + ASCII，
     供设备端 freetype 在独立 flash 分区按需渲染任意中文（含 SD 卡上的中文文件名）。

依赖:
  - node + npx  (自动拉取 lv_font_conv，用于子集 C 字体)
  - fonttools   (pip install fonttools，用于 cjk.ttf 子集化)
源字体: Windows 黑体 C:\Windows\Fonts\simhei.ttf
"""
import os
import sys
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_FONT = r"C:\Windows\Fonts\simhei.ttf"

SIZES = [(12, "lv_font_chinese_12"), (14, "lv_font_chinese_14"), (16, "lv_font_chinese_16")]

# 自动抽取时纳入的字符类别
CJK_LO = 0x4E00
CJK_HI = 0x9FFF
FULLWIDTH_LO = 0xFF00
FULLWIDTH_HI = 0xFFEF
CJK_PUNCT = {0x3000, 0x3001, 0x3002, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF08, 0xFF09,
             0xFF5E, 0x2026, 0x300C, 0x300D, 0x3010, 0x3011, 0x00B7}


def scan_source_chars():
    """扫描 main/ 下所有 .cpp/.h，收集需要进入子集字体的中文字符与标点。"""
    chars = set()
    main_dir = os.path.join(ROOT, "main")
    if not os.path.isdir(main_dir):
        return chars
    for fn in sorted(os.listdir(main_dir)):
        if not (fn.endswith(".cpp") or fn.endswith(".h")):
            continue
        path = os.path.join(main_dir, fn)
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                text = f.read()
        except OSError:
            continue
        for ch in text:
            o = ord(ch)
            if CJK_LO <= o <= CJK_HI:
                chars.add(ch)
            elif FULLWIDTH_LO <= o <= FULLWIDTH_HI:
                chars.add(ch)
            elif o in CJK_PUNCT:
                chars.add(ch)
    return chars


def to_ranges(chars):
    """把离散字符集合并为连续区间，减少 lv_font_conv 的 -r 参数数量。"""
    cps = sorted(set(ord(c) for c in chars))
    if not cps:
        return []
    ranges = []
    start = prev = cps[0]
    for c in cps[1:]:
        if c == prev + 1:
            prev = c
        else:
            ranges.append((start, prev))
            start = prev = c
    ranges.append((start, prev))
    return ranges


def build_subset_fonts():
    if not os.path.exists(SRC_FONT):
        raise SystemExit("源字体不存在: " + SRC_FONT)

    chars = scan_source_chars()
    ranges = to_ranges(chars)
    extra = ["-r", "0x20-0x7E"]  # 可打印 ASCII
    for lo, hi in ranges:
        if lo == hi:
            extra += ["-r", "0x%04X" % lo]
        else:
            extra += ["-r", "0x%04X-0x%04X" % (lo, hi)]

    print("子集字体覆盖 %d 个中文字/标点 + ASCII。" % len(chars))
    for size, name in SIZES:
        out = os.path.join(ROOT, "main", "ui_font_%d.c" % size)
        cmd = [
            "npx", "--yes", "lv_font_conv",
            "--font", SRC_FONT,
            "--lv-font-name", name,
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "-o", out,
        ] + extra
        cmd_str = " ".join('"%s"' % c if " " in c else c for c in cmd)
        print(">>", cmd_str)
        subprocess.run(cmd_str, check=True, shell=True)
        # 统一 include 为 "lvgl.h" (与 ESP-IDF LVGL 组件头文件布局一致)
        with open(out, "r", encoding="utf-8") as f:
            data = f.read()
        data = data.replace(
            '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif',
            '#include "lvgl.h"')
        with open(out, "w", encoding="utf-8") as f:
            f.write(data)
        print("   ->", out)
    print("子集字体生成完成。")


def build_cjk_ttf():
    try:
        from fontTools.subset import Subsetter, Options
        from fontTools.ttLib import TTFont
    except ImportError:
        print("[跳过] 未安装 fonttools，无法生成 cjk.ttf（pip install fonttools）。")
        return

    if not os.path.exists(SRC_FONT):
        raise SystemExit("源字体不存在: " + SRC_FONT)

    unicodes = (set(range(0x20, 0x7F))
                | set(range(0x3000, 0x3040))
                | set(range(0xFF00, 0xFFF0))
                | set(range(CJK_LO, CJK_HI + 1)))
    print("生成 cjk.ttf（CJK 全量 + 常用标点 + ASCII）...")
    font = TTFont(SRC_FONT)
    opts = Options()
    subsetter = Subsetter(options=opts)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)
    out = os.path.join(ROOT, "tools", "cjk.ttf")
    font.save(out)
    size_kb = os.path.getsize(out) / 1024.0
    print("   -> %s (%.1f KB)" % (out, size_kb))


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else "all"
    if only in ("all", "subset"):
        build_subset_fonts()
    if only in ("all", "ttf"):
        build_cjk_ttf()
    if only not in ("all", "subset", "ttf"):
        print("用法: gen_font.py [all|subset|ttf]")


if __name__ == "__main__":
    main()
