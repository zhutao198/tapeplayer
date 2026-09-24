# TapeBook 系统概要设计文档

| 文档信息 | |
|---------|---|
| 项目名称 | TapeBook — 磁带机风格听书机 |
| 文档版本 | 1.7 |
| 编制日期 | 2026-07-02 |
| 最后修订 | 2026-07-29 |
| 适用平台 | ESP32-S3-WROOM-1 N16R8 (16 MB Flash + 8 MB Octal PSRAM, 3.3 V SPI) |
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
│  │ 6 实体按键 │  │ SPI TFT   │  │ 喇叭 3W    │  │ WS2812 RGB  │   │
│  │          │  │ LCD 屏幕  │  │            │  │  状态指示灯  │   │   │
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
│  │  I2S  │  SPI(SD) │ SPI(TFT) │ GPIO  │  ADC  │  NVS  │ FATFS│ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────┬───────────┬───────────┬───────────┬────────────────┘
              │           │           │           │
         ┌────▼───┐  ┌────▼───┐  ┌───▼────┐  ┌───▼────┐
         │MAX98357│  │MicroSD │  │SPI TFT │  │TP4056  │
         │ I2S DAC│  │  SPI   │  │ LCD    │  │Charge  │
         └────────┘  └────────┘  └────────┘  └────────┘
```

### 2.2 数据总线划分

| 总线 | 设备 | 速率 | 占用 |
|------|------|------|------|
| I2S | MAX98357 | 44.1kHz × 16bit × 2ch = 1.4 Mbps | 高速实时 |
| SPI | MicroSD (SPI2_HOST) | 最高 40 MHz | 大数据块读 |
| SPI | TFT LCD 显示屏 | 最高 40 MHz | 显示刷新 |
| GPIO | 7 个功能按键 + 状态检测 | 软件轮询 20ms | 无总线占用 |
| ADC | 电池电压 (IO1) | 软件触发 | < 1 Hz |
| PWM/GPIO | WS2812 RGB LED (IO48) | 800kHz 时序 | 低频状态指示 |

### 2.3 时钟与功耗域

- **CPU 主时钟**：240 MHz（解码需要）
- **I2S 时钟**：MCLK 由 APLL 派生，保证低抖动
- **休眠模式**：Light-sleep（保留 RAM），按键 GPIO 中断唤醒
- **LCD 软件电源开关**：IO39 控制 AO3401 PMOS，待机时可彻底断开 LCD 电源

### 2.4 ESP32-S3-WROOM1 模块特性（基于官方 v1.7 数据手册）

| 项目 | 规格 |
|------|------|
| SoC | ESP32-S3R8V / ESP32-S3R16V（Xtensa LX7 双核 32 位，240 MHz）|
| 封装内 PSRAM | 8 MB / 16 MB Octal SPI |
| Flash | 16 MB / 32 MB Octal SPI |
| 电源 | 3.0 ~ 3.6 V（典型 3.3 V）|
| 工作温度 | -40 ~ +65 °C |
| 模块尺寸 | 18.0 × 25.5 × 3.1 mm |
| 封装 | SMD 贴片（41 焊盘 + 1 EPAD）|
| GPIO | 33 个可用（含 4 个 Strapping 引脚）|
| 板载天线 | PCB 天线（2.4 GHz Wi-Fi + BLE 5）|
| 峰值电流 | Wi-Fi TX ~355 mA；BLE TX ~130 mA |
| 深度休眠 | 7 µA（RTC 内存保持）|

**实际使用模块：ESP32-S3-WROOM-1 N16R8（非 WROOM-2）**

> 注：原理图 V1 使用 ESP32-S3-WROOM-1（U1 封装为 41 引脚）。WROOM-1 与 WROOM-2 的主要差异在于 PSRAM 配置和部分 GPIO 可用性。本项目实际引脚分配以原理图网表为准。

**GPIO 使用约束**：

- ❌ 避免使用 GPIO47/48 作普通 IO — 本项目 IO48 用于 WS2812（3.3V 域 GPIO 推挽，硬件经电平转换驱动灯珠）
- ⚠️ GPIO0/3/45/46 为 Strapping 引脚 — 启动期间不能拉错电平
- ✅ I2S 使用的 GPIO5/6/7 在 WROOM-1 上完全可用
- ✅ SPI 推荐的 GPIO10/11/12/13 在 WROOM-1 上完全可用
- ✅ TFT 使用的 GPIO8/15/16/17/18 在 WROOM-1 上完全可用

## 3. 硬件设计

### 3.1 硬件框图

```
                            ┌─────────────────────┐
                            │   ESP32-S3-WROOM-1  │
                            │   (Octal PSRAM)    │
                            │   240MHz / 8 MB PSRAM   │
   ┌──────┐   I2S_BCLK(6)──►│                     │
   │      │   I2S_LRC(7)───►│                     │
   │MAX   │   I2S_DIN(5)───►│  GPIO Matrix        │
   │98357 │   I2S_SD(4) ──►│                     │◄──SD_CS(10)
   │      │                 │                     │◄──SD_MOSI(11)
   │      │ ◄──GAIN/SD_MODE │                     │◄──SD_SCLK(12)
   └──┬───┘                 │                     │◄──SD_MISO(13)
      │                     │                     │
      ▼                     │                     │◄──TFT_SDA(18)
   ┌──────┐                 │                     │◄──TFT_SCL(8)
   │3W 4Ω│                 │                     │◄──TFT_DC(16)
   │Speaker                │                     │◄──TFT_RES(17)
   └──────┘                 │                     │◄──TFT_BLK(15)
                            │                     │
                            │                     │◄──KEY_PLAY(9)
                            │                     │◄──KEY_STOP(14)
                            │                     │◄──KEY_PREV(21)
                            │                     │◄──KEY_NEXT(47)
                            │                     │◄──KEY_FF(41)
                            │                     │◄──KEY_REV(42)
                            │                     │
                            │                     │◄──BAT_DET(ADC, IO1)
                            │                     │◄──CHRG(IO2)
                            │                     │◄──SD_CD(IO38)
                            │                     ├──WS2812(IO48)
                            │                     │◄──POW_EN(IO40)
                            │                     │──LCD_POW_EN(IO39)
                            └─────────────────────┘
```

> 注：`KEY_STOP` 实际经 SW7 → IO14；`KEY_PREV` 实际经 SW5 → IO21。本原理图 V1 网表中 IO3、IO46 未连接任何信号，留作 NC；旧文档中"双源/备用"说法与网表不符，已修正。

### 3.2 GPIO 分配表（以原理图 V1 网表为准）

| GPIO | 功能 | 输入/输出 | 上下拉 | 中断 | 备注 |
|------|------|----------|--------|------|------|
| **0** | BOOT / Reset (SW8) | IN | - | - | 启动选择/复位键 |
| **1** | 🔋 BAT_DET (ADC) | IN | - | - | 电池电压检测，经 LMV321 运放 |
| **2** | ⚡ CHRG 状态 | IN | 上拉 | - | TP4056 充电状态指示（充电中=LOW）|
| **4** | 📗 MAX98357 SD_MODE | OUT | - | - | 采样率模式选择（I2S_SD 信号，接 MAX98357 Pin4，非 I2S 数据流）|
| **5** | 🔊 I2S_DIN | OUT | - | - | I2S 数据输入到 MAX98357 |
| **6** | 🎵 I2S_BCLK | OUT | - | - | I2S 位时钟 |
| **7** | 🎵 I2S_LRC | OUT | - | - | I2S 左右声道时钟 |
| **8** | 🖵 TFT_SCL | OUT | - | - | TFT SPI 时钟线 |
| **9** | ▶️ KEY_PLAY | IN | 上拉 | - | 播放/暂停键（经 SW4）|
| **10** | 💾 SD_CS | OUT | - | - | SD 卡片选 |
| **11** | 💾 SD_MOSI | OUT | - | - | SD 卡 MOSI（接 U3.3 CMD）|
| **12** | 💾 SD_SCLK | OUT | - | - | SD 卡时钟（接 U3.5 CLK）|
| **13** | 💾 SD_MISO | IN | - | - | SD 卡 MISO（接 U3.7 DAT0）|
| **14** | 🛑 KEY_STOP | IN | 上拉 | - | 停止键（经 SW7）|
| **15** | 💡 TFT_BLK | OUT | - | PWM | 屏幕背光控制（PWM 调光） |
| **16** | 🖵 TFT_DC | OUT | - | - | 屏幕数据/命令选择 |
| **17** | 🖵 TFT_RES | OUT | - | - | 屏幕硬件复位 |
| **18** | 🖵 TFT_SDA | OUT | - | - | 屏幕数据线（SPI MOSI） |
| **21** | ⏮️ KEY_PREV | IN | 上拉 | - | 上一曲键（经 SW5）|
| **38** | 💾 SD_CD | IN | 上拉 | GPIO_INT | SD 卡在位检测 |
| **39** | 🔌 LCD_POW_EN | OUT | 下拉 | - | LCD 软件电源开关（PMOS 控制） |
| **40** | 🔌 POW_EN | OUT | - | - | 系统电源保持锁存 |
| **41** | ⏩ KEY_FF | IN | 上拉 | - | 快进键（经 SW2）|
| **42** | ⏪ KEY_REV | IN | 上拉 | - | 快退键（经 SW6）|
| **47** | ⏭️ KEY_NEXT | IN | 上拉 | - | 下一曲键（经 SW3）|
| **48** | 🌈 WS2812 | OUT | - | - | RGB 状态指示灯（1.8V 域） |

**GPIO 冲突与约束检查**：
- ✅ **无电气冲突**：所有 GPIO 功能分配互不冲突
- ⚠️ **IO15 = TFT_BLK**：虽为 Strapping 引脚（MTDI），但用作 PWM 输出控制背光，启动时默认高阻态不影响 boot
- ⚠️ **IO48 = WS2812**：属于 VDD_SPI 1.8V 域，WS2812 需要电平转换或选用 1.8V 兼容型号
- ⚠️ **IO1 = BAT_DET (ADC)**：使用 ADC1_CH0，注意不要同时配置为普通 GPIO 输入
- ✅ **IO21/IO41/IO42/IO47** 用于按键：非 Strapping 引脚，安全
- ✅ **IO0 = BOOT**（SW8）：Strapping 引脚，按键上拉 + 默认高电平启动进入正常模式
- 📌 **IO3 / IO46 未连接**：原理图 V1 网表中这两个 GPIO 没有任何 `*SIGNAL*` 连接，留作 NC；旧文档中曾列为"KEY_STOP 双源 / KEY_PREV 双源"已确认不实

### 3.3 电源设计

#### 3.3.1 电源架构框图

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
                                    ┌────────▼────────┐
                                    │  MX66100T 双路   │
                                    │  电源开关 IC     │
                                    │  (按键开机/软关机)│
                                    └────────┬─────────┘
                                             │ SYS_3.7V
                       ┌─────────────────────┼─────────────────────┐
                       ▼                     ▼                     ▼
              ┌─────────────────┐   ┌─────────────┐        ┌─────────────┐
              │ MT3608 升压     │   │ MAX98357 VIN │        │ AO3401 PMOS │
              │ 3.7V → 5V       │   │ (5V VBUS)    │        │ LCD_POW_SW  │
              │ (供功放+USB)     │   └─────────────┘        │ IO39 控制   │
              └────────┬────────┘                          └──────┬──────┘
                       │ 5V_VBUS                                     │ 3V3_LCD
                       ▼                                            ▼
              ┌─────────────────┐                           ┌─────────────┐
              │ BL8039 降压     │                           │ SPI TFT LCD │
              │ 5V → 3.3V       │                           │ (独立供电)   │
              │ (供 MCU+外设)   │                           └─────────────┘
              └────────┬────────┘
                       │ +3V3_MCU
                       ▼
              ┌─────────────────────────────────────┐
              │ ESP32-S3 + MicroSD + 按键 + 运放     │
              └─────────────────────────────────────┘
```

#### 3.3.2 电源架构说明

| 电源轨 | 电压 | 来源 | 供电对象 |
|--------|------|------|---------|
| **VBUS** | 5.0V | MT3608 (升压) | MAX98357 功放、USB 接口 |
| **+3V3_MCU** | 3.3V | BL8039 (降压) | ESP32-S3、MicroSD、LMV321、按键网络、WS2812 |
| **3V3_LCD** | 3.3V | AO3401 PMOS (受控) | SPI TFT LCD 屏幕（可软件断电） |

#### 3.3.3 软件电源开关

- **系统 POW_EN (IO40)**：控制 MX66100T 双路开关的锁存电路（Q2=2N7002 NMOS），实现软件关机功能
- **LCD_POW_EN (IO39)**：控制 Q3=AO3401A PMOS，实现 LCD 屏幕独立电源开关
  - IO39 = LOW → PMOS 导通 → LCD 上电
  - IO39 = HIGH → PMOS 关断 → LCD 断电（待机省电）
  - 默认状态（MCU 未初始化）：R47(100K) 下拉 → LCD 默认上电

#### 3.3.4 电流预算

| 模块 | 工作电流 | 峰值 | 电源轨 |
|------|---------|------|--------|
| ESP32-S3 (WiFi/BT 关) | 50 mA | 240 mA (瞬态) | +3V3_MCU |
| MAX98357 (中等音量) | 80 mA | 250 mA (峰值音量) | VBUS (5V) |
| SPI TFT LCD (背光 50%) | 40 mA | 80 mA (全亮) | 3V3_LCD |
| SPI TFT LCD (休眠断电) | **0 mA** | — | 软件关断 |
| MicroSD 读写 | 30 mA | 100 mA | +3V3_MCU |
| LMV321 运放 | < 1 mA | 1 mA | +3V3_MCU |
| WS2812 LED (单色) | 12 mA | 60 mA (全白) | +3V3_MCU |
| **合计（正常播放）** | **~213 mA** | **~620 mA** | |
| **合计（LCD 断电）** | **~173 mA** | **~540 mA** | |

2600 mAh 电池理论续航：
- 正常播放（含 LCD）：2600 / 213 ≈ **12 小时**
- LCD 断电纯音频：2600 / 173 ≈ **15 小时**
- 实际约打 7 折：**8-10 小时**

### 3.4 关键电路

#### 3.4.1 MAX98357 配置

| 引脚 | 连接 | 含义 |
|------|------|------|
| BCLK | IO6 (I2S_BCLK) | 位时钟 |
| LRC | IO7 (I2S_LRC) | 左右声道时钟 |
| DIN | IO5 (I2S_DIN) | 数据输入 |
| SD | IO4 (I2S_SD) | MAX98357 **SD_MODE** 采样率模式选择脚（接 Pin4，非 I2S 数据流，作普通 GPIO 输出设定电平）|
| GAIN | GND | 12 dB 增益（MAX98357AETE+T） |
| SD_MODE | IO4 (I2S_SD) | 由 MCU GPIO4 控制：拉高 = 原 VDD 意图的采样率模式（与单声道混合一致）；拉低为另一模式 |
| VIN | 5V (VBUS) | 由 MT3608 升压供电，输出功率可达 3W |

#### 3.4.2 SD 卡 SPI 接口

- 使用 **SPI2_HOST**（避开 SPI0/SPI1 保留给 Flash/PSRAM）
- DMA 通道：SPI_DMA_CH_AUTO
- 最高时钟：40 MHz（普通卡）/ 20 MHz（低速卡兼容）
- CS 信号：软件管理（IO10）
- CD 在位检测：IO38（卡插入时为指定电平）

SD 卡座（MicroSD 推推式座，位号 **U3**）通过 SPI 模式与 MCU 相连，使用 **SPI2_HOST**；TFT 显示屏则使用**独立的 SPI3_HOST**（SCLK=IO8 / MOSI=IO18，与 SD 的 IO12/IO11/IO13 引脚完全不同源），**两条 SPI 总线互不共用、可并行操作**。SD 卡 CS 为 IO10（软件管理），而 TFT 的 CS（J2.7）由 R46(10K) **下拉至 GND**（CS 低电平有效，故恒为选通/恒选）、不占用 MCU GPIO（详见 3.4.3 节 J2 引脚表 Pin 7）：

| U3 引脚 | 信号 | GPIO | 方向 | 说明 |
|---------|------|------|------|------|
| 1 | DAT2 | NC | — | SPI 模式未使用 |
| 2 | DAT3 / CS | IO10 | OUT | 片选（软件管理，SD_CS），网表信号 `SD_CS` |
| 3 | CMD | IO11 | OUT | 命令/数据输入 MOSI（SD_MOSI），网表接 `U3.3` |
| 4 | VDD | — | — | 3.3V 供电（来自 +3V3_MCU）|
| 5 | CLK | IO12 | OUT | 时钟 SCLK（SD_CLK），网表接 `U3.5` |
| 6 | VSS | — | — | 地 |
| 7 | DAT0 | IO13 | IN | 数据输出 MISO（SD_MISO），网表接 `U3.7` |
| 8 | DAT1 | NC | — | SPI 模式未使用 |
| 检测簧片 | CD | IO38 | IN | 卡在位检测（SD_CD，上拉），网表信号 `SD_CD` |

> 注：U3 具体封装脚位与 CD 检测簧片物理脚号以原理图 PDF 零件封装为准；上表"信号 → GPIO"映射与 V1 网表（`hardware/V1/audio_player.txt`）一致。此前的文档版本未列出本引脚表，已补齐。

#### 3.4.3 SPI TFT 显示接口

显示屏通过 8pin XH1.5 连接器 (J2) 与 MCU 相连：

| J2 引脚 | 信号 | GPIO | 方向 | 说明 |
|---------|------|------|------|------|
| Pin 1 | GND | — | — | 地 |
| Pin 2 | +3V3_LCD | — | — | 受 PMOS 控制的屏幕电源 |
| Pin 3 | TFT_SCL | IO8 | OUT | SPI 时钟 |
| Pin 4 | TFT_SDA | IO18 | OUT | SPI 数据 (MOSI) |
| Pin 5 | TFT_RES | IO17 | OUT | 硬件复位（低有效） |
| Pin 6 | TFT_DC | IO16 | OUT | 数据/命令选择 |
| Pin 7 | TFT_CS | — | — | 片选（R46 10K 下拉到 GND，常选——CS 低电平有效故恒为选通） |
| Pin 8 | TFT_BLK | IO15 | OUT(PWM) | 背光控制（可 PWM 调光） |

#### 3.4.4 按键电路

6 个功能按键（PLAY/STOP/PREV/NEXT/FF/REV）+ 1 个 Boot/Reset 键（SW8），共 7 个实体按键，均为"上拉→串联电阻→按键→GND"结构：

```
       +3V3_MCU
          │
         Rpu (10KΩ)
          │
          ├────► GPIOx (ESP32)
          │
         Rs (1KΩ)  ← 串联电阻（限流/去抖）
          │
         SWx (轻触开关)
          │
         Cfilt (100nF)  ← 滤波电容（并联到 SW-GND 侧）
          │
         GND
```

按下时 GPIO 读 0（低电平），松开时读 1（高电平）。外部 10K 上拉 + 100nF 滤波确保稳定。

| 按键 | GPIO | 原理图标号 | 功能 |
|------|------|-----------|------|
| SW2 | IO41 | KEY_FF | 快进 |
| SW3 | IO47 | KEY_NEXT | 下一曲 |
| SW4 | IO9 | KEY_PLAY | 播放/暂停 |
| SW5 | IO21 | KEY_PREV | 上一曲 |
| SW6 | IO42 | KEY_REV | 快退 |
| SW7 | IO14 | KEY_STOP | 停止 |
| SW8 | IO0 | BOOT | Boot/Reset（长按关机） |

#### 3.4.5 电池电压检测电路

```
  BAT+ (3.0~4.2V)
       │
       R_top (— 分压网络 —)
       │
       ├────► LMV321 (U10) 同相比例放大
       │       │
       │       └────► IO1 (BAT_DET, ADC1_CH0)
       │
  GND ─┴──── ─ ─ ─ ─ ─ ─ ─ ─
```

- 采用 **LMV321 运放** 作同相比例放大（非简单分压），提高测量精度和输入阻抗
- 输出接 **IO1 (ADC1_CH0)**，ADC 参考电压 ~1.1V
- 映射关系：18650 满充 4.2V → ADC 高位；截止 3.0V → ADC 低位
- 运放由 +3V3_MCU 供电

#### 3.4.6 WS2812 RGB LED

- 驱动引脚：**IO48**（U1 门引脚 Pin25）
- ⚠️ IO48 属于 VDD_SPI 1.8V 域，WS2812 标准逻辑电平为 3.3V/5V
- 建议：选用 1.8V 逻辑兼容型号，或添加电平转换电路（如 74HCT245）
- 独立供电：+3V3_MCU
- 用途：播放状态指示（呼吸/闪烁/颜色编码）

### 3.5 PCB 布局注意事项

1. **SD 卡座**：远离 MAX98357 模拟部分和 SPI TFT 高速信号线，避免数字噪声耦合
2. **I2S 信号线**：BCLK/LRC/DIN 等长走线（±5mm 内），避免 90° 直角，远离开关电源（MT3608）
3. **电源分区**：
   - 模拟地/数字地单点接地（AGDG/DGND 在 LMV321 下方汇合）
   - MT3608（升压）远离音频和 ADC 电路，防止开关噪声耦合
   - BL8039（3.3V LDO）靠近 ESP32-S3 放置，退耦电容紧靠电源引脚
4. **喇叭走线**：短而粗（≥0.5mm），差分走线（若用 BTL 模式），避开敏感信号
5. **TFT SPI 走线**：SDA/SCL/DC/RES 等长匹配，避免跨分割平面
6. **天线区域**：ESP32-S3 板载天线下方及周围 5mm 内不走线、不铺铜

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
| `display` | SPI TFT LCD 界面绘制 | `init/update/show_*` | esp_lcd(ST7789)/LVGL |
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
#define TAPE_ACCEL_STEP2_MS  2000   // 2.0s 进入 2.0x
#define TAPE_ACCEL_STEP3_MS  4000   // 4.0s 进入 3.0x
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
| 快进 2.0x | 2.0x | 88200 Hz | 不跳帧 | 叽叽喳喳 |
| 快进 3.0x | 3.0x | 132300 Hz | 不跳帧 | 快速扫描 |
| 快进 8.0x | 8.0x | 176400 Hz（I2S 上限 4.0×44.1k） | 每 50ms 跳 350ms（跳 7/8） | 极速 |
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

### 5.6 SPI TFT LCD 显示（display）

#### 5.6.1 屏幕规格与接口

| 项目 | 规格 |
|------|------|
| 接口类型 | SPI TFT（非 I2C OLED），走 **SPI3_HOST**（独立于 SD 卡的 SPI2_HOST） |
| 面板 | ST7789 控制器，物理分辨率 240×320（2.0 寸） |
| 逻辑分辨率 | 由 `config.h` 的 `DISPLAY_ORIENTATION` 决定：0=横屏 320×240（默认），1=竖屏 240×320；方向由 esp_lcd 的 `swap_xy/mirror` 在初始化时设置，后期可切换 |
| 连接器 | XH1.5-8P (J2) |
| 控制线 | CS(接地恒选), DC, RES, SCL(CK), SDA(MOSI), BLK(PWM) |
| 背光 | IO15 PWM 调光（可软件控制亮度/彻底关闭） |
| 驱动方案 | ESP-IDF 原生 `esp_lcd`：`esp_lcd_new_panel_io_spi`（IO 层）+ `esp_lcd_new_panel_st7789`（面板驱动，IDF v5.x 内置，零第三方） |
| GUI 框架 | **LVGL v9**（idf component manager 拉取），`flush_cb` 底层为 `esp_lcd_panel_draw_bitmap` |

#### 5.6.2 屏幕布局

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
│  12:35 / 45:00             [2.0x]    │  时间/速度 (h=8, y=48)
├──────────────────────────────────────┤  y=50
│ RW  ◀◀  ▶  ■  ▶▶  FF   [VOL-/+]     │  按键提示 (h=10, y=60)
└──────────────────────────────────────┘  y=64 (或更高，取决于实际分辨率)

> 注：状态栏图标（L=锁定、B=电量、V=音量、→=顺序播放）为 ASCII 示意。TFT 可显示更丰富的图形和颜色。
```

#### 5.6.3 渲染策略

- **驱动库**：ESP-IDF 原生 `esp_lcd`（`esp_lcd_panel_st7789`，IDF v5.x 内置），不再使用 u8g2 / 自写 SPI 驱动
- **GUI 框架**：LVGL v9，draw buffer 置于 PSRAM，`flush_cb` 调用 `esp_lcd_panel_draw_bitmap`（DMA 刷屏）
- **重绘机制**：LVGL 脏区（invalidate）机制自动局部重绘；业务层仍按 200ms 节奏更新数据
- **文件滚动**：长文件名使用 LVGL label 的 `LV_LABEL_LONG_SCROLL_CIRCULAR` 滚动模式
- **进度条**：LVGL `lv_bar` widget，数据变化时 `lv_bar_set_value`
- **字体**：LVGL 内置 Montserrat 字体（中文字体可后续通过 `lv_font_conv` 生成）
- **背光管理**：
  - 正常播放：PWM 50%~100% 亮度
  - 无操作 2 分钟：PWM 渐暗至 0% 或直接关闭
  - 深度休眠前：IO39 = HIGH → PMOS 关断 LCD 电源（零功耗）
  - 唤醒时：IO39 = LOW → PMOS 导通 → LCD 上电 → RES 复位 → 重新初始化

#### 5.6.4 显示状态机

```
       ┌──────────┐ 开机完成 ┌──────────┐
       │  SPLASH  ├─────────►│  MAIN    │
       └──────────┘          └─────┬────┘
                                    │
                                    ├── 长时间无操作 → DIM_BACKLIGHT
                                    │                  └─ 按键唤醒 → MAIN
                                    │
                                    ├── 更长时间无操作 → LCD_POW_OFF
                                    │                  └─ 按键唤醒 → 重新初始化LCD → MAIN
                                    │
                                    ├── 长按 STOP → BROWSE (文件夹浏览)
                                    │                  └─ 短按 STOP → MAIN
                                    │
                                    └── 长按 PLAY → LOCKED
                                                       └─ 长按 PLAY → MAIN
```

> 与旧版差异：新增 **DIM_BACKLIGHT**（背光渐暗）和 **LCD_POW_OFF**（PMOS 彻底断电）两个低功耗状态，利用 IO39 软件电源开关实现。

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
// LVGL 实现：lv_bar widget，范围 0~1000（千分比），数据变化时更新
void update_progress_bar(lv_obj_t *bar, int current_sec, int total_sec) {
    if (total_sec <= 0) {
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        return;
    }
    int permille = (int)(1000LL * current_sec / total_sec);
    lv_bar_set_value(bar, CLAMP(permille, 0, 1000), LV_ANIM_OFF);
}
```

### 7.4 跳帧距离计算

| 速度 | 50ms 内跳过的秒数 | 计算依据 |
|------|----------------|---------|
| 1.5x | 0.075 秒 | 50ms × 1.5 |
| 2.0x | 0.1 秒 | 50ms × 2.0 |
| 3.0x | 0.15 秒 | 50ms × 3.0 |
| 8.0x | 0.35 秒 | 50ms × (8-1)（跳帧模式） |
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
│   ├── u8g2/                   # （已废弃，待删除）原 u8g2 源码，显示层已迁移到 LVGL
│   └── audio_board/            # 覆盖 ADF 自带 audio_board 的自定义 tapebook 板
└── managed_components/         # 自动管理的依赖（idf component manager，含 lvgl）
```

#### 10.1.1 自定义 ADF Board（tapebook）

项目通过 `EXTRA_COMPONENT_DIRS = $ENV{ADF_PATH}/components` 引入 ESP-ADF。
IDF 组件解析**优先**搜索项目自身 `components/`，因此在 `components/audio_board`
建立同名组件即可**覆盖** ADF 自带的 `audio_board`，无需改动全局 ADF 安装：
方案可随仓库版本化、不受 ADF 更新影响。

- `components/audio_board/tapebook/` 为自定义板，仅提供编译通过的桩实现：
  项目使用自写 I2S/LCD/SD 驱动，`audio_board_init()` 实际不被调用。
- `tapebook/board_def.h` 必须定义 `ESP_SD_PIN_*` 全套宏（供 `esp_peripherals/sdcard.c`
  编译）与 `BOARD_PA_GAIN`（供 ADF `audio_hal` 驱动编译），引脚值置 -1 / 0，运行时不生效。
- 选板在 `configs/sdkconfig.defaults.*` 中由 `CONFIG_ESP_TAPEBOOK_BOARD=y` 指定，
  不再依赖任何现成开发板（如 `esp32_s3_box_3`、`lyrat_v4_3`）。

> 详细计划与实施步骤见 `docs/PLAN_TAPEBOOK_ADF_BOARD.md`。

### 10.2 依赖管理

```bash
# 1. ESP-IDF v5.3（推荐 LTS 稳定版）
git clone --recursive https://github.com/espressif/esp-idf.git -b v5.3

# 2. ESP-ADF v2.7（音频框架）
git clone --recursive https://github.com/espressif/esp-adf.git -b v2.7

# 3. LVGL v9（GUI 图形库）
# 经 idf component manager 自动下载（main/idf_component.yml: lvgl/lvgl >=9.0.0）
# ST7789 驱动使用 IDF 内置 esp_lcd 组件（esp_lcd_panel_st7789），无需第三方
```

> **兼容性提醒**：ESP-ADF 版本与 ESP-IDF 版本有严格对应关系。若 `v2.7` 编译时报告 IDF 版本不匹配，请优先使用 Espressif 官方推荐的 IDF 分支（例如 ADF 的 `idf_v5.x` 兼容分支）。

### 10.3 编译烧录流程

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 配置（启用 ADF、LVGL）
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
T+2000 FF保持 HOLD         mode=FF,gear=2   speed=2.0x          叽叽喳喳
T+4000 FF保持 HOLD         mode=FF,gear=3   speed=3.0x          快速扫描
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
- [x] 显示层迁移到原生 esp_lcd (ST7789) + LVGL v9（Phase 1 完成；Phase 2 UI 细化、Phase 3 删除 u8g2 残留待办）
- [ ] 集成 ESP-ADF 并验证管道
- [ ] 编写单元测试（按键状态机、tape_control 档位）
- [ ] 制作 3D 打印外壳图纸

### 11.6 变更记录

| 版本 | 日期 | 作者 | 变更 |
|------|------|------|------|
| 1.0 | 2026-07-02 | — | 初稿：基于 PRD V1.0 编写 |
| 1.1 | 2026-07-02 | — | 评审修订：修正目标模块为 ESP32-S3-WROOM-2 Octal PSRAM；删除重复的 3. 硬件设计章节；修正磁带快进时序图；修正 I2S 采样率与快进/快退表；修正 seek API 与跳帧代码示例；修正功耗与电池 ADC 说明；澄清分区表与 OTA 布局 |
| 1.2 | 2026-07-02 | — | 评审修订 V2：系统框图 SRAM 512KB→~400KB；MAX98357 SD_MODE GND→VDD；AMS1117 500mA→800mA；任务优先级修正(main=1, scan 在主循环内)；按键状态机增加双击/超长按；playlist 改为结构体绑定排序+PSRAM；跳帧策略仅≥4x；配置速查表增加双击窗口/超长按阈值 |
| **1.3** | **2026-07-28** | **—** | **原理图对齐修订（基于 PADS Logic V9.5 网表权威数据）：模块型号 WROOM-1 替代 WROOM-2；系统框图更新为 SPI TFT+WS2812 替代 OLED+耳机/编码器；GPIO 分配表完全重写（I2S:IO5/6/7、TFT:IO8/15/16/17/18、按键:IO3/9/14/41/42/46/47、BAT_DET:IO1、CHRG:IO2 等）；电源架构更新为 TP4056→MT3608→BL8039 三级拓扑+MX66100T 软开关+LCD PMOS 独立供电；新增 LCD_POW_EN(IO39)、POW_EN(IO40)、WS2812(IO48)、SD_CD(IO38) 引脚定义；显示模块从 I2C OLED 改为 SPI TFT（含 DC/RES/BLK/CS 四控制线）；按键电路增加串联电阻+滤波电容规格；电池检测从简单分压改为 LMV321 运放方案；PCB 布局增加电源分区和天线区域约束** |
| **1.4** | **2026-07-29** | **—** | **硬件映射勘误（基于 V1 网表复核）：① SD 卡 GPIO12/13 引脚纠正——GPIO12=SD_SCLK、GPIO13=SD_MISO（原文档写反）；② 按键映射纠正——`KEY_STOP` 实际经 SW7→IO14、`KEY_PREV` 实际经 SW5→IO21，删除原错的 IO3(SW2)/IO46(SW5) 双源说法，GPIO 表移除 IO3/IO46 行、新增 IO21 行；③ 补齐 3.4.2 节 SD 卡座（U3）SPI 引脚详述表；④ 同步修正固件 `board_def.h` 中 ESP_SD_PIN_CLK/D0 的反接（CLK=12、D0=13）；⑤ 固件 I2S 引脚按网表修正（BCLK=6/WS=7/DIN=5），并在 `main/config.h`+`main/main.cpp` 新增 GPIO4(SD_MODE) 输出初始化（拉高）** |\n| **1.5** | **2026-07-29** | **—** | **按键 SW 位号勘误（GPIO↔功能网络映射无误，仅 SW 位号标注错误）：按权威确认重排按键表——SW2=KEY_FF(IO41)、SW3=KEY_NEXT(IO47)、SW4=KEY_PLAY(IO9)、SW5=KEY_PREV(IO21)、SW6=KEY_REV(IO42)、SW7=KEY_STOP(IO14)、SW8=BOOT(IO0)。原 1.4 中 SW3=PLAY/SW4=FF/SW9=REV 的 SW 位号有误（SW9 非按键），已修正；GPIO 表相应更新"经 SWx"注释** |
| **1.6** | **2026-07-29** | **—** | **网表全量一致性复核（基于 `hardware/V1/audio_player.txt` 元件定义 + `*SIGNAL*` 段）：逐信号核对 U1(ESP32-S3-WROOM1) 引脚→GPIO 映射，确认 KEY/SD/I2S/TFT/电源/检测全部一致；发现并修正 3.4.2 节误写的"TFT IO37"——网表证实 TFT_CS(J2.7) 经 R46(10K) 下拉至 GND 恒选、U1.30(IO37) 未接任何网络，与 J2 引脚表 Pin7 及 GPIO 表（无 IO37 行）一致，故删除"TFT IO37"表述** |\n| **1.7** | **2026-07-29** | **—** | **代码-文档一致性收尾（依据代码修正 DESIGN）：① 显示规格更正为 ST7789 控制器、320×240（横屏）；② 磁带速度档位依代码改为 1.5x/2.0x/3.0x/8.0x（原误写 2.5x/4.0x），同步修正采样率/跳帧/时序表；③ 默认目标模块改为 ESP32-S3-WROOM-1 N16R8（Kconfig 默认 + 本文档模块型号/PSRAM/WS2812 电平域），Kconfig `USE_U8G2` 帮助文本由 I2C OLED 改为 SPI TFT 离线渲染；④ 固件补全 WS2812(IO48) 驱动（led_strip/RMT，状态色：蓝=充电/红=极低/橙=低/绿=播放/灭=空闲）；⑤ 清理死代码：移除未引用的 `u8g2_esp32_hal` 与 `tapebook_board` 组件及其 CMake 依赖，audio_player.cpp 过时注释 4.0x→8.0x** |
| **1.8** | **2026-07-29** | **—** | **显示层迁移到原生 esp_lcd + LVGL（Phase 1）：① 弃用自写 ST7789 SPI 驱动与 u8g2 1bpp 离线渲染（"esp_lcd_st7789 不存在"的前提在 IDF v5.5.3 已过时——`esp_lcd_panel_st7789` 为 IDF 内置）；② `display.cpp` 重写为 `esp_lcd_new_panel_io_spi`(SPI3_HOST) + `esp_lcd_new_panel_st7789` + LVGL 9.5.0（idf component manager 拉取），保留原 `display_*` API；③ `config.h` 新增 `DISPLAY_ORIENTATION` 方向开关（0=横屏 320×240 默认 / 1=竖屏 240×320，方向经 `swap_xy/mirror` 设置，后期可改）；④ 新增 `main/lv_conf.h`（RGB565/16 位色深/FreeRTOS）；⑤ `main/CMakeLists.txt` REQUIRES 去 u8g2、加 lvgl+esp_lcd，`idf_component.yml` 加 `lvgl/lvgl >=9.0.0`；⑥ 勘误 3.4.2 节"SD 与 TFT 共用 SPI2_HOST"表述——实际 TFT 走独立 SPI3_HOST（IO8/IO18），与 SD 的 SPI2_HOST（IO10-13）互不共用；⑦ 另：ADF 选板改为项目内 `components/audio_board` 覆盖的自定义 tapebook 板（`CONFIG_ESP_TAPEBOOK_BOARD=y`，见 §10.1.1）。详见 `docs/PLAN_LVGL_ESP_LCD_MIGRATION.md`** |
