#!/usr/bin/env python3
"""生成磁带红色轮毂位图 (带 6 辐条) 的 LVGL 透明背景预渲染帧序列。

为消除播放界面卷轴旋转的实时仿射计算开销 (ESP32-S3 软件渲染卡顿),
本脚本预渲染 N 帧不同角度的静态位图, 动画只需切换 lv_img src (纯 alpha blit, 极快)。

输出 main/reel_img.h:
  - reel_frame_data[F][40*40*3]  (LV_COLOR_FORMAT_RGB565A8: RGB565[w*h] + Alpha[w*h])
  - reel_frame_dsc[F]            (lv_img_dsc_t 数组)
背景完全透明, 旋转时只有红轮毂本身在转, 不会带黑方块。
"""
import math
from PIL import Image, ImageDraw

S = 40        # 尺寸
FRAMES = 24   # 帧数 (每 360/24 = 15° 一帧)

def rgb565(r, g, b):
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5

RED   = (0xD6, 0x45, 0x45)
BLACK = (0x11, 0x13, 0x17)
GREY  = (0x2A, 0x2D, 0x33)

def make_reel():
    """画一张竖直的红轮毂 RGBA 图 (背景透明)。"""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 0, S - 1, S - 1], fill=(*RED, 255))          # 外红圈
    d.ellipse([7, 7, S - 8, S - 8], fill=(*BLACK, 255))        # 黑内盘
    cx = cy = S / 2
    for i in range(6):                                          # 6 条红辐条
        a = math.radians(i * 60)
        x0 = cx + 9 * math.sin(a); y0 = cy - 9 * math.cos(a)
        x1 = cx + 13 * math.sin(a); y1 = cy - 13 * math.cos(a)
        d.line([(x0, y0), (x1, y1)], fill=(*RED, 255), width=3)
    d.ellipse([16, 16, 23, 23], fill=(*GREY, 255))             # 中心 hub
    return img

def to_rgb565a8(img):
    """转 LV_COLOR_FORMAT_RGB565A8 字节流: [RGB565 LE]*N + [Alpha]*N。"""
    px = img.load()
    rgb = []
    alpha = []
    for y in range(S):
        for x in range(S):
            r, g, b, a = px[x, y]
            rgb.append(rgb565(r, g, b))
            alpha.append(a)
    out = []
    for c in rgb:
        out.append(c & 0xFF)
        out.append((c >> 8) & 0xFF)
    out.extend(alpha)
    return out

base = make_reel()
frames_bytes = []
for f in range(FRAMES):
    angle = 360.0 * f / FRAMES
    # 以圆心为中心旋转 (不 expand, 保持 40x40, 透明区补 0)
    rot = base.rotate(-angle, resample=Image.BICUBIC, center=(S / 2, S / 2))
    frames_bytes.append(to_rgb565a8(rot))

with open("main/reel_img.h", "w", encoding="utf-8") as fh:
    fh.write("/* 自动生成: tools/gen_reel.py (红轮毂 40x40 RGB565A8 透明, %d 帧预渲染, LVGL v9) */\n" % FRAMES)
    fh.write("#pragma once\n")
    fh.write("#include \"lvgl.h\"\n")
    fh.write("#define REEL_FRAME_COUNT %d\n" % FRAMES)
    fh.write("#define REEL_W %d\n#define REEL_H %d\n" % (S, S))
    fh.write("static const uint8_t reel_frame_data[%d][%d] = {\n" % (FRAMES, S * S * 3))
    for f in range(FRAMES):
        fh.write("  { /* frame %d */\n" % f)
        b = frames_bytes[f]
        for i in range(0, len(b), 12):
            row = ", ".join("0x%02X" % v for v in b[i:i + 12])
            fh.write("    " + row + ",\n")
        fh.write("  },\n")
    fh.write("};\n")
    fh.write("static const lv_img_dsc_t reel_frame_dsc[%d] = {\n" % FRAMES)
    for f in range(FRAMES):
        fh.write("  { .header = { .cf = LV_COLOR_FORMAT_RGB565A8, .w = %d, .h = %d }, "
                 ".data_size = sizeof(reel_frame_data[%d]), .data = (const uint8_t *)reel_frame_data[%d] },\n"
                 % (S, S, f, f))
    fh.write("};\n")
print("OK: main/reel_img.h (%dx%d RGB565A8 transparent, %d frames)" % (S, S, FRAMES))
