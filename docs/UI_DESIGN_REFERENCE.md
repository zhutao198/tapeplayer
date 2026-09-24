# TapeBook UI 设计参考方案

> 基于 `docs/UI界面参考仅屏幕.jpg`（用户已透视矫正并裁剪为仅屏幕区域），结合本项目硬件（ST7789 2.0" 320×240 TFT）与软件栈（LVGL v9 + ESP32-S3）进行 1:1 复刻设计。

---

## 1. 参考图分析

### 1.1 参考产品

- **产品**：Snowsky ECHO-LL（雪漫天）磁带风格数字播放器
- **屏幕**：彩色 TFT，横屏（实际显示区域约 970:550 = 1.76:1，模拟 320×240）
- **核心视觉元素**（基于矫正后的"仅屏幕"截图）：
  - 顶部状态栏 6 项：扬声器音量 / 曲目计数 / 模式 / 循环图标 / NOR 方框 / 电池
  - 文件名（大号白色，居左）
  - 中央**盒式磁带**（深蓝紫色盒壳，V8 加深色调）：
    - 左右**等大**红色轮毂窗口
    - **跑道形**蓝紫连接条 + **长方形透明窗** + 窗内**磁带卷圈**（左右各一组同心圆弧）
    - 红色轮圈 + 黑色内盘 + 6 个粗壮红色辐条 + 深银金属轴心
  - 格式信息行：`FLAC|44KHZ|16bit|0918kbps` + `SQ` 紫色方块
  - 品牌信息行：`TIME STUDI` + `FiiO`（紫色文字）
  - 底部进度条：▶ 00:00:42 + 绿色填充进度条 + 00:02:27

### 1.2 关键设计原则

1. **盒壳为深蓝紫色**（V8 加深，`#5a6f95` 主色 + `#7d92bf` 高光 + `#3a4868` 暗部），是盒壳+跑道的统一色调。
2. **左右轮毂完全等大**，红色轮圈 + 黑色内盘 + 6 个**粗壮**红色辐条（梯形块，不是细线）。
3. **金属轴心为深银灰色**（不是蓝色），中心有黑色小圆。
4. **中央为跑道形连接条 + 长方形窗 + 磁带圈**：跑道是深蓝紫圆角矩形跨过两个轮毂之间；窗内左右各一组**同心 180° 半弧**（与左右磁带轮同心，圆心在窗外左右两侧，只露出朝向中央的部分）；左右镜像沿 Y 轴对称；中央两条横向细线（磁带主线）。
5. **进度条左半段绿色填充**（`#22c55e`），右半段深灰。
6. 状态栏字体偏小但清晰（白/淡紫）。

### 1.3 不适合直接照搬的元素

| 参考图元素 | 不适合原因 |
|-----------|-----------|
| 细腻的磁带壳纹理、高光、渐变 | LVGL 在 320×240 上绘制复杂渐变/位图开销大 |
| 大面积拟物阴影 | 小屏幕会显得脏乱，且 LVGL 阴影对象占用内存 |
| 复杂横向波形频谱 | 简化为磁带卷圈（同心圆弧 SVG），不渲染实时波形 |
| FiiO / TIME STUDI 等品牌字样 | 本项目是 TapeBook，启动/关于界面再使用 |

---

## 2. 设计目标

1. **复古磁带机识别度高**：一眼看出是"磁带播放器"。
2. **信息密度适中**：在 320×240 内展示 6 项状态栏 + 文件名 + 磁带盒 + 格式行 + 品牌行 + 进度条。
3. **中文可读**：最小中文字号 14px（实际约 10pt）。
4. **LVGL 可高效实现**：仅使用基础对象（`lv_obj`、`lv_arc`、`lv_line`、`lv_bar`、`lv_label`），避免大位图。
5. **动画轻量**：卷轴旋转使用 50ms 定时器，不阻塞音频解码。

---

## 3. 屏幕布局（320×240 横屏）

```
┌──────────────────────────────────────────┐  y=0
│ 🔊30  1/1  FAST-LL  ⟲  NOR  [电池]        │  状态栏 14~36px
├──────────────────────────────────────────┤
│ Epic_Trailer_Theme.flac                  │  文件名 y=54
├──────────────────────────────────────────┤  y=96
│ ┌────────────────────────────────────┐    │
│ │ ╭─────╮  ╱──────╲  ╭─────╮          │    │
│ │ │ ◉   │ │ 磁带圈│ │   ◉ │  磁带盒  │    │  y=96~284
│ │ ╰─────╯  ╲──────╱  ╰─────╯          │    │
│ └────────────────────────────────────┘    │
├──────────────────────────────────────────┤  y=300
│ FLAC|44KHZ|16bit|0918kbps           [SQ] │  格式行
│ 🔧 TIME STUDI            ▶ FiiO          │  品牌行 y=328
├──────────────────────────────────────────┤
│ ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░   │  进度条
│ ▶ 00:00:42                       00:02:27 │  时间行
└──────────────────────────────────────────┘  y=240*2=480
```

### 3.1 区域尺寸

| 区域 | 高度（逻辑像素） | 备注 |
|------|-----------------|------|
| 状态栏 | 22px | 顶部 6 项横向排列 |
| 文件名 | 24px | 白色大号字体 |
| 磁带区 | 188px | 盒壳 + 轮毂 + 中央窗口 |
| 格式行 | 20px | 紫文字 + SQ 方块 |
| 品牌行 | 18px | 双侧紫色品牌字样 |
| 进度条+时间 | 30px | 绿色填充 + 时间显示 |

---

## 4. 视觉规范

### 4.1 颜色

| 用途 | 色值 | 说明 |
|------|------|------|
| 屏幕背景 | `#000000` | 纯黑 |
| 盒壳主色（V8 加深） | `#5a6f95` | 深蓝紫 |
| 盒壳高光 | `#7d92bf` | 顶部高光 |
| 盒壳暗部（V8 加深） | `#3a4868` | 底部阴影 |
| 盒壳边缘 | `#2a3552` | 整体边缘色调 |
| 卷轴红色 | `#e02020` | 主红 |
| 卷轴红暗 | `#b81818` / `#5a0808` | 深红/边缘 |
| 辐条盘 | `#0a0a0a` | 近黑 |
| 金属轴心 | `#d0d0d0` → `#2a2a30` | 深银渐变 |
| 中心黑色圆 | `#0a0a0a` | 螺丝孔 |
| 窗口背景 | `#050810` | 中央窗口深色 |
| 磁带圈半弧 | `#3a4256` | 5 圈同心 180° 半弧，颜色随半径递增透明度 |
| 进度条填充 | `#22c55e` | 亮绿 |
| 进度条背景 | `#1a1f30` | 深灰蓝 |
| 主文字 | `#ffffff` | 文件名、时间 |
| 紫文字 | `#b8a4dc` | 状态栏、格式、品牌行 |
| SQ 方块 | `#7a4ec0` | 紫色背景 |
| 停止/降亮 | `filter:brightness(.5)` | 整体变暗 |

### 4.2 字体层级

| 层级 | 字号 | 用途 |
|------|------|------|
| H1 | 22px | 文件名 |
| H2 | 18px | 状态栏曲目计数、时间显示 |
| H3 | 16px | 模式、格式信息 |
| H4 | 13px | NOR 框字、品牌字样 |

### 4.3 图标规范

全部矢量绘制，不依赖字体图标：

- **扬声器**：SVG path 锥形波
- **循环**：圆环 + 中心双向箭头（CSS border + ::before/::after）
- **NOR**：圆角方框 + 文字
- **电池**：矩形外框 + 正极小柱
- **卷轴**：`lv_obj` 圆形（轮圈）+ 6 个 `lv_obj` 矩形（辐条块）+ `lv_obj` 圆形（金属轴心 + 中心小圆）

---

## 5. 各状态设计

### 5.1 主播放界面

| 状态 | 模式字 | 进度条 | 卷轴 |
|------|--------|-------|------|
| 播放中 | `FAST-LL` / `SEQ-LL` | 绿色填充实时更新 | 双轮匀速正向旋转 |
| 已暂停 | `PAUSED` | 保持当前位置 | 静止 |
| 已停止 | `STOPPED` | 0% | 静止 |
| 快进 | `FF 2.0x` | 绿色填充快速推进 | 双轮快速正向 |
| 快退 | `RW 2.0x` | 绿色填充快速倒退 | 双轮快速反向 |

### 5.2 音量调节

- 状态栏扬声器图标 + 数字（30）
- 调节时实时刷新，3 秒后隐藏数字

### 5.3 文件夹浏览 / 系统设置 / A-B 复读

复用布局：
- 顶部状态栏（仅模式 + NOR + 电池）
- 中央列表 / 磁带盒
- 底部固定功能提示行

### 5.4 启动画面

- 居中显示大磁带盒（简化版本）+ `TAPEBOOK` + 加载提示
- 底部占位时间

### 5.5 错误提示

- 无 SD 卡：`NO SD` 模式 + 灰色文件名 + 暗化磁带盒 + 居中提示
- 无音频文件：`EMPTY` 模式 + 同上

---

## 6. 盒式磁带正面视图设计（V6 - 1:1 复刻）

### 6.1 视觉特征（基于矫正后的"仅屏幕"图）

参考图中 Snowsky ECHO-LL 的磁带图形在 320×240 上**必须按以下特征复现**：

1. **深蓝紫色盒壳**（V8 加深）：`#5a6f95` 主色，圆角矩形 + 顶部高光 + 底部阴影。
2. **左右完全等大轮毂**：红色轮圈 + 黑色内盘 + 6 个粗壮红色辐条 + 深银金属轴心。
3. **跑道融合（V11 花生形/哑铃形）**：把一个圆沿 Y 轴（穿过圆心水平线）劈成两半——左半圆和右半圆保留原位，用两条平行水平线连接切口上端和下端，形成**封闭的扁圆轮廓**（形如花生/哑铃）。左半圆圆心 = 左轮中心 (90,90)，右半圆圆心 = 右轮中心 (450,90)，**半圆半径 R = 80**（比磁带轮 r=75 稍大，留 5px 空隙）；上下两条平行线 y 坐标 = cy±R（即 10 和 170），连接左右半圆切口。**半圆垂直高度 = 平行线之间的距离 = 2R = 160**，整条轨道呈水平扁圆封闭轮廓，半圆鼓出在轮子外侧，上下边是直的。
4. **磁带圈（V8 黑色同心半弧）**：窗内左右各一组同心 180° 半弧，圆心在窗外左右两侧（与左右磁带轮同心），7 圈递增半径（85-115px），颜色为黑色到深灰梯度（`rgb(10-60,10-60,10-60)`），左右镜像沿 Y 轴对称。
5. **不画外露走带机构**——磁带在盒壳内不可见。

### 6.2 尺寸与位置（320×240 逻辑像素）

| 元素 | 尺寸/位置 | 说明 |
|------|----------|------|
| 磁带盒壳 | left=12, top=96, w=296, h=188 | 圆角 18px，浅蓝紫渐变 |
| 左轮毂窗口 | left=18, 居中, 直径 138px | 圆形深色内底 |
| 右轮毂窗口 | right=18, 居中, 直径 138px | 同上 |
| 跑道（V10 双U型，开口对开口） | 540×180px SVG | 左U型圆心(90,90) r=95 开口朝右；右U型圆心(450,90) r=95 开口朝左；条带宽 32px |
| 中央长方形窗 | 居中, 200×50px | 圆角 6px，深色背景 |
| 窗内磁带圈（左/右，V8 黑色半弧） | 120×60px SVG | 7 圈同心 180° 半弧，圆心在窗外左右两侧（cx=0），半径 85-115px，黑色/深灰梯度 |
| 卷轴（红色轮圈） | 直径 126px | 6px 红色边框 |
| 6 辐条块 | 14×36px 矩形 | 围绕中心 60° 等分布置 |
| 金属轴心 | 直径 32px | 深银渐变 + 8px 中心黑圆 |

### 6.3 LVGL 实现建议

由于 LVGL v9 原生不支持 `clip-path: polygon`，长方形窗本身用 `lv_obj` 即可。窗内磁带卷圈用 `lv_canvas` 绘制 SVG 同心圆弧。

```c
/* ---------- 磁带盒壳（浅蓝紫渐变） ---------- */
lv_obj_t *shell = lv_obj_create(parent);
lv_obj_set_size(shell, 296, 188);
lv_obj_set_pos(shell, 12, 96);
lv_obj_set_style_radius(shell, 18, 0);
lv_obj_set_style_bg_color(shell, lv_color_hex(0xaab8d6), 0);
lv_obj_set_style_bg_grad_color(shell, lv_color_hex(0xc4d0e8), 0);
lv_obj_set_style_bg_grad_dir(shell, LV_GRAD_DIR_VER, 0);

/* ---------- 左右轮毂窗口（深色圆孔） ---------- */
static const lv_point_t hub_ctr[2] = {{87, 190}, {233, 190}};  /* 相对屏幕 */
for (int i = 0; i < 2; i++) {
    lv_obj_t *hub_win = lv_obj_create(parent);
    lv_obj_set_size(hub_win, 138, 138);
    lv_obj_set_pos(hub_win, hub_ctr[i].x - 69, hub_ctr[i].y - 69);
    lv_obj_set_style_radius(hub_win, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub_win, lv_color_hex(0x000000), 0);

    /* 红色轮圈 */
    lv_obj_t *reel = lv_obj_create(hub_win);
    lv_obj_set_size(reel, 126, 126);
    lv_obj_center(reel);
    lv_obj_set_style_radius(reel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(reel, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_border_color(reel, lv_color_hex(0xe02020), 0);
    lv_obj_set_style_border_width(reel, 6, 0);

    /* 6 辐条块（旋转动画作用于此容器） */
    lv_obj_t *spokes = lv_obj_create(reel);
    lv_obj_set_size(spokes, 110, 110);
    lv_obj_center(spokes);
    lv_obj_set_style_bg_opa(spokes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(spokes, LV_OPA_TRANSP, 0);
    for (int k = 0; k < 6; k++) {
        lv_obj_t *spoke = lv_obj_create(spokes);
        lv_obj_set_size(spoke, 14, 36);
        lv_obj_align(spoke, LV_ALIGN_CENTER, 0, -25);
        lv_obj_set_style_radius(spoke, 2, 0);
        lv_obj_set_style_bg_color(spoke, lv_color_hex(0xe02020), 0);
        lv_obj_set_style_bg_grad_color(spoke, lv_color_hex(0xb81818), 0);
        lv_obj_set_style_bg_grad_dir(spoke, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_transform_angle(spoke, k * 600, 0);
    }

    /* 金属轴心 */
    lv_obj_t *hub = lv_obj_create(reel);
    lv_obj_set_size(hub, 32, 32);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0x2a2a30), 0);
    lv_obj_set_style_bg_grad_color(hub, lv_color_hex(0xd0d0d0), 0);
    lv_obj_set_style_bg_grad_dir(hub, LV_GRAD_DIR_RADIAL, 0);
    /* 中心黑圆 */
    lv_obj_t *center = lv_obj_create(hub);
    lv_obj_set_size(center, 8, 8);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0x0a0a0a), 0);
}

/* ---------- 中央跑道形连接条（V8 深蓝紫） ---------- */
lv_obj_t *track = lv_obj_create(shell);
lv_obj_set_size(track, 228, 120);
lv_obj_center(track);
lv_obj_set_style_radius(track, 60, 0);  /* 跑道圆角 = 高度一半 */
lv_obj_set_style_bg_color(track, lv_color_hex(0x5a6f95), 0);
lv_obj_set_style_bg_grad_color(track, lv_color_hex(0x7d92bf), 0);
lv_obj_set_style_bg_grad_dir(track, LV_GRAD_DIR_VER, 0);

/* ---------- 长方形透明窗 ---------- */
lv_obj_t *win = lv_obj_create(track);
lv_obj_set_size(win, 172, 74);
lv_obj_center(win);
lv_obj_set_style_radius(win, 6, 0);
lv_obj_set_style_bg_color(win, lv_color_hex(0x050810), 0);
lv_obj_set_style_border_color(win, lv_color_hex(0x0a0d18), 0);
lv_obj_set_style_border_width(win, 2, 0);
lv_obj_set_style_clip_corner(win, true, 0);

/* ---------- 窗内磁带卷圈（V8 同心 180° 半弧，左右镜像） ---------- */
/* 圆心在窗外左右两侧，与左右磁带轮同心；只露 180° 朝向中央的半弧 */
#define COIL_W 80
#define COIL_H 80
static lv_color_t coil_buf[COIL_W * COIL_H];

/* 左磁带圈：圆心在窗外左侧，露 180° 朝向中央的半弧 */
lv_obj_t *coil_canvas_l = lv_canvas_create(win);
lv_canvas_set_buffer(coil_canvas_l, coil_buf, COIL_W, COIL_H, LV_IMG_CF_TRUE_COLOR);
lv_canvas_fill_bg(coil_canvas_l, lv_color_hex(0x050810), LV_OPA_TRANSP);
lv_obj_set_pos(coil_canvas_l, -46, (74-COIL_H)/2);
lv_draw_arc_dsc_t arc_dsc;
lv_draw_arc_dsc_init(&arc_dsc);
arc_dsc.color = lv_color_hex(0x3a4256);
arc_dsc.width = 1;
for (int i = 1; i <= 5; i++) {
    arc_dsc.start_angle = 270;  /* 顶 */
    arc_dsc.end_angle = 90;    /* 底 - 顺时针 180° */
    arc_dsc.opa = 50 + i * 20;  /* 透明度随半径递增 */
    lv_canvas_draw_arc(coil_canvas_l, 32, 64,  /* 圆心在 (32,64) = 窗外左侧 */
                        30 + i*7, &arc_dsc);
}

/* 右磁带圈：圆心在窗外右侧，方向相反 - 左半弧（镜像） */
lv_obj_t *coil_canvas_r = lv_canvas_create(win);
lv_canvas_set_buffer(coil_canvas_r, coil_buf, COIL_W, COIL_H, LV_IMG_CF_TRUE_COLOR);
lv_canvas_fill_bg(coil_canvas_r, lv_color_hex(0x050810), LV_OPA_TRANSP);
lv_obj_align(coil_canvas_r, LV_ALIGN_RIGHT_MID, 46, 0);
for (int i = 1; i <= 5; i++) {
    arc_dsc.start_angle = 270;
    arc_dsc.end_angle = 90;
    arc_dsc.opa = 50 + i * 20;
    lv_canvas_draw_arc(coil_canvas_r, COIL_W-32, 64,  /* 圆心在 (48,64) = 窗外右侧 */
                        30 + i*7, &arc_dsc);
}

/* 窗中央磁带主线（两条横向细线） */
static lv_point_t line_top[2] = {{30, 30}, {172-30, 30}};
static lv_point_t line_bot[2] = {{30, 44}, {172-30, 44}};
lv_obj_t *line1 = lv_line_create(win);
lv_line_set_points(line1, line_top, 2);
lv_obj_set_style_line_color(line1, lv_color_hex(0x506080), 0);
lv_obj_set_style_line_width(line1, 1, 0);
lv_obj_t *line2 = lv_line_create(win);
lv_line_set_points(line2, line_bot, 2);
lv_obj_set_style_line_color(line2, lv_color_hex(0x506080), 0);
lv_obj_set_style_line_width(line2, 1, 0);
```

### 6.4 状态与磁带盒联动

| 播放状态 | 卷轴（6 辐条盘） | 整体磁带盒 |
|---------|------------------|----------|
| 播放中 | 双轮同速正向旋转（50ms/帧，3°/帧） | 正常亮度 |
| 已暂停 | 静止，停在当前角度 | 亮度降至 80% |
| 已停止 | 静止 | 亮度降至 55% |
| 快进 | 双轮快速正向旋转（周期 600ms） | 正常亮度 |
| 快退 | 双轮快速反向旋转（周期 600ms） | 正常亮度 |

---

## 7. 状态栏设计

### 7.1 左侧 4 项

内容：扬声器 + 音量数字 / 曲目计数 / 模式文字 / 循环图标

- **扬声器 + 30**：SVG 矢量 + 数字
- **1/1**：等宽数字
- **FAST-LL**：紫文字，全大写
- **循环图标**：圆环 + 中心双向箭头（CSS 实现）

### 7.2 右侧 2 项

- **NOR 方框**：1.5px 紫色描边 + 文字
- **电池**：白色矩形外框 + 右侧小凸起（无填充指示）

---

## 8. 格式信息 + 品牌行

### 8.1 格式信息行

内容：`FLAC|44KHZ|16bit|0918kbps` + `SQ` 方块

- 紫文字，左对齐
- SQ 方块：紫色背景 `#7a4ec0` + 白字 + 圆角 3px
- 整体一行内分布，使用 `justify-content: space-between`

### 8.2 品牌行

内容：左侧 `🔧 TIME STUDI` + 右侧 `▶ FiiO`

- 紫文字 `#b8a4dc`
- 本项目不显示 FiiO 等品牌字样，改为 TapeBook 相关标记或留空

---

## 9. 底部进度条

内容：▶ 00:00:42 + 进度条 + 00:02:27

- 进度条左半段绿色填充 `#22c55e`（带 glow）
- 右半段深灰 `#1a1f30`
- 圆角 3px
- 高度 6px

---

## 10. 动画与性能

### 10.1 动画列表

| 动画 | 帧率 | 实现方式 | 开销 |
|------|------|---------|------|
| 卷轴 6 辐条旋转 | 20fps (50ms) | `transform_angle` | 低 |
| 文件名滚动 | LVGL 自带 | `LV_LABEL_LONG_SCROLL_CIRCULAR` | 低 |
| 进度条更新 | 1fps | `lv_bar_set_value` | 极低 |

> 窗内磁带圈用 lv_canvas + lv_canvas_draw_arc 绘制同心圆弧，磁带主线用 lv_line 两条横向细线。均为静态对象，无动画开销。

### 10.2 性能约束

- 避免在 `lvgl_task` 中做文件系统或音频解码操作。
- 卷轴旋转使用整数角度，避免浮点三角函数。
- 所有对象在 `ui_create()` 中预先创建。

---

## 11. 与现有代码的衔接

### 11.1 复用部分

| 现有实现 | 复用方式 |
|---------|---------|
| `display.cpp` 的 LVGL 初始化、刷新、锁机制 | 完全保留 |
| 电池/音量/SD 图标 | 保留样式，调整尺寸 |
| 卷轴旋转动画 `reel_anim_cb()` | 保留，调整旋转角度 |
| 进度条、时间、状态文字 | 保留 |

### 11.2 新增/修改部分

| 新增内容 | 实现位置 |
|---------|---------|
| 浅蓝紫盒壳（V6） | 修改 `display.cpp` 中盒壳颜色 |
| 跑道（V10 双U型开口对开口） | 新增 SVG path：两条 stroke-width=32 的 U 型 arc |
| 6 粗辐条轮毂 | 修改 `ui_reel_create()` |
| 窗内磁带圈（V8 同心半弧，左右镜像） | 新增 `lv_canvas` + `lv_canvas_draw_arc` 起止角度 270°→90° |
| 长方形窗 + 磁带主线 | `lv_obj` + `lv_line` 两条横向细线 |
| 状态栏重构 | 调整 `lbl_status` 与图标组位置 |
| 品牌行 + SQ 方块 | 新增 |

---

## 12. 设计原则总结

1. **参考图是标准，不是猜测**：基于用户矫正后的"仅屏幕"图 1:1 复刻。
2. **盒壳浅蓝紫色**：不是深色也不是亮蓝灰。
3. **轮毂完全等大**：左右对称。
4. **跑道形连接 + 长方形窗 + 磁带圈**：跑道是浅蓝紫色圆角矩形跨过两个轮毂之间；窗内左右各一组同心圆弧（表示轮子上卷绕的磁带圈），中央两条横向细线（磁带主线）。这是用户最关心的细节，必须保留。
5. **进度条绿色填充**：关键视觉元素。
6. **磁带必须封闭在盒内**：绝不画外露走带机构。

---

## 13. 附录：参考图特征速查

| 特征 | 参考图表现 | 本项目方案 |
|------|-----------|-----------|
| 盒壳颜色 | 浅蓝紫 | `#aab8d6` → `#c4d0e8` 渐变 |
| 盒壳形状 | 圆角矩形 + 顶部高光 | 圆角 18px + 垂直渐变 |
| 卷轴外观 | 红色粗轮圈 + 黑色内盘 + 6 粗辐条 | 6px 红色边框 + 6 个 14×36 矩形 |
| 金属轴心 | 深银灰色 + 中心黑圆 | `#d0d0d0` → `#2a2a30` + 8px 黑圆 |
| 中央连接 | 跑道形蓝紫色圆角矩形 | `lv_obj` + `border-radius = 高度一半` |
| 中央窗 | 长方形（圆角 6px） | `lv_obj` + 深色背景 |
| 窗内磁带圈 | 左右各一组同心圆弧（5 圈） | `lv_canvas` + `lv_canvas_draw_arc` |
| 磁带主线 | 两条横向细线 | `lv_line` |
| 顶部状态栏 | 6 项布局 | 扬声器+数字 / 曲目 / 模式 / 循环 / NOR / 电池 |
| 格式信息 | FLAC\|44KHZ\|16bit\|0918kbps + SQ | 紫文字 + 紫色 SQ 方块 |
| 品牌行 | TIME STUDI / FiiO | TapeBook（启动/关于界面） |
| 进度条 | ▶ 时间 + 绿色填充 + 总时长 | 同结构，颜色 `#22c55e` |

---

**文档版本**：1.11（V11 花生形跑道 = 半圆 + 平行线封闭曲线）  
**创建日期**：2026-08-26  
**关联文件**：`docs/UI界面参考仅屏幕.jpg`、`docs/ui_preview_v2.html`、`main/display.cpp`  
**修订记录**：
- v1.0：初版，红轮+电平柱状图
- v1.1：卷轴精细化（6 辐条+金属轴心）
- v1.2：等大双卷轴 + 走带机构（用户反馈"像放映机"）
- v1.3：封闭盒式磁带正面视图（无外露走带）
- v1.4：用户矫正全图后基于矫正图复刻
- v1.5：用户回退到 V1.3
- v1.6：用户只保留屏幕区域 1:1 复刻（V6）
- v1.7：跑道形连接条 + 长方形窗 + 磁带圈可见（V7）
- v1.8：深蓝紫加深 + 磁带半弧左右镜像对称（V8）
- v1.9：跑道跨两轮 + 磁带黑色同心（V9）
- v1.10：跑道双U型（V10，用户反馈"越来越怪异"）
- **v1.11：跑道花生形（V11，半圆垂直高度 = 平行线间距 = 2R）**
- **v1.10：跑道改为双U型开口对开口（V10）**
