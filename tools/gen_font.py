#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 TapeBook UI 用的中文子集 LVGL 字体 (12/14/16 px)。

依赖: node + npx (自动拉取 lv_font_conv)
源字体: Windows 黑体 C:\Windows\Fonts\simhei.ttf
输出:   main/ui_font_12.c / ui_font_14.c / ui_font_16.c
        (符号名 lv_font_chinese_12 / 14 / 16)

仅包含 UI 界面真正用到的汉字 + 可打印 ASCII，体积可控 (~数十 KB/档)。
"""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_FONT = r"C:\Windows\Fonts\simhei.ttf"

SIZES = [(12, "lv_font_chinese_12"), (14, "lv_font_chinese_14"), (16, "lv_font_chinese_16")]

# UI 中出现的全部中文字符（含标点），用于子集化
TEXTS = [
    "播放中", "已暂停", "已停止", "快进中", "快退中", "已锁定",
    "正在播放",
    "顺序", "循环", "单曲",
    "电量", "充电", "音量",
    "上一首", "播放", "停止", "下一首", "快退", "快进",
    "浏览",
    "有声书播放器", "正在加载 SD 卡", "请插入 SD 卡", "未检测到 SD 卡",
    "未找到音频文件", "请将 拷贝到 SD 卡",
    "播放控制", "磁带控制", "选择", "确认", "返回",
    "ESP32S3",
    "：",
]

def build_range():
    chars = set()
    for t in TEXTS:
        for ch in t:
            chars.add(ch)
    # 可打印 ASCII
    ascii_range = "0x20-0x7E"
    # 中文 / 全角标点（仿射到 codepoint）
    cn = sorted(chars, key=lambda c: ord(c))
    cn_range = ",".join("0x%04X" % ord(c) for c in cn)
    return ascii_range, cn_range

def main():
    if not os.path.exists(SRC_FONT):
        raise SystemExit("源字体不存在: " + SRC_FONT)
    ascii_range, cn_range = build_range()
    for size, name in SIZES:
        out = os.path.join(ROOT, "main", "ui_font_%d.c" % size)
        cmd = [
            "npx", "--yes", "lv_font_conv",
            "--font", SRC_FONT,
            "--lv-font-name", name,
            "--size", str(size),
            "-r", ascii_range,
            "-r", cn_range,
            "--bpp", "4",
            "--format", "lvgl",
            "-o", out,
        ]
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
    print("字体生成完成。")

if __name__ == "__main__":
    main()
