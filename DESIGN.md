# TapeBook 系统概要设计文档

| 文档信息 | |
|---------|---|
| 项目名称 | TapeBook — 磁带机风格听书机 |
| 文档版本 | 1.1 |
| 编制日期 | 2026-07-02 |
| 适用平台 | ESP32-S3-WROOM-2 (N32R16V / Octal PSRAM) |
| 关联文档 | PRD.md / README.md |

---

## 目录

1. [设计目标与原则](#1-设计目标与原则)
2. [系统总体架构](#2-系统总体架构)
3. [硬件设计](#3-硬件设计)
4. [软件架构](#4-软件架构)
5. [关键模块详细设计](#5-关键模块详细设计)
6. [数据流](#6-数据流)
7. [关键算法](#7-关键算法)
8. [接口规范](#8-接口规范)
9. [错误处理与可靠性设计](#9-错误处理与可靠性设计)
10. [构建与部署](#10-构建与部署)
11. [附录](#11-附录)

---

## 1. 设计目标与原则

### 1.1 设计目标

将 PRD 中定义的产品需求转化为可落地的工程方案，确保：

| 目标 | 度量 |
|------|------|
| **磁带机手感真实还原** | 按住快进键时听到变调加速声，松开立即恢复 |
| **响应实时** | 按键响应 ≤ 100ms，快进激活 ≤ 50ms |
| **离线独立运行** | 无网络、无手机、可独立完成所有听书操作 |
| **低功耗长续航** | 18650 2600mAh 电池可连续播放 ≥ 8 小时 |
| **代码可维护** | 模块化分层，单个文件 < 500 行，单一职责 |
| **易于扩展** | V1.0 → V2.0 增量迭代不破坏既有架构 |

### 1.2 设计原则

1. **分层解耦**：应用层 / 业务逻辑层 / 引擎层 / HAL 层严格分离
2. **状态机优先**：所有交互（按键、应用模式、播放）用 FSM 描述
3. **配置驱动**：所有时间阈值、加速档位、引脚集中在 `config.h`
4. **优雅降级**：硬件异常 → 安全状态，不死机
5. **小内存占用**：PSRAM 大缓冲区，主内存堆 ≤ 100KB

---

## 2. 系统总体架构

### 2.1 系统框图

```
┌─────────────────────────────────────────────────────────────────┐
│                        用户交互层                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
│  │ 6 实体按键 │  │ OLED 128x64│  │ 喇叭 3W    │  │ 3.5mm 耳机   │   │
│  └─────┬────┘  └─────┬─────┘  └─────┬────┘  └──────┬───────┘   │
└────────┼─────────────┼──────────────┼───────────────┼──────────┘
         │             │              │               │
┌────────▼─────────────▼──────────────▼───────────────▼──────────┐
│                    ESP32-S3 主控 (240MHz, ~400KB DRAM, 8/16MB PSRAM) │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                     FreeRTOS 任务层                         │ │
│  │   Main Task (20ms)    Audio Task    Idle Hook              │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                     业务逻辑层 (Modules)                    │ │
│  │  button │ tape │ playlist │ display │ bookmark │ settings │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                  音频引擎层 (ESP-ADF)                       │ │
│  │  fatfs_stream → decoder (MP3/AAC/FLAC/OGG/Opus) → i2s      │ │
│  │  resample_filter ← EQ/Voice ← Volume                       │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                     硬件抽象层 (HAL)                        │ │
│  │  I2S  │  SPI  │  I2C  │  GPIO  │  ADC  │  NVS  │  FATFS    │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────┬───────────┬───────────┬───────────┬────────────────┘
              │           │           │           │
         ┌────▼───┐  ┌────▼───┐  ┌───▼────┐  ┌───▼────┐
         │MAX98357│  │MicroSD │  │SSD1306 │  │TP4056  │
         │ I2S DAC│  │  SPI   │  │OLED I2C│  │Charge  │
         └────────┘  └────────┘  └────────┘  └────────┘
```

### 2.2 数据总线划分

| 总线 | 设备 | 速率 | 占用 |
|------|------|------|------|
| I2S | MAX98357 | 44.1kHz × 16bit × 2ch = 1.4 Mbps | 高速实时 |
| SPI | MicroSD | 最高 40 MHz | 大数据块读 |
| I2C | SSD1306 | 400 kHz | 低速显示 |
| GPIO | 6 个按键 | 软件轮询 20ms | 无总线占用 |
| ADC | 电池电压 (可选) | 软件触发 | < 1 Hz |

### 2.3 时钟与功耗域

- **CPU 主时钟**：240 MHz（解码需要）
- **I2S 时钟**：MCLK 由 APLL 派生，保证低抖动
- **休眠模式**：Light-sleep（保留 RAM），按键 GPIO 中断唤醒

### 2.4 ESP32-S3-WROOM-2 模块特性（基于官方 v1.7 数据手册）

| 项目 | 规格 |
|------|------|
| SoC | ESP32-S3R8V / ESP32-S3R16V（Xtensa LX7 双核 32 位，240 MHz） |
| 封装内 PSRAM | 8 MB / 16 MB Octal SPI |
| Flash | 16 MB / 32 MB Octal SPI |
| 电源 | 3.0 ~ 3.6 V（典型 3.3 V） |
| 工作温度 | -40 ~ +65 °C |
| 模块尺寸 | 18.0 × 25.5 × 3.1 mm |
| 封装 | SMD 贴片（41 焊盘 + 1 EPAD） |
| GPIO | 33 个（含 4 个 Strapping 引脚） |
| 1.8V 电压域 | GPIO47、GPIO48（VDD_SPI 域，需电平转换） |
| 板载天线 | PCB 天线（2.4 GHz Wi-Fi + BLE 5） |
| 峰值电流 | Wi-Fi TX ~355 mA；BLE TX ~130 mA |
| 深度休眠 | 7 µA（RTC 内存保持） |

**与 WROOM-1 关键差异**：

1. **Octal PSRAM** 占用额外 4 个 GPIO（FSPI IOs），WROOM-2 可用 GPIO 更少
2. **GPIO47/GPIO48 为 1.8V 域**（WROOM-1 没有这两个 GPIO 暴露）
3. **闪存 + PSRAM 都改用 Octal SPI**（WROOM-1 通常 Quad），总线速度更高
4. **VDD_SPI 固定 1.8V**（由 eFuse 锁定），不可通过软件改回 3.3V
5. **Strapping 引脚默认状态不同**：GPIO45 默认下拉（0），影响 ROM 日志打印

**GPIO 使用约束**：

- ❌ 避免使用 GPIO47/48（1.8V）— 除非配合电平转换
- ⚠️ GPIO0/3/45/46 为 Strapping 引脚 — 启动期间不能拉错电平
- ✅ I2S 推荐的 GPIO4/5/6/7（BCK/WS/DOUT/MCLK）在 WROOM-2 上完全可用
- ✅ SPI 推荐的 GPIO10/11/12/13 在 WROOM-2 上完全可用
- ✅ I2C 推荐的 GPIO17/18 在 WROOM-2 上完全可用

## 3. 硬件设计

### 3.1 硬件框图

```
                            ┌─────────────────────┐
                            │   ESP32-S3-WROOM-2  │
                            │   (Octal PSRAM)    │
                            │   240MHz / 8-16MB PSRAM │
   ┌──────┐   I2S_BCK(4)──►│                     │
   │      │   I2S_WS(5)───►│                     │
   │MAX   │   I2S_DOUT(6)─►│  GPIO Matrix        │
   │98357 │   I2S_MCLK(7)─►│                     │◄──SD_CS(10)
   │      │                 │                     │◄──SD_MOSI(11)
   │      │ ◄──GAIN/SD_MODE │                     │◄──SD_MISO(12)
   └──┬───┘                 │                     │◄──SD_SCLK(13)
      │                     │                     │
      ▼                     │                     │◄──OLED_SDA(17)
   ┌──────┐                 │                     │◄──OLED_SCL(18)
   │3W 4Ω│                 │                     │
   │Speaker                │                     │◄──BTN_PLAY(1)
   └──────┘                 │                     │◄──BTN_STOP(2)
                            │                     │◄──BTN_PREV(8)
                            │                     │◄──BTN_NEXT(9)
                            │                     │◄──BTN_REW(14)
                            │                     │◄──BTN_FF(15)
                            │                     │◄──ENC_A(38) [可选]
                            │                     │◄──ENC_B(39) [可选]
                            │                     │◄──BAT_ADC(3) [可选]
                            └─────────────────────┘
```

### 3.2 GPIO 分配表（扩展版）

| GPIO | 功能 | 输入/输出 | 上下拉 | 中断 | 备注 |
|------|------|----------|--------|------|------|
| 1 | BTN_PLAY/PAUSE | IN | 上拉 | - | 按下低电平 |
| 2 | BTN_STOP | IN | 上拉 | - | 按下低电平 |
| 3 | BAT_ADC | IN | - | - | 1/2 分压，0~2.4V → ADC1_CH3 |
| 4 | I2S_BCK | OUT | - | - | 位时钟 |
| 5 | I2S_WS | OUT | - | - | 字选择 |
| 6 | I2S_DOUT | OUT | - | - | 数据输出 |
| 7 | I2S_MCLK | OUT | - | - | 主时钟 (APLL) |
| 8 | BTN_PREV | IN | 上拉 | - | |
| 9 | BTN_NEXT | IN | 上拉 | - | |
| 10 | SD_CS | OUT | - | - | 片选 |
| 11 | SD_MOSI | OUT | - | - | |
| 12 | SD_MISO | IN | - | - | |
| 13 | SD_SCLK | OUT | - | - | |
| 14 | BTN_REW | IN | 上拉 | - | 快退 |
| 15 | BTN_FF | IN | 上拉 | - | 快进 |
| 17 | OLED_SDA | I/O | 上拉 | - | I2C0 |
| 18 | OLED_SCL | OUT | - | - | I2C0 |
| 38 | ENC_A | IN | 上拉 | GPIO_INT | 编码器 A 相 |
| 39 | ENC_B | IN | 上拉 | GPIO_INT | 编码器 B 相 |
| 0 | BOOT | - | - | - | 保留（自动下载模式） |

**GPIO 冲突检查**：
- GPIO 1, 2 (按键) ⚠️ 注意：下载时为 UART0，可能需要切换
- GPIO 17, 18 ⚠️ 默认 USB-JTAG，需在 menuconfig 关闭 USB
- **WROOM-2 特有约束**：
  - GPIO47/48 为 1.8V 域，本项目未使用，避开即可
  - GPIO0/3/45/46 为 Strapping 引脚，本项目未用作业务 GPIO，安全
  - 所有使用的 GPIO（1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 38, 39）在 WROOM-2 上完全可用

### 3.3 电源设计

```
   ┌──────────┐         ┌──────────┐
   │ USB 5V   │────────►│ TP4056   │──OUT+──┐
   │ Type-C   │   IN+   │ 充电管理  │        │
   └──────────┘         │ + 保护    │        │
                        └──────────┘        │
                                             │
                                             ▼
                                    ┌──────────────────┐
                                    │  18650 锂电池    │
                                    │  3.7V 2600mAh    │
                                    │  带保护板        │
                                    └────────┬─────────┘
                                             │ B+
                                             │
                                             ▼
                                    ┌──────────────────┐
                                    │ 拨动开关 SS12D00 │ (总电源)
                                    └────────┬─────────┘
                                             │
                       ┌─────────────────────┼─────────────────────┐
                       ▼                     ▼                     ▼
              ┌─────────────┐        ┌─────────────┐        ┌─────────────┐
              │ AMS1117-3.3 │        │ MAX98357 VIN│        │ OLED VCC    │
              │ LDO 800mA   │        │ (3.3V 也可) │        │ 3.3V        │
              └──────┬──────┘        └─────────────┘        └─────────────┘
                     │ 3.3V
                     ▼
              ┌─────────────┐
              │ ESP32-S3    │ + MicroSD + 编码器
              └─────────────┘
```

**电流预算**：

| 模块 | 工作电流 | 峰值 |
|------|---------|------|
| ESP32-S3 (WiFi/BT 关) | 50 mA | 240 mA (瞬态) |
| MAX98357 (中等音量) | 80 mA | 250 mA (峰值音量) |
| OLED SSD1306 | 20 mA | 30 mA |
| MicroSD 读写 | 30 mA | 100 mA |
| **合计** | **~180 mA** | **~620 mA** |

2600 mAh 电池理论续航 = 2600 / 180 ≈ **14 小时**（实际约 10-12 小时，留出余量）

### 3.4 关键电路

#### 3.4.1 MAX98357 配置

| 引脚 | 连接 | 含义 |
|------|------|------|
| GAIN | GND | 12 dB 增益（4Ω，仅 MAX98357B；A 变体为 9dB） |
| SD_MODE | VDD | L+R 混合为单声道输出（单扬声器场景应接 VDD 以获得左右混合） |
| VIN | 3.3V 或 5V | 3.3V 时输出约 1.8W，5V 时 3W |

#### 3.4.2 SD 卡 SPI 接口

- 使用 SPI2_HOST（避开 SPI0/SPI1 保留给 Flash/PSRAM）
- DMA 通道：SPI_DMA_CH_AUTO
- 最高时钟：40 MHz（普通卡） / 20 MHz（低速卡兼容）
- CS 信号：软件管理，注意在 SPI 多设备时序

#### 3.4.3 按键电路

```
       3.3V
        │
       ┌┴┐
       │ │ 10kΩ 内部上拉
       └┬┘
        │  (ESP32 内部)
        ├────► GPIOx
        │
       ┌┴┐
       │ │ 轻触开关
       │ │
       └┬┘
        │
       GND
```

按下时 GPIO 读 0，松开时 GPIO 读 1。已使用 ESP32 内部上拉，无需外部电阻。

#### 3.4.4 电池电压检测（可选）

```
  BAT+ ──── ┬──── ─ ─ ─ ─ ─
            │
            R1 (100kΩ)
            │
            ├────► GPIO3 (ADC1_CH3)
            │
            R2 (100kΩ)
            │
  GND ──────┴──── ─ ─ ─ ─ ─
```

分压比 1:1，3.7V → 1.85V（ADC 安全范围）。

**注意**：ESP32-S3 ADC 输入源阻抗建议 ≤ 50kΩ。两个 100kΩ 电阻串联的等效源阻抗约为 50kΩ，处于边界。为获得稳定读数，建议在 ADC 引脚对 GND 并联 **100nF 陶瓷电容**；更高精度可改用 47kΩ+47kΩ 分压（功耗约 39µA）。

### 3.5 PCB 布局注意事项

1. **SD 卡座**：远离 MAX98357 模拟部分，避免数字噪声耦合
2. **I2S 信号线**：BCK/WS/DOUT 等长走线，避免 90° 直角
3. **电源**：ESP32 与 MAX98357 各自退耦电容（100nF + 10μF）
4. **喇叭走线**：短而粗，避免 PWM 类信号的串扰

---

## 4. 软件架构

### 4.1 软件分层

```
┌─────────────────────────────────────────────────────────────┐
│                   应用层 (Application Layer)                │
│  main.cpp - 主循环、状态机、事件分发、用户交互              │
├─────────────────────────────────────────────────────────────┤
│                   业务逻辑层 (Business Logic)               │
│  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌──────────┐ │
│  │ button_mgr │ │tape_control│ │ playlist   │ │ display  │ │
│  └────────────┘ └────────────┘ └────────────┘ └──────────┘ │
│  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌──────────┐ │
│  │ bookmark   │ │ settings   │ │ power_mgmt │ │ voice    │ │
│  └────────────┘ └────────────┘ └────────────┘ └──────────┘ │
├─────────────────────────────────────────────────────────────┤
│                   引擎层 (Engine Layer)                     │
│  ┌────────────────────────────────────────────────────┐    │
│  │           audio_player (ESP-ADF Pipeline)           │    │
│  │  Reader → Decoder → [EQ] → [Resample] → I2S Writer │    │
│  └────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                   抽象层 (HAL Layer)                        │
│  I2S / SPI / I2C / GPIO / ADC / NVS / FATFS               │
├─────────────────────────────────────────────────────────────┤
│                   OS Layer (FreeRTOS / ESP-IDF)             │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 模块职责矩阵

| 模块 | 职责 | 对外接口 | 依赖 |
|------|------|---------|------|
| `main` | 主循环、状态机、事件路由 | `app_main()` | 所有模块 |
| `button_manager` | 按键扫描、去抖、短按/双击(仅部分按键)/长按/超长按、HOLD 检测 | `init/scan()` | GPIO |
| `tape_control` | 磁带机档位管理、速度计算 | `init/ff_press/ff_release/...` | esp_timer |
| `playlist` | SD 卡扫描、排序、索引 | `scan/count/next/prev/get_*` | FATFS |
| `audio_player` | 音频管道、解码、变速、seek | `init/play/pause/seek/set_speed` | ESP-ADF |
| `display` | OLED 界面绘制 | `init/update/show_*` | I2C/u8g2 |
| `bookmark` | 书签增删查 | `add/del/list/jump` | NVS |
| `settings` | 配置存储 | `get_volume/set_volume/get_mode` | NVS |
| `power_mgmt` | 电池检测、休眠、定时 | `init/tick/get_battery` | ADC/FreeRTOS |
| `voice_prompt` | 预录 WAV 状态播报 | `say_status/say_time` | SD WAV / audio_player |

### 4.3 任务划分与调度

```
┌─────────────────────────────────────────────────────────────┐
│ Task Name          │ Priority │ Stack │ Period │ Trigger   │
├─────────────────────────────────────────────────────────────┤
│ main_task          │ 1        │ 8KB   │ 20ms   │ 事件循环  │
│ audio_pipeline_xxx │ 10       │ 4KB   │ async  │ ESP-ADF   │
│ key_scan           │ -        │ -     │ 20ms   │ 主循环内  │
│ display_update     │ -        │ -     │ 200ms  │ 主循环内  │
│ power_mgmt_tick    │ 2        │ 2KB   │ 1000ms │ 软件定时器│
│ idle_hook          │ 0        │ -     │ idle   │ FreeRTOS  │
└─────────────────────────────────────────────────────────────┘
```

**优先级原则**：实时性要求高的任务（按键、音频）优先级高；后台任务（电源管理）优先级低。

### 4.4 进程间通信

- **按键 → 主循环**：通过事件队列 `g_button_events` (8 个槽位)
- **主循环 → 音频引擎**：函数调用 + 全局状态变量
- **音频引擎 → 主循环**：状态回调函数 (`audio_status_cb_t`)
- **音频引擎内部**：ESP-ADF 内部 `audio_event_iface`（异步消息）

由于本系统应用层采用**单线程主循环 + 回调驱动**架构，业务模块之间不直接使用多线程同步，避免竞争问题。

> 注：ESP-ADF 音频管道内部会创建自己的任务（如 `audio_pipeline` 任务、I2S DMA 任务等），这些属于引擎层内部任务；应用层代码仅通过事件/回调与它们交互，不直接访问其内部同步原语。

### 4.5 内存分配

| 区域 | 大小 | 用途 |
|------|------|------|
| 主 SRAM (DRAM) | 400KB | FreeRTOS 栈、任务控制、关键变量 |
| PSRAM (Octal) | 8MB / 16MB | 音频帧缓冲、解码器中间态、播放列表 |
| NVS Flash 分区 | 64KB | 断点记忆、书签、设置 |
| FATFS 分区 | 剩余空间 | SD 卡文件系统 |

关键分配：

```c
// 主 SRAM 中（避免 PSRAM 访问延迟）
static app_state_t g_app_state;
static btn_ctx_t   g_buttons[BTN_ID_MAX];
static tape_mode_t g_tape_mode;

// PSRAM 中（大块数据）— 结构体整体排序，名称-路径不错乱
static EXT_RAM_ATTR playlist_item_t g_items[PLAYLIST_MAX_SIZE];  // 名称+路径绑定
static EXT_RAM_ATTR int16_t g_audio_buf[AUDIO_BUF_SIZE];  // 16KB
```

---

## 5. 关键模块详细设计

### 5.1 音频引擎（audio_player）

#### 5.1.1 架构

采用 ESP-ADF 的 pipeline 架构：

```
┌────────────┐   ┌──────────┐   ┌────────────┐   ┌──────────┐
│  fatfs_    │   │ decoder  │   │ resample   │   │ i2s_     │
│  stream    ├──►│ (按格式) ├──►│ (可选)     ├──►│ stream   ├──► I2S
│  (Reader)  │   │          │   │            │   │ (Writer) │    Bus
└────────────┘   └──────────┘   └────────────┘   └──────────┘
```

#### 5.1.2 解码器选择算法

```c
static audio_element_handle_t create_decoder(const char *filepath) {
    const char *ext = get_file_ext(filepath);
    if      (strcasecmp(ext, ".mp3")  == 0) return mp3_decoder_init(...);
    else if (strcasecmp(ext, ".aac")  == 0) return aac_decoder_init(...);
    else if (strcasecmp(ext, ".m4a")  == 0) return aac_decoder_init(...);
    else if (strcasecmp(ext, ".flac") == 0) return flac_decoder_init(...);
    else if (strcasecmp(ext, ".ogg")  == 0) return ogg_decoder_init(...);
    else if (strcasecmp(ext, ".opus") == 0) return opus_decoder_init(...);
    else if (strcasecmp(ext, ".wav")  == 0) return wav_decoder_init(...);
    return NULL; // 不支持
}
```

#### 5.1.3 状态机

```
                play()
        ┌──────────────────┐
        │                  │
        ▼                  │
   ┌─────────┐   pause()   ┌─────────┐
   │ STOPPED │◄───────────│ PLAYING │
   │  /IDLE  │             │         │
   └────┬────┘   resume()  └────┬────┘
        │                        │
        │ stop()                 │ pause()
        │                        ▼
        │                  ┌─────────┐
        └─────────────────►│ PAUSED  │
                           └─────────┘
                                │
                                │ track_finished()
                                ▼
                          ┌─────────┐
                          │ STOPPED │
                          └─────────┘
```

- **STOPPED/IDLE**：无音频管道，或播放已停止且位置归零
- **PLAYING**：音频管道运行中
- **PAUSED**：音频管道挂起，当前位置保持
- **track_finished()**：当前曲目自然播完，根据播放模式进入下一首或停止

**与主状态机的关系**：
- 音频播放器状态机只描述解码器/管道的运行状态
- 主应用状态机（`app_state_t`）描述整个设备的模式（播放、暂停、快进、快退、菜单等）

#### 5.1.4 关键接口

```c
bool audio_player_play(const char *filepath);   // 同步启动，返回成功/失败
void audio_player_pause(void);
void audio_player_resume(void);
void audio_player_stop(void);                    // 释放管道
void audio_player_seek(int seconds);            // 跳转到 N 秒
int  audio_player_get_position(void);            // 当前秒数
int  audio_player_get_duration(void);            // 总秒数
void audio_player_set_speed(float speed);        // 1.0 正常, >1 加速, <0 倒放
void audio_player_set_volume(int vol);           // 0~100
void audio_player_tick(void);                    // 主循环调用，处理 FF/REW 跳帧
```

### 5.2 磁带机控制（tape_control） ★ 核心

#### 5.2.1 状态机

```
       ┌──────────────────┐ press_ff() ┌──────────────────┐
       │ TAPE_MODE_NORMAL ├───────────►│ TAPE_MODE_FAST_  │
       │ (gear=0)         │            │ FORWARD          │
       │ speed=1.0        │◄───────┐   │ (gear=0~4)       │
       └───────▲──────────┘ release│   └────────┬─────────┘
               │    (ff_release)   │            │ release_ff()
               │                   │            ▼
               │    release        │   ┌──────────────────┐
               │   (rew_release)   │   │ TAPE_MODE_REWIND │
               │                   │   │ (gear=0~4)       │
               │                   ▲   └──────────────────┘
               └───────────────────┘ press_rewind()
```

#### 5.2.2 加速阶梯算法

```c
// 配置（config.h）
#define TAPE_ACCEL_STEP1_MS   800   // 0.8s 进入 1.5x
#define TAPE_ACCEL_STEP2_MS  2000   // 2.0s 进入 2.5x
#define TAPE_ACCEL_STEP3_MS  4000   // 4.0s 进入 4.0x
#define TAPE_ACCEL_STEP4_MS  7000   // 7.0s 进入 8.0x

// 阶梯表
typedef struct { uint32_t threshold_ms; float speed; } speed_step_t;
static const speed_step_t steps[] = {
    {TAPE_ACCEL_STEP1_MS, 1.5f},
    {TAPE_ACCEL_STEP2_MS, 2.5f},
    {TAPE_ACCEL_STEP3_MS, 4.0f},
    {TAPE_ACCEL_STEP4_MS, 8.0f},
};

// 档位计算
void tape_control_tick(void) {
    if (mode == TAPE_MODE_NORMAL) return;

    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - press_start_us) / 1000);
    int new_gear = 0;
    float new_speed = 1.0f;

    for (int i = 0; i < NUM_STEPS; i++) {
        if (elapsed_ms >= steps[i].threshold_ms) {
            new_gear = i + 1;
            new_speed = steps[i].speed;
        } else {
            break;
        }
    }

    if (mode == TAPE_MODE_REWIND) {
        new_speed = -new_speed;  // 倒带用负数表示方向
    }

    if (new_gear != current_gear || new_speed != current_speed) {
        current_gear = new_gear;
        current_speed = new_speed;
        audio_player_set_speed(new_speed);  // 通知音频引擎变速/跳帧
    }
}
```

#### 5.2.3 快进/快退音效实现

采用 **"变速 + 跳帧"** 组合方案，模拟磁带机：

| 操作 | 速度倍率 | I2S 采样率 | 跳帧策略 | 听感 |
|------|---------|-----------|---------|------|
| 正常 | 1.0x | 44100 Hz | 不跳帧 | 原音 |
| 快进 1.5x | 1.5x | 66150 Hz | 不跳帧 | 1.5x 变调快放 |
| 快进 2.5x | 2.5x | 96000 Hz（上限） | 不跳帧 | 叽叽喳喳 |
| 快进 4.0x | 4.0x | 96000 Hz（上限） | 每 50ms 跳 200ms | 极快扫描 |
| 快进 8.0x | 8.0x | 96000 Hz（上限） | 每 50ms 跳 400ms | 极速 |
| 快退 1.5x | -1.5x | 44100 Hz | 每 50ms seek -75ms | 断续倒退跳跃 |
| 快退 8.0x | -8.0x | 44100 Hz | 每 50ms seek -400ms | 极速倒带（断续） |

> **注意**：I2S 采样率只能为正，不能为负。快退时通过 **频繁向后 seek** 来模拟"倒带"感，保持采样率不变（或略高以产生变调），因此听感与快进不同——更偏向"断续跳跃"而非"连续变调"。

```c
// I2S 采样率修改（变调效果）
i2s_stream_set_clk(i2s_writer, sample_rate, 16, 2);
// 注意：ESP-ADF 中采样率修改会重新初始化 I2S 驱动，需确保无并发访问

// 跳帧（seek）
// 在 ESP-ADF 中通常通过 pipeline 或 decoder 元素的 position API 实现，例如：
// audio_element_get_pos(decoder, &cur_pos, 0);  // 获取当前位置（ms）
// audio_element_set_pos(decoder, target_pos_ms);  // 跳转到目标位置（ms）
// 具体可用性取决于当前解码器是否支持 seek。
// 跳帧后会有一小段解码延迟（约 100-300ms），这正是磁带机的声音！
```

### 5.3 按键管理（button_manager）

#### 5.3.1 状态机

```
       ┌─────┐ 按下 ┌─────────┐ 30ms ┌─────────┐
       │ IDLE├─────►│DEBOUNCE ├─────►│ PRESSED ├──────────────────────────────────────────┐
       └─────┘      └────┬────┘      └────┬────┘                                            │
                         │ 抖动            │ 500ms                                           │
                         │ 退回 IDLE       ▼                                                 │
                         │            ┌─────────┐                                            │
                         │            │LONG_PRESS├────►│HOLD──►(3s)EXTRA_LONG_PRESS──►HOLD │
                         │            └────┬────┘                                            │
                         │                 │ 进入 HOLD │ 松开                                 │
                         │                 ▼           ▼                                      │
                         │            ┌─────────┐ ┌─────┐                                    │
                         │            │  HOLD   ├►│ IDLE│ (RELEASE)                          │
                         │            └─────────┘ └─────┘                                    │
                         │                                                                │
                         │   PRESSED 松开 (短按):                                          │
                         │   ┌─ dbl_click_en=true ──► DBL_WAIT ─► (300ms超时) ─► SHORT_PRESS
                         │   │                              │ 二次按下
                         │   │                              └─► DBL_DEBOUNCE ─► DBL_PRESSED ─► DOUBLE_CLICK
                         │   │
                         │   └─ dbl_click_en=false ─► SHORT_PRESS (即时，0ms延迟)
```
                         │                 ▼           ▼
                         │            ┌─────────┐ ┌─────┐
                         │            │  HOLD   ├►│ IDLE│ (RELEASE)
                         │            └─────────┘ └─────┘
```

#### 5.3.2 事件类型

| 事件 | 触发时机 | 用法 |
|------|---------|------|
| `SHORT_PRESS` | 按下→松开（双击启用按键需等待窗口确认；FF/RW 即时触发） | 播放/暂停、停止、上下首、±10s 跳转 |
| `DOUBLE_CLICK` | 短按间隔 < 300ms 的第二次松开（仅 Play/Stop 启用） | 切换播放模式、语音播报 |
| `LONG_PRESS` | 按下超过 500ms 瞬间 | 磁带加速(FF/RW)、音量调节(Prev/Next) |
| `EXTRA_LONG_PRESS` | 按下超过 3000ms 瞬间 | 按键锁定/解锁 |
| `HOLD` | 持续按住期间每 20ms | 带加速持续(FF/RW)、音量连续调节(Prev/Next 每 100ms 一步) |
| `RELEASE` | 从 HOLD 状态松开 | 退出磁带模式 |

#### 5.3.3 性能指标

- 扫描周期：20ms（满足 ≤ 100ms 响应要求）
- 去抖时间：30ms（硬件典型值）
- 长按阈值：500ms（可配置）
- 双击窗口：300ms（可配置）
- 超长按阈值：3000ms（按键锁定专用）
- CPU 占用：< 0.1%

### 5.4 播放列表（playlist）

#### 5.4.1 数据结构

```c
typedef struct {
    char display_name[FILENAME_MAX_LEN];  // 显示用，可含子目录前缀
    char full_path[FILENAME_MAX_LEN * 2]; // 完整路径，用于打开文件
} playlist_item_t;

// 结构体数组整体排序，保证 display_name 与 full_path 一一对应
static EXT_RAM_ATTR playlist_item_t g_items[PLAYLIST_MAX_SIZE];

typedef struct {
    int count;
    int current_index;
} playlist_state_t;
```

#### 5.4.2 扫描算法

```
1. 打开 SD 卡根目录
2. 遍历根目录：
   - 普通文件 → 检查扩展名 → 加入列表
   - 子目录 → 暂存到子目录列表
3. 遍历每个子目录：
   - 同上，加入列表（display_name 加子目录前缀）
4. 按文件名排序
5. 返回总数
```

**复杂度**：O(N)，N 为总文件数。256 文件约 50ms 完成。

#### 5.4.3 文件过滤

支持扩展名（大小写不敏感）：
- 音频：`.mp3`, `.wav`, `.flac`, `.aac`, `.m4a`, `.ogg`, `.opus`
- 隐藏文件（`.xxx`）：跳过
- 系统文件：`System Volume Information`, `._.DS_Store` 等：跳过

### 5.5 断点续播（settings / bookmark 模块）

#### 5.5.1 NVS 数据布局

```
Namespace: "tapebook"
┌──────────────────────────────────────────────────────┐
│ Key             │ Type    │ Example       │ 备注      │
├──────────────────────────────────────────────────────┤
│ last_track_idx  │ uint8   │ 3             │ 当前曲目  │
│ last_position   │ uint32  │ 3725          │ 秒数      │
│ volume          │ uint8   │ 70            │ 0~100     │
│ play_mode       │ uint8   │ 1             │ 0=顺序    │
│ auto_off_min    │ uint8   │ 30            │ 定时关机  │
│ ab_repeat       │ uint32  │ 0x003C0078    │ A=60s, B=120s（高 16 bit = A，低 16 bit = B）│
│ book_0_name     │ str[64] │ "三体.mp3"    │ 书签关联  │
│ book_0_pos      │ uint32  │ 3725          │ 书签位置  │
│ book_0_total    │ uint32  │ 43200         │ 总时长    │
│ ...                                                      │
└──────────────────────────────────────────────────────┘
```

#### 5.5.2 保存时机

| 触发条件 | 保存内容 |
|---------|---------|
| 用户主动停止 | 当前文件 + 位置 |
| 切换曲目 | 旧文件 + 位置 |
| 定时关机 | 当前文件 + 位置 |
| 播放过程中 | 每 30 秒自动保存 |
| 低电关机 | 当前文件 + 位置 |

#### 5.5.3 启动恢复流程

```
1. mount SD
2. nvs_open("tapebook", READ_ONLY)
3. 读 last_track_idx, last_position
4. 扫描 SD 卡片单
5. 如果 last_track_idx 对应文件存在 → 跳转到 last_position
6. 如果文件已删除 → 清除该条记忆，从头开始
7. nvs_close()
```

### 5.6 OLED 显示（display）

#### 5.6.1 屏幕布局（128×64）

```
┌──────────────────────────────────────┐  y=0
│ ▶ 03/12  L  B85  V70  →              │  状态栏 (h=10)
├──────────────────────────────────────┤  y=10
│                                      │
│  三体第一部.mp3                       │  文件名 (h=14, y=24)
│                                      │
├──────────────────────────────────────┤  y=28
│  ████████████████░░░░░░░░░░░░░░░░░  │  进度条 (h=10, y=38)
│  ▲当前位置                            │
├──────────────────────────────────────┤  y=40
│  12:35 / 45:00             [2.5x]    │  时间/速度 (h=8, y=48)
├──────────────────────────────────────┤  y=50
│ RW  ◀◀  ▶  ■  ▶▶  FF   [VOL-/+]     │  按键提示 (h=10, y=60)
└──────────────────────────────────────┘  y=64

> 注：状态栏图标（L=锁定、B=电量、V=音量、→=顺序播放）为 ASCII 示意，实际实现采用 u8g2 字体或 8×8 位图。Emoji 不作为实际显示内容。
```

#### 5.6.2 渲染策略

- **整屏重绘**：200ms 一次，仅更新变化部分
- **文件滚动**：长文件名（>21 字符）每 200ms 滚动 2 像素
- **进度条**：每 200ms 重新计算并重绘
- **图标**：使用 u8g2 内置字体（5x7, 6x10, 8x13）

#### 5.6.3 显示状态机

```
       ┌──────────┐ 开机完成 ┌──────────┐
       │  SPLASH  ├─────────►│  MAIN    │
       └──────────┘          └─────┬────┘
                                    │
                                    ├── 长时间无操作 → SCREENSAVER
                                    │                  └─ 按键唤醒 → MAIN
                                    │
                                    ├── 长按 STOP → BROWSE (文件夹浏览)
                                    │                  └─ 短按 STOP → MAIN
                                    │
                                    └── 长按 PLAY → LOCKED
                                                       └─ 长按 PLAY → MAIN
```

### 5.7 电源管理（power_mgmt）P2

#### 5.7.1 功能

- 电池电压 ADC 采样（1 Hz）
- 计算电量百分比
- 低电告警（< 15%）
- 自动休眠（无操作 5 分钟）

#### 5.7.2 状态机

```
        ┌──────────┐ BAT<15% ┌──────────┐ BAT<5% ┌──────────┐
        │ NORMAL   ├────────►│ WARNING  ├───────►│ SHUTDOWN │
        └──────────┘         └──────────┘        └──────────┘
             │                    │                    │
             │ 无操作 5 分钟        │                    ▼
             ▼                    │              保存状态 → 深度休眠
        ┌──────────┐               │
        │SCREENSAVE│               │
        └──────────┘               │
             │ 按键唤醒             │
             └────────► NORMAL
```

### 5.8 语音播报（voice_prompt）P2

#### 5.8.1 实现方案

由于 ESP32 没有内置硬件 TTS，采用**预录语音文件**方式：

- 在 SD 卡 `/voice/` 目录下存放录好的 WAV 文件
- 文件命名：`zh_playing.wav`, `zh_paused.wav`, `zh_volume_70.wav` 等
- 需要播报时，临时切换音频管道到该文件播放

#### 5.8.2 播报时机

- 双击 STOP → 播报当前状态
- 开机完成 → "开始使用"
- 切换曲目 → "正在播放 XX"
- 设置完成 → "已设定 X"

---

## 6. 数据流

### 6.1 主循环数据流

```
        ┌────────────────────────────────────────┐
        │            Main Loop (20ms)            │
        └─┬───────┬──────────┬──────────┬────────┘
          │       │          │          │
          ▼       ▼          ▼          ▼
      ┌──────┐ ┌──────┐ ┌────────┐ ┌─────────┐
      │ KEY  │ │TAPE  │ │ AUDIO  │ │ DISPLAY │
      │ SCAN │ │ TICK │ │  TICK  │ │ UPDATE  │
      └──┬───┘ └──┬───┘ └───┬────┘ └────┬────┘
         │        │         │           │
         ▼        ▼         ▼           ▼
   ┌─────────────────────────────────────────┐
   │      事件队列 → 状态机 → 动作执行       │
   └─────────────────────────────────────────┘
```

### 6.2 音频数据流

```
   SD Card (FATFS)
       │ 4KB DMA Block
       ▼
   fatfs_stream (Reader)
       │ 16KB PCM Frame
       ▼
   ┌─── decoder ───┐
   │ mp3/aac/flac  │
   │   解码        │
   └───────┬───────┘
           │ PCM 44.1kHz 16bit
           ▼
   ┌─── resample ───┐ (可选变速)
   │ 调整采样率    │
   └───────┬───────┘
           │ PCM Stream
           ▼
   ┌─── i2s_stream ──┐
   │ I2S DMA 发送   │
   └───────┬─────────┘
           │ I2S Bus
           ▼
   MAX98357 DAC → Speaker
```

### 6.3 磁带快进数据流

```
   用户长按 FF 键
        │
        ▼
   button_manager → BTN_EVENT_LONG_PRESS
        │
        ▼
   main → tape_control_ff_press()
        │
        ▼
   tape_control: mode=FF, gear=0
        │
        ▼
   audio_player_set_speed(1.5x)  // i2s_stream_set_clk(66150Hz)
        │
        ▼ (每 50ms)
   audio_player_tick() → seek(+N) → audio_element_seek()
        │
        ▼
   解码器内部跳转 → 输出变调音频
        │
        ▼
   用户松开 FF 键
        │
        ▼
   button_manager → BTN_EVENT_RELEASE
        │
        ▼
   main → tape_control_ff_release()
        │
        ▼
   audio_player_set_speed(1.0x) → i2s_stream_set_clk(44100Hz)
        │
        ▼
   恢复正常播放
```

---

## 7. 关键算法

### 7.1 磁带机快进音效算法 ★

**目标**：模拟真实磁带机快进时的"变调 + 抖动感"

**步骤**：

```c
void tape_ff_tick(void) {
    if (mode != TAPE_MODE_FAST_FORWARD && mode != TAPE_MODE_REWIND) return;
    
    elapsed_ms = now - press_start_ms;
    float speed = get_speed_by_elapsed(elapsed_ms);  // 1.5/2.5/4.0/8.0
    bool is_forward = (mode == TAPE_MODE_FAST_FORWARD);

    // 1. 设置 I2S 采样率（产生变调）
    // 只有快进时才提高采样率；快退通过向后 seek 模拟，采样率保持 44100
    if (is_forward) {
        int sample_rate = (int)(44100.0f * speed);
        sample_rate = CLAMP(sample_rate, 8000, 96000);
        i2s_stream_set_clk(i2s_writer, sample_rate, 16, 2);
    } else {
        i2s_stream_set_clk(i2s_writer, 44100, 16, 2);
    }
    
    // 2. 每 50ms 跳帧（高速档时增加扫描感）
    static uint64_t last_skip = 0;
    if (now - last_skip >= 50000) {
        // 跳过的毫秒数 = 50ms × 速度倍率，方向由模式决定
        int skip_ms = (int)(0.05f * speed * 1000.0f);
        if (!is_forward) {
            skip_ms = -skip_ms;  // 快退时反向
        }
        int cur_ms = 0;
        audio_element_get_pos(decoder, &cur_ms, 0);  // 获取当前位置（ms）
        int target_ms = cur_ms + skip_ms;
        target_ms = CLAMP(target_ms, 0, audio_player_get_duration() * 1000);
        audio_element_set_pos(decoder, target_ms);     // 跳转到目标位置
        last_skip = now;
    }
}
```

**为什么这样能模拟磁带机**：
- **变调**：磁带机转速提升时，电机驱动磁带速度加快，导致音频被压缩 → 听到"叽叽喳喳"
- **跳帧**：磁带机快进时是真正"快进"磁带位置，磁头跳过大段音频 → 听到断续片段
- **不平滑跳帧**：跳 50ms 但解码会有 100-300ms 延迟，造成不规则的"咿呀"声 → 极似磁带

### 7.2 文件夹排序算法

使用 `qsort` 按字符串排序，但需要考虑：

1. **文件夹在前，文件在后**
2. **数字优先排序**：`1.mp3` < `2.mp3` < `10.mp3`（而不是 `1.mp3` < `10.mp3` < `2.mp3`）

```c
static int natural_compare(const char *a, const char *b) {
    // 实现自然排序：逐字符比较，遇数字时整段比较
    // 1.mp3 < 2.mp3 < 10.mp3
}
```

### 7.3 进度条更新算法

```c
void draw_progress_bar(int current_sec, int total_sec) {
    if (total_sec <= 0) return;
    int width = (int)(126.0f * current_sec / total_sec);
    width = CLAMP(width, 0, 126);
    
    // 绘制外框
    u8g2_DrawFrame(&u8g2, 1, 30, 126, 10);
    // 绘制填充
    if (width > 0) {
        u8g2_DrawBox(&u8g2, 2, 31, width, 8);
    }
    // 当前位置小三角
    int marker_x = (int)(126.0f * current_sec / total_sec) + 1;
    u8g2_DrawTriangle(&u8g2, marker_x-2, 28, marker_x+2, 28, marker_x, 31);
}
```

### 7.4 跳帧距离计算

| 速度 | 50ms 内跳过的秒数 | 计算依据 |
|------|----------------|---------|
| 1.5x | 0.075 秒 | 50ms × 1.5 |
| 2.5x | 0.125 秒 | 50ms × 2.5 |
| 4.0x | 0.2 秒 | 50ms × 4.0 |
| 8.0x | 0.4 秒 | 50ms × 8.0 |
| -1.5x | -0.075 秒 | 50ms × 1.5（负） |
| -8.0x | -0.4 秒 | 50ms × 8.0（负） |

注意：实际跳过的样本数 = 秒数 × 采样率，且解码器 seek 只能定位到帧边界，所以实际跳跃距离可能略大于理论值。

---

## 8. 接口规范

### 8.1 模块接口头文件

每个 `.h` 文件遵循以下规范：

```c
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 类型定义 */
typedef enum { ... } module_state_t;

/* 函数声明 */
void module_init(void);
void module_do_something(int param);

#ifdef __cplusplus
}
#endif
```

### 8.2 关键数据结构

```c
// 主状态机
typedef enum {
    APP_STATE_IDLE,
    APP_STATE_STOPPED,
    APP_STATE_PLAYING,
    APP_STATE_PAUSED,
    APP_STATE_FAST_FORWARD,
    APP_STATE_REWIND,
    APP_STATE_BROWSE_FILES,
    APP_STATE_MENU,
    APP_STATE_LOCKED,
    APP_STATE_SLEEP,
} app_state_t;

// 播放模式
typedef enum {
    PLAY_MODE_SEQUENCE = 0,
    PLAY_MODE_REPEAT_ALL,
    PLAY_MODE_REPEAT_ONE,
    PLAY_MODE_SHUFFLE,
} play_mode_t;

// 播放器状态（用于显示）
typedef enum {
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_FAST_FORWARD,
    PLAYER_STATE_REWIND,
} player_state_t;
```

### 8.3 全局变量约定

- 所有全局变量以 `g_` 前缀
- 跨模块可见的全局变量集中在 `globals.h`
- 仅模块内部使用的变量用 `static` 修饰

---

## 9. 错误处理与可靠性设计

### 9.1 错误分类

| 错误类型 | 示例 | 处理策略 |
|---------|------|---------|
| **硬件错误** | SD 卡拔出、I2C NACK | 复位外设、显示提示、暂停播放 |
| **文件系统错误** | 文件不存在、读取失败 | 跳过文件、播放下一首 |
| **解码错误** | 文件损坏、格式不支持 | 跳过文件、记录日志 |
| **NVS 错误** | 空间满、版本不匹配 | 擦除重新初始化 |
| **内存错误** | malloc 失败 | 释放非关键资源重试 |

### 9.2 SD 卡热拔插处理

```c
// 在主循环中每 5 秒检测 SD 卡状态
void sd_monitor_task(void *arg) {
    while (1) {
        sdmmc_card_state_t state = sdmmc_get_state(card);
        if (state == SDMMC_CARD_REMOVED) {
            // SD 卡被拔出
            audio_player_stop();
            display_show_no_card();
            playlist_clear();
            // 等待 SD 卡重新插入
            wait_for_sd_insert();
            // 重新挂载
            sd_mount_and_rescan();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

### 9.3 看门狗

- 启用 Task Watchdog Timer (TWDT)
- 主循环每 20ms 调用 `esp_task_wdt_reset()`
- 防止任何任务死循环卡死整个系统

### 9.4 异常恢复流程

```
系统启动
  ├─ NVS 初始化失败 → 擦除 NVS → 重试 → 成功则继续
  ├─ SD 卡挂载失败 → 提示插卡 → 等待重试
  ├─ I2C 设备无响应 → 重启 I2C 总线 → 重试
  ├─ 音频初始化失败 → 复位 ESP32 → 重启
  └─ 任一步骤失败 ≥ 3 次 → 进入安全模式（仅显示错误码）
```

---

## 10. 构建与部署

### 10.1 构建系统

```
audio_player/
├── CMakeLists.txt              # 顶层 (project)
├── sdkconfig.defaults          # 默认配置
├── partitions.csv              # 分区表
├── main/                       # 主组件
│   ├── CMakeLists.txt          # 组件构建 (idf_component_register)
│   └── *.cpp / *.h
├── components/                 # 自定义组件（可选）
│   └── u8g2_esp32/             # u8g2 的 ESP32 移植
└── managed_components/         # 自动管理的依赖（idf component manager）
```

### 10.2 依赖管理

```bash
# 1. ESP-IDF v5.3（推荐 LTS 稳定版）
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.3

# 2. ESP-ADF v2.7（音频框架）
git clone --recursive https://github.com/espressif/esp-adf.git -b v2.7

# 3. u8g2 (OLED 图形库)
# 通过 idf component manager 自动下载，或放到 components/ 目录
```

> **兼容性提醒**：ESP-ADF 版本与 ESP-IDF 版本有严格对应关系。若 `v2.7` 编译时报告 IDF 版本不匹配，请优先使用 Espressif 官方推荐的 IDF 分支（例如 ADF 的 `idf_v5.x` 兼容分支）。

### 10.3 编译烧录流程

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 配置（启用 ADF、u8g2）
idf.py menuconfig
#   → Audio HAL → Enable
#   → FATFS → Long filename support
#   → Partition Table → Custom (partitions.csv)
#   → Flash size → 16 MB

# 编译
idf.py build

# 烧录 + 监视
idf.py -p COM3 flash monitor
```

### 10.4 分区表

```
# Name,    Type, SubType, Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000     (24KB - 设置/书签/断点)
phy_init,  data, phy,      0xf000,   0x1000     (4KB)
factory,   app,  factory,  0x10000,  0x300000   (3MB - 应用程序)
```

> 注：ESP32-S3-WROOM-2 通常为 16MB/32MB Flash。上表仅占用约 4MB，若 Flash 更大，可扩展 `nvs` 或保留后续区域用于 OTA/数据存储。`storage` 分区非必须；SD 卡 FATFS 由 SDSPI 驱动直接挂载，不占用内部 Flash 分区。

### 10.5 OTA 升级

预留 OTA 升级能力，将工厂分区替换为两个 OTA 分区：

```
# Name,    Type, SubType, Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000     (24KB)
phy_init,  data, phy,      0xf000,   0x1000     (4KB)
ota_0,     app,  ota_0,    0x10000,  0x180000   (1.5MB)
ota_1,     app,  ota_1,    0x190000, 0x180000   (1.5MB)
```

> 注：此 OTA 表未包含 `factory` 回退分区。若需要安全回退，可将每个 OTA 分区缩减为 1MB，并在 `0x210000` 处保留 1MB 的 `factory` 分区。启用 OTA 时，请同时配置 `sdkconfig` 中 `Partition Table → Two OTA definitions`。

### 10.6 开发调试工具

| 工具 | 用途 |
|------|------|
| `idf.py monitor` | 串口监视器，查看日志 |
| `idf.py gdb` | 调试器，支持断点 |
| ESP-ADF `audio_dump` | 抓取原始 PCM 数据 |
| `esp-spy` | 抓取 WiFi 数据（蓝牙调试） |
| OLED 截图工具 | 验证显示布局 |

---

## 11. 附录

### 11.1 时序图示例：磁带机快进完整流程

```
时间  按键    状态机        tape_control     audio_player      用户听感
─────────────────────────────────────────────────────────────────────
T+0   FF按下  DEBOUNCE     -                -                 -
T+30  FF保持  PRESSED      -                -                 -
T+500 FF保持  LONG_PRESS   mode=FF,gear=0   speed=1.0x        正常（缓冲期）
T+800 FF保持  HOLD         mode=FF,gear=1   speed=1.5x          轻微变调
T+2000 FF保持 HOLD         mode=FF,gear=2   speed=2.5x          叽叽喳喳
T+4000 FF保持 HOLD         mode=FF,gear=3   speed=4.0x          快速扫描
T+7000 FF保持 HOLD         mode=FF,gear=4   speed=8.0x          极速
T+8500 FF松开 RELEASE      mode=NORMAL,gear=0 speed=1.0x         恢复正常
T+8510 -       IDLE        -                -                 1.0x 正常播放
```

### 11.2 性能预算

| 操作 | CPU 周期 | 实测耗时 |
|------|---------|---------|
| 按键扫描 (1次) | < 1000 | < 5μs |
| 显示更新 (整屏) | ~50000 | ~3ms |
| 音频 tick (正常) | ~5000 | ~50μs |
| 音频 tick (FF/REW seek) | ~50000 | ~3ms |
| 播放列表扫描 (256 文件) | ~500000 | ~50ms |
| 断点保存 NVS | ~10000 | ~10ms |

主循环 20ms 内可完成：按键扫描 + 音频 tick + 显示更新（仅重绘变化部分）。

### 11.3 配置参数速查表

| 参数 | 默认值 | 范围 | 用途 |
|------|--------|------|------|
| `BTN_DEBOUNCE_MS` | 30 | 10-100 | 按键去抖 |
| `BTN_LONG_PRESS_MS` | 500 | 200-2000 | 长按阈值 |
| `BTN_DOUBLE_CLICK_MS` | 300 | 200-500 | 双击判定窗口（仅 Play/Stop 启用） |
| `BTN_EXTRA_LONG_MS` | 3000 | 2000-5000 | 超长按阈值（按键锁定） |
| `BTN_SCAN_INTERVAL` | 20 | 10-50 | 扫描周期 |
| `TAPE_ACCEL_STEP1_MS` | 800 | 200-2000 | 1.5x 进入时长 |
| `TAPE_ACCEL_STEP4_MS` | 7000 | 5000-15000 | 8.0x 进入时长 |
| `AUDIO_SAMPLE_RATE` | 44100 | 8000-48000 | 默认采样率 |
| `AUDIO_BUFFER_SIZE` | 8192 | 1024-32768 | 音频缓冲区 |
| `AUDIO_OUTPUT_VOL` | 70 | 0-100 | 默认音量 |
| `PLAYLIST_MAX_SIZE` | 256 | 64-1024 | 最大曲目数 |
| `DISPLAY_REFRESH_MS` | 200 | 50-500 | 显示刷新周期 |

### 11.4 调试日志规范

所有模块使用统一 TAG：

```c
static const char *TAG = "module_name";  // 例: "audio", "tape", "btn"
ESP_LOGI(TAG, "Info message");   // I - 重要信息
ESP_LOGW(TAG, "Warning");        // W - 警告
ESP_LOGE(TAG, "Error");          // E - 错误
ESP_LOGD(TAG, "Debug");          // D - 调试（默认不显示）
```

### 11.5 后续工作

- [ ] 实现 NVS 断点续播模块
- [ ] 实现 A-B 复读模块
- [ ] 实现定时关机模块
- [ ] 添加 u8g2 图形库支持
- [ ] 集成 ESP-ADF 并验证管道
- [ ] 编写单元测试（按键状态机、tape_control 档位）
- [ ] 制作 3D 打印外壳图纸

### 11.6 变更记录

| 版本 | 日期 | 作者 | 变更 |
|------|------|------|------|
| 1.0 | 2026-07-02 | — | 初稿：基于 PRD V1.0 编写 |
| 1.1 | 2026-07-02 | — | 评审修订：修正目标模块为 ESP32-S3-WROOM-2 Octal PSRAM；删除重复的 3. 硬件设计章节；修正磁带快进时序图；修正 I2S 采样率与快进/快退表；修正 seek API 与跳帧代码示例；修正功耗与电池 ADC 说明；澄清分区表与 OTA 布局 |
| 1.2 | 2026-07-02 | — | 评审修订 V2：系统框图 SRAM 512KB→~400KB；MAX98357 SD_MODE GND→VDD；AMS1117 500mA→800mA；任务优先级修正(main=1, scan 在主循环内)；按键状态机增加双击/超长按；playlist 改为结构体绑定排序+PSRAM；跳帧策略仅≥4x；配置速查表增加双击窗口/超长按阈值 |
