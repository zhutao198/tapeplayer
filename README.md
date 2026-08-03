# ESP32-S3 听书机 (Tape-Style Audiobook Player)

基于 **ESP32-S3-WROOM-2** 模块的磁带机风格听书机。支持主流音频格式，快进/快退模拟传统磁带机体验。

> **模块说明**：ESP32-S3-WROOM-2 是乐鑫推出的 S3 通用型 Wi-Fi + BLE 模组，搭载 **Octal SPI Flash + Octal PSRAM**（双 Octal 总线架构），相比 WROOM-1 提供更大的存储与更高的带宽。
>
> **可用料号（依官方数据手册 v1.7）**：
> - ✅ **ESP32-S3-WROOM-2-N32R16V**（32MB Flash / 16MB Octal PSRAM）— 当前主推
> - ⚠️ ESP32-S3-WROOM-2-N16R8V（16MB Flash / 8MB Octal PSRAM）— 已停产（EOL）
> - ⚠️ ESP32-S3-WROOM-2-N32R8V（32MB Flash / 8MB Octal PSRAM）— 已停产（EOL）
>
> **关键设计注意点**：
> - 33 个可用 GPIO（41 个引脚中 33 个为 GPIO）
> - **GPIO47/GPIO48 工作在 1.8V 电压域**（VDD_SPI），需电平转换才能接 3.3V 设备
> - GPIO0/3/45/46 为 Strapping 引脚，**默认慎用**（影响启动模式）

---

## 硬件清单

| 组件 | 型号 | 数量 | 用途 |
|------|------|------|------|
| 主控模块 | ESP32-S3-WROOM-2 (N32R16V / Octal PSRAM) | 1 | 音频解码 + 系统控制 |
| DAC/功放 | MAX98357 I2S 模块 | 1 | 3W 音频输出，直接驱动喇叭 |
| 存储 | MicroSD 卡模块 (SPI) | 1 | 存放音频文件 |
| 显示屏 | ST7789 2.0寸 SPI TFT (240×320) | 1 | 显示曲目/进度/状态（LVGL 界面） |
| 按键 | 6×6 轻触开关 × 6 + 拨轮开关(LCK-TG001A-G1) × 1 | 7 | 控制按键 + 音量± |
| 喇叭 | 3W 4Ω/8Ω | 1 | 音频输出 |
| 面包板 + 杜邦线 | - | 若干 | 连接 |

## 连线方案

```
ESP32-S3                MAX98357
----------              ----------
GPIO4  (I2S_BCK)   →    BCLK
GPIO5  (I2S_WS)    →    LRC
GPIO6  (I2S_DOUT)  →    DIN
GPIO7  (I2S_MCLK)  →    可选 (如模块支持)
3.3V               →    VIN
GND                →    GND
                    →    SD_MODE (接VDD = L+R混合单声道输出)

ESP32-S3                MicroSD (SPI)
----------              -------------
GPIO10 (CS)        →    CS
GPIO11 (MOSI)      →    MOSI (DI)
GPIO12 (MISO)      →    MISO (DO)
GPIO13 (SCK)       →    SCK (CLK)
3.3V               →    VCC
GND                →    GND

ESP32-S3                ST7789 TFT (SPI3, 独立于 SD 卡的 SPI2)
----------              --------------------------------------
GPIO18 (MOSI)      →    SDA
GPIO8  (SCK)       →    SCL
GPIO16 (DC)        →    DC
GPIO17 (RES)       →    RES
GPIO15 (PWM)       →    BLK (背光)
GPIO39 (LCD_POW_EN)→    PMOS 电源开关（低电平上电）
（CS 硬件下拉 GND 恒选，不占 GPIO）
3.3V               →    VCC
GND                →    GND

ESP32-S3                按键 (共8个: 6 直键 + 2 拨轮 GPIO, 共用 GND)
----------              ----------
GPIO9  → 播放/暂停      (RTC GPIO ✅ 可唤醒)
GPIO14 → 停止           (RTC GPIO ✅ 可唤醒)
GPIO21 → 上一首         (RTC GPIO ✅ 可唤醒)
GPIO47 → 下一首
GPIO42 → 快退 (按住倒带, 松开恢复)
GPIO41 → 快进 (按住快进, 松开恢复)
GPIO0  → 音量- (LCK 左拨, 经 10kΩ 上拉到 3.3V; Strapping 引脚)
GPIO3  → 音量+ (LCK 右拨, 经 10kΩ 上拉到 3.3V; Strapping 引脚)

所有按键另一端全部接 GND。
ESP32 内部上拉, 按下时读低电平。
LCK-TG001A-G1 拨轮开关的「下按」(C 向)为纯机械电源开关, 不进 GPIO。
```

## 磁带机式快进/快退设计

这是本项目的核心亮点，模拟传统磁带机的操作体验：

### 快进模式 (Fast Forward)
```
按下快进键 → 播放速度逐步加速:
  0.0s ─ 0.8s:  1.0x (缓冲期)
  0.8s ─ 2.0s:  1.5x (有轻微变调声)
  2.0s ─ 4.0s:  2.5x (叽叽喳喳的快放声)
  4.0s ─ 7.0s:  4.0x (快速扫描)
  7.0s+:         8.0x (极速)

松开快进键 → 立即从新位置恢复 1.0x 正常播放
```

### 快退模式 (Rewind)
```
按下快退键 → 倒放速度逐步加速（档位同上）
松开快退键 → 立即从新位置恢复 1.0x 正常播放
```

### 技术实现
- **跳帧式 seek**：每 50ms 根据速度计算跳帧距离，调用解码器 seek
- **I2S 变速**：调整 I2S 采样率实现变调（产生磁带机特有的"变声"效果）
- **逐级加速**：基于按住时长自动切换档位

## 音频格式支持

| 格式 | 扩展名 | 解码器 | 备注 |
|------|--------|--------|------|
| MP3 | .mp3 | libmad | 最常见的格式 |
| AAC | .aac, .m4a | AAC decoder | 苹果常用格式 |
| WAV | .wav | PCM pass-through | 无损，文件较大 |
| FLAC | .flac | libFLAC | 无损压缩 |
| OGG Vorbis | .ogg | libvorbis | 开源格式 |
| Opus | .opus | libopus | 高效有损格式 |

## 目录结构

```
audio_player/
├── main/
│   ├── CMakeLists.txt          # 组件构建配置
│   ├── main.cpp                # 程序入口 + 主循环
│   ├── config.h                # 引脚定义 + 配置常量
│   ├── button_manager.h/cpp    # 按键扫描 + 状态机去抖
│   ├── tape_control.h/cpp      # 磁带机式快进快退控制
│   ├── playlist.h/cpp          # 播放列表管理
│   ├── display.h/cpp           # TFT 显示模块（原生 esp_lcd ST7789 + LVGL v9）
│   ├── lv_conf.h               # LVGL 配置（RGB565 / 16 位色深）
│   └── audio_player.h/cpp      # 音频播放引擎
├── partitions.csv              # 分区表
├── sdkconfig.defaults          # SDK 默认配置
├── CMakeLists.txt              # 顶层 CMake
└── README.md
```

## 开发环境搭建

### 1. 安装 ESP-IDF (v5.1+)

**Windows (推荐离线安装器)**：
下载地址: https://dl.espressif.com/dl/esp-idf/
选择 ESP-IDF v5.3 离线安装器，安装时勾选 ESP32-S3 支持。

**Linux/Mac**：
```bash
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.3
cd esp-idf
./install.sh esp32s3
. ./export.sh
```

### 2. 安装 ESP-ADF (Audio Development Framework)

```bash
cd ~/esp
git clone --recursive https://github.com/espressif/esp-adf.git -b v2.7
```

设置环境变量：
```bash
export ADF_PATH=~/esp/esp-adf
```

或在项目 `CMakeLists.txt` 中取消注释：
```cmake
set(EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components)
```

### 3. 编译与烧录

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 配置项目 (启用 ESP-ADF, 配置 SD 卡引脚等)
idf.py menuconfig

# 编译
idf.py build

# 烧录并查看日志
idf.py -p COM3 flash monitor
```

### 4. menuconfig 关键配置项

```
Component config →
  Audio HAL → 启用
  FAT Filesystem support → 启用 Long filename support

Serial flasher config →
  Flash size → 16 MB (ESP32-S3 通常有 16MB)

Partition Table →
  选择自定义分区表 (partitions.csv)
```

## 功能流程

```
上电
 │
 ├── 初始化 NVS, GPIO, I2C, SPI
 ├── 显示启动画面
 ├── 挂载 SD 卡
 ├── 扫描音频文件 → 构建播放列表
 │
 └── 进入主循环 (每 20ms 一次):
      │
      ├── 扫描按键事件:
      │   ├── 播放/暂停: 切换播放/暂停
      │   ├── 停止: 安全停止（保留当前位置，下次播放从原位置续播，误触不丢进度）
      │   ├── 上一首/下一首: 切换曲目
      │   ├── 快进按下: 进入加速模式
      │   ├── 快进松开: 恢复正常速度
      │   ├── 快退按下: 进入倒放加速
      │   └── 快退松开: 恢复正常速度
      │
      ├── 磁带控制 tick (档位自动升级)
      ├── 音频播放器 tick (跳帧 seek)
      │
      └── 刷新 TFT 显示 (LVGL, 数据每200ms更新):
          显示曲目名 / 进度条 / 时间 / 速度 / 音量
```

## 断点续播

播放位置自动保存到 ESP32 内部 NVS，**停止 / 暂停 / 关机 / 唤醒统一为续播模型**（磁带机式体验）：
- 停止播放 → 保存当前文件和秒数偏移；再按播放**从原位置续播**（不再回到文件开头）
- 关机 / light-sleep 唤醒 → 自动恢复到上次位置
- 上电开机 → 自动恢复到上次位置（断点随 NVS 持久化）
- 组合键 **快退 + 停止**（250ms 内先后/同按）→ 跳到当前曲目开头，用于从头重听
- **快进/快退为按住态**：其间按播放/停止/上一首/下一首一律忽略、继续走带，仅松手才恢复正常速度续播（磁带机互锁）；进入/退出变速态会清空组合键计时，避免误触跳曲首
- 使用 NVS 命名空间 `"tapebook"` 存储（与 config.h/DESIGN 一致）

## UI 界面预览

显示屏（ST7789 320×240 横屏）由 `main/display.cpp`（原生 `esp_lcd` + LVGL v9）驱动，UI 分为主播放界面、浏览界面、提示界面三类。**全中文界面**，带**磁带卷轴动画**、**图形电量/音量**、**快进快退读秒**与**按键位置图例**。

- **HTML 交互预览（推荐，长文件名滚动 + 卷轴旋转动画实时播放）**：[`docs/ui_preview.html`](docs/ui_preview.html)
- **PNG 整页截图（含全部 11 个界面，~360 KB）**：[`docs/ui_preview.png`](docs/ui_preview.png)

界面构成：

| # | 界面 | 说明 |
|---|------|------|
| 1 | 主播放 · 播放中 | 卷轴**正向匀速**旋转，长文件名循环滚动 |
| 2 | 主播放 · 已暂停 | 卷轴静止 |
| 3 | 主播放 · 已停止 | 卷轴静止变暗 |
| 4 | 主播放 · 快退中 | 卷轴**反向**（与快进同速 2.0x），居中显示「快退 2.0x → 01:05」读秒 |
| 5 | 主播放 · 快进中 | 卷轴**快速正向**（2.0x），居中显示「快进 2.0x → 05:30」读秒 |
| 6 | 主播放 · 音量调节 | 图形音量条（长按上一首 − / 下一首 +）演示 |
| 7 | 浏览界面 | `浏览 2/24` 文件列表，选中行 `>` + 导航提示 |
| 8 | 启动画面 | 有声书播放器 / ESP32-S3 / 正在加载 SD 卡 |
| 9 | 提示 · 无卡 | 未检测到 SD 卡 |
| 10 | 提示 · 无文件 | 未找到音频文件 |
| 11 | 按键位置参考 | 物理按键左→右布局（按此摆放按键）+ 操作映射 |

设计要点：
- **全中文**：状态栏、模式、按键均为中文；设备端新增中文子集字体 `main/ui_font_12/14/16.c`（`tools/gen_font.py` 用 Windows 黑体生成）。
- **磁带动画**：`reel_anim_cb` 定时器（50ms）按状态旋转——播放=正向匀速 / 快进=快速正向 / **快退=反向（与快进同速）** / 暂停·停止·锁定=静止。
- **图形电量/音量**：状态栏右侧以图标显示——电量（外框+填充，低于 20% 变红，充电显示「充」）、音量（扬声器图标 + 水平音量条，随音量填充），不再用纯文字百分比。
- **快进/快退读秒**：该状态下在进度条上方居中显示醒目琥珀色「快进/快退 N.Nx → mm:ss」，便于掌握跳转位置。
- **按键与物理位置对应**：底部提示按物理按键左→右顺序——`快退 播放 快进 停止 ｜ 上一首 下一首`（走带四键居左成组，选曲两键在右），当前主操作高亮；第 ⑪ 屏给出物理布局参考与操作映射——**播放短按=播放/暂停、长按=切换模式（顺序/列表循环/单曲循环）**、**停止短按=停止、长按=进入浏览（快退/快进短按上/下翻页、长按跳到列表头/尾、上一首/下一首短按上/下移一曲、长按连续快速移动、播放确认、停止退出）**、**浏览中停止长按=给选中曲加书签**、**上一首/下一首长按=音量 −/+（仅非浏览态）**；已移除「按键锁定」功能，原双击操作改为长按/移入浏览界面，且播放/停止键关闭双击检测使短按即时响应；新增 **快退+停止组合键＝跳到当前曲首（从头重听）**，单独按键零延迟、仅 250ms 内成对按下才触发；**快进/快退为按住态，其间其他键一律忽略、继续走带，仅松手才恢复正常续播（磁带机互锁）**；**新增专用音量键**——GPIO0（左拨 LCK a-b）= 音量-、GPIO3（右拨 LCK a-d）= 音量+，短按 ±1 级、释放存 NVS，长按/连续拨动兜底 100ms/级；**同时移除**上一首/下一首长按的音量调节逻辑（专用键已接管，避免双路径）。
- 配色：背景 `#0a0e17`、强调青 `#2dd4bf`、琥珀 `#f5a623`、磁带卷轴装饰、进度条 + 百分比。竖屏（`DISPLAY_ORIENTATION=1`，240×320）布局按 `DISPLAY_WIDTH/HEIGHT` 比例自动重排。

## 扩展建议

1. **音量旋钮**：增加 EC11 旋转编码器，接入两个 GPIO，用中断方式读取
2. **电池供电**：增加 18650 锂电池 + TP4056 充电模块
3. **蓝牙音频**：ESP32-S3 支持蓝牙 A2DP，可连接蓝牙耳机
4. **WiFi 传文件**：增加 Web 服务器，通过浏览器管理 SD 卡文件
5. **自动关机**：无操作 10 分钟后自动休眠
6. **外壳**：3D 打印一个磁带机风格外壳 🎵
