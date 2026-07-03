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
| 显示屏 | SSD1306 0.96寸 OLED (I2C) | 1 | 显示曲目/进度/状态 |
| 按键 | 6×6 轻触开关 | 6 | 控制按键 |
| 旋转编码器 | EC11 (可选) | 1 | 音量旋钮 |
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

ESP32-S3                SSD1306 OLED (I2C)
----------              ------------------
GPIO17 (SDA)       →    SDA
GPIO18 (SCL)       →    SCL
3.3V               →    VCC
GND                →    GND

ESP32-S3                按键 (共6个，对GND)
----------              ----------
GPIO1  → 播放/暂停
GPIO2  → 停止
GPIO8  → 上一首
GPIO9  → 下一首
GPIO14 → 快退 (按住倒带，松开恢复)
GPIO15 → 快进 (按住快进，松开恢复)

所有按键另一端全部接 GND。
ESP32 内部上拉，按下时读低电平。
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
│   ├── display.h/cpp           # OLED 显示模块
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
      │   ├── 停止: 停止播放回到文件开头
      │   ├── 上一首/下一首: 切换曲目
      │   ├── 快进按下: 进入加速模式
      │   ├── 快进松开: 恢复正常速度
      │   ├── 快退按下: 进入倒放加速
      │   └── 快退松开: 恢复正常速度
      │
      ├── 磁带控制 tick (档位自动升级)
      ├── 音频播放器 tick (跳帧 seek)
      │
      └── 刷新 OLED 显示 (每200ms):
          显示曲目名 / 进度条 / 时间 / 速度 / 音量
```

## 断点续播

播放位置自动保存到 ESP32 内部 NVS：
- 停止播放 → 保存当前文件和秒数偏移
- 下次开机 → 自动恢复到上次位置
- 使用 NVS 命名空间 `"tapebook"` 存储（与 config.h/DESIGN 一致）

## 扩展建议

1. **音量旋钮**：增加 EC11 旋转编码器，接入两个 GPIO，用中断方式读取
2. **电池供电**：增加 18650 锂电池 + TP4056 充电模块
3. **蓝牙音频**：ESP32-S3 支持蓝牙 A2DP，可连接蓝牙耳机
4. **WiFi 传文件**：增加 Web 服务器，通过浏览器管理 SD 卡文件
5. **自动关机**：无操作 10 分钟后自动休眠
6. **外壳**：3D 打印一个磁带机风格外壳 🎵
