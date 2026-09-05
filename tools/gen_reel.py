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

S = 64        # 尺寸 (P1-UI: 48->64, 匹配设计稿大轮毂)
FRAMES = 48   # 帧数 (7.5°/帧)

def rgb565(r, g, b):
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5

RED   = (0xE0, 0x20, 0x20)   # 设计稿亮红
RED2  = (0xA0, 0x10, 0x10)   # 深红
BLACK = (0x0A, 0x0A, 0x0A)
HUB_LIGHT = (0xD0, 0xD0, 0xD0)
HUB_DARK  = (0x40, 0x40, 0x48)

def make_reel():
    """画一张红轮毂 RGBA 图 (背景透明), 64px 版。"""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = S / 2
    # 外红圈 (粗环)
    d.ellipse([0, 0, S - 1, S - 1], fill=(*RED, 255))
    # 黑内盘
    inner = 9
    d.ellipse([inner, inner, S - 1 - inner, S - 1 - inner], fill=(*BLACK, 255))
    # 6 条矩形辐条 (红色渐变感: 用粗线)
    spoke_w = 5
    spoke_inner = 14
    spoke_outer = 26
    for i in range(6):
        a = math.radians(i * 60)
        x0 = cx + spoke_inner * math.sin(a); y0 = cy - spoke_inner * math.cos(a)
        x1 = cx + spoke_outer * math.sin(a); y1 = cy - spoke_outer * math.cos(a)
        d.line([(x0, y0), (x1, y1)], fill=(*RED, 255), width=spoke_w)
    # 金属轴心 (径向渐变模拟: 多层圆)
    hub_r = 12
    for r in range(hub_r, 0, -1):
        t = r / hub_r
        cr = int(HUB_LIGHT[0] * (1 - t) + HUB_DARK[0] * t)
        cg = int(HUB_LIGHT[1] * (1 - t) + HUB_DARK[1] * t)
        cb = int(HUB_LIGHT[2] * (1 - t) + HUB_DARK[2] * t)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(cr, cg, cb, 255))
    # 中心螺丝孔
    d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=(*BLACK, 255))
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
    fh.write("/* 自动生成: tools/gen_reel.py (红轮毂 64x64 RGB565A8 透明, %d 帧预渲染, LVGL v9) */\n" % FRAMES)
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
