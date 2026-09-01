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


def _parse_tmp(font_c):
    """解析 lv_font_conv 生成的单个 .c，返回 (glyphs, bitmap_bytes)。
    glyphs: list of (unicode, bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y)
    其中 unicode 来自 cmaps，bitmap_index 为批内偏移。"""
    import re
    # 提取 glyph_dsc 条目
    dsc_re = re.compile(
        r"\.bitmap_index\s*=\s*(-?\d+),\s*\.adv_w\s*=\s*(-?\d+),\s*"
        r"\.box_w\s*=\s*(-?\d+),\s*\.box_h\s*=\s*(-?\d+),\s*"
        r"\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)")
    dsc_block = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\};", font_c, re.S).group(1)
    dscs = dsc_re.findall(dsc_block)
    # glyph_id 0 是保留(reserved)，从 1 开始有效
    # 提取 cmaps 得到 unicode -> glyph_id
    cmap_block = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\};", font_c, re.S).group(1)
    cmap_re = re.compile(
        r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*"
        r"\.glyph_id_start\s*=\s*(\d+)")
    unicode_to_gid = {}
    for rs, rl, gs in cmap_re.findall(cmap_block):
        rs, rl, gs = int(rs), int(rl), int(gs)
        for i in range(rl):
            unicode_to_gid[rs + i] = gs + i
    # 提取 bitmap 字节
    bmp_block = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", font_c, re.S).group(1)
    bytes_hex = re.findall(r"0x([0-9A-Fa-f]{2})", bmp_block)
    bitmap = bytes(int(h, 16) for h in bytes_hex)
    # 组装 glyph 列表（跳过 gid 0 保留项）
    glyphs = []
    for gid, (bi, aw, bw, bh, ox, oy) in enumerate(dscs):
        if gid == 0:
            continue
        uni = None
        for u, g in unicode_to_gid.items():
            if g == gid:
                uni = u
                break
        if uni is None:
            continue
        glyphs.append((uni, int(bi), int(aw), int(bw), int(bh), int(ox), int(oy)))
    return glyphs, bitmap


def build_subset_fonts():
    if not os.path.exists(SRC_FONT):
        raise SystemExit("源字体不存在: " + SRC_FONT)

    chars = scan_source_chars()
    cps = sorted(set(ord(c) for c in chars))
    # 可打印 ASCII 始终纳入
    ascii_cps = list(range(0x20, 0x7F))
    all_cps = sorted(set(ascii_cps) | set(cps))

    n = len(all_cps)
    print("子集字体覆盖 %d 个字符(含 ASCII)。分批调用 lv_font_conv 生成..." % n)
    for size, name in SIZES:
        _build_one_size(size, name, all_cps)
    print("子集字体生成完成。")


def _build_one_size(size, name, all_cps):
    """对单个字号分批生成并合并输出最终 .c。"""
    import re
    tmp_dir = os.path.join(ROOT, "main")
    BATCH = 160
    n = len(all_cps)
    tmp_files = []
    try:
        batch_idx = 0
        for i in range(0, n, BATCH):
            chunk = all_cps[i:i + BATCH]
            s = p = chunk[0]
            ranges_arg = []
            for c in chunk[1:]:
                if c == p + 1:
                    p = c
                else:
                    ranges_arg.append((s, p))
                    s = p = c
            ranges_arg.append((s, p))
            extra = []
            for lo, hi in ranges_arg:
                if lo == hi:
                    extra += ["-r", "0x%04X" % lo]
                else:
                    extra += ["-r", "0x%04X-0x%04X" % (lo, hi)]
            tmp = os.path.join(tmp_dir, "_tmp_%d.c" % batch_idx)
            cmd = ["npx", "--yes", "lv_font_conv",
                   "--font", SRC_FONT, "--lv-font-name", "lv_font_tmp",
                   "--size", str(size), "--bpp", "4", "--format", "lvgl",
                   "--no-compress", "-o", tmp] + extra
            subprocess.run(cmd, check=True, shell=True)
            tmp_files.append(tmp)
            batch_idx += 1

        merged = {}
        full_bitmap = bytearray()
        for tmp in tmp_files:
            with open(tmp, "r", encoding="utf-8") as f:
                data = f.read()
            glyphs, bitmap = _parse_tmp(data)
            for (uni, bi, aw, bw, bh, ox, oy) in glyphs:
                nbytes = (bw * bh * 4 + 7) // 8
                glyph_bmp = bitmap[bi:bi + nbytes]
                new_index = len(full_bitmap)
                full_bitmap += glyph_bmp
                merged[uni] = (new_index, aw, bw, bh, ox, oy)

        sorted_uni = sorted(merged.keys())
        cmap_ranges = []
        s = p = sorted_uni[0]
        for u in sorted_uni[1:]:
            if u == p + 1:
                p = u
            else:
                cmap_ranges.append((s, p))
                s = p = u
        cmap_ranges.append((s, p))

        # 组装 .c 文本
        lines = []
        lines.append('#include "lvgl.h"')
        lines.append("")
        lines.append("/* Auto-generated by tools/gen_font.py (subset, PLAIN bpp4, LVGL v9 compatible) */")
        lines.append("/* Source: %s  size: %d */" % (SRC_FONT, size))
        lines.append("")
        lines.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
        lines.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
        for u in sorted_uni:
            bi, aw, bw, bh, ox, oy = merged[u]
            lines.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d},"
                         % (bi, aw, bw, bh, ox, oy))
        lines.append("};")
        lines.append("")
        # bitmap
        lines.append("static const uint8_t glyph_bitmap[] = {")
        bmp = bytes(full_bitmap)
        for i in range(0, len(bmp), 16):
            chunk = bmp[i:i + 16]
            lines.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
        lines.append("};")
        lines.append("")
        # cmaps
        lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] =")
        lines.append("{")
        for (lo, hi) in cmap_ranges:
            gid_start = sorted_uni.index(lo) + 1  # gid 1-based
            lines.append("    {")
            lines.append("        .range_start = %d, .range_length = %d, .glyph_id_start = %d,"
                         % (lo, hi - lo + 1, gid_start))
            lines.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
            lines.append("    },")
        lines.append("};")
        lines.append("")
        lines.append("#if LVGL_VERSION_MAJOR >= 8")
        lines.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
        lines.append("#else")
        lines.append("static lv_font_fmt_txt_dsc_t font_dsc = {")
        lines.append("#endif")
        lines.append("    .glyph_bitmap = glyph_bitmap,")
        lines.append("    .glyph_dsc = glyph_dsc,")
        lines.append("    .cmaps = cmaps,")
        lines.append("    .kern_dsc = NULL,")
        lines.append("    .kern_scale = 0,")
        lines.append("    .cmap_num = %d," % len(cmap_ranges))
        lines.append("    .bpp = 4,")
        lines.append("    .kern_classes = 0,")
        lines.append("    .bitmap_format = 0,")
        # R098h: LVGL v9 的 lv_font_fmt_txt_dsc_t 比 v8 多一个 `uint8_t stride;`
        # 字段，控制位图每行字节对齐。lv_font_conv 默认 stride=1（bpp=4 时每行
        # 对齐到 1 字节），必须显式写出，否则默认 0 导致行错位、屏幕乱码。
        lines.append("    .stride = 1,")
        lines.append("};")
        lines.append("")
        lines.append("#if LVGL_VERSION_MAJOR >= 8")
        lines.append("const lv_font_t %s = {" % name)
        lines.append("#else")
        lines.append("lv_font_t %s = {" % name)
        lines.append("#endif")
        lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
        lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
        # line_height / base_line 近似：line_height 略大于字号，base_line 约等于 0
        lines.append("    .line_height = %d," % int(size * 1.1))
        lines.append("    .base_line = 0,")
        lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
        lines.append("    .underline_thickness = 0,")
        lines.append("    .underline_position = 0,")
        lines.append("    .dsc = &font_dsc,")
        lines.append("    .fallback = NULL,")
        lines.append("    .user_data = NULL,")
        lines.append("};")
        lines.append("")

        out = os.path.join(tmp_dir, "ui_font_%d.c" % size)
        with open(out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        print("   ->", out, "(%d glyphs, %d bytes bitmap)" % (len(sorted_uni), len(bmp)))
    finally:
        for tmp in tmp_files:
            try:
                os.remove(tmp)
            except OSError:
                pass


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
