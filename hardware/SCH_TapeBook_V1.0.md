# TapeBook V1.0 原理图设计规格书

> **项目**: TapeBook — 磁带机风格听书机  
> **主控**: ESP32-S3-WROOM-1 N16R8 (16MB Quad Flash + 8MB Octal PSRAM)  
> **文档版本**: 1.0  
> **创建日期**: 2026-07-09  
> **对应硬件版本**: HARDWARE_PIN_WIRING.md v1.1  
> **关联文件**: `SCH_TapeBook_V1.0.net`（Protel 网表）, `SCH_TapeBook_V1.0_BOM.csv`（BOM）

---

## 原理图共 6 页

| 页码 | 标题 | 内容 |
|------|------|------|
| 1 | 电源系统 | USB Type-C、TP4056 充电、AMS1117-3.3 稳压、电源开关、电池保护 |
| 2 | ESP32-S3 最小系统 | WROOM-1 模组、USB Serial/JTAG、EN 复位、BOOT 模式、去耦 |
| 3 | I2S 音频功放 | MAX98357A、喇叭输出、耳机座 |
| 4 | MicroSD 卡 (SPI) | MicroSD 座、SPI 上拉、信号完整性 |
| 5 | OLED 显示屏 (I2C) | SSD1306/SSD1315 OLED、I2C 上拉 |
| 6 | 用户输入 | 6 按键、EC11 编码器(可选)、电池 ADC(可选) |

---

## Sheet 1: 电源系统

### 1.1 电源树

```
USB 5V (Type-C VBUS)
   │
   ├──→ TP4056 VCC (充电输入)
   │        │
   │        ├──→ BAT: 18650 锂电池 3.7V 2600mAh (带保护板)
   │        └──→ PROG: R_PROG → GND (设定充电电流)
   │
   ├──→ 电源开关 SS12D00 (总电源 ON/OFF)
   │        │
   │        └──→ AMS1117-3.3 VIN
   │                │
   │                ├──→ 3.3V 主电源轨
   │                └──→ MAX98357 VIN (可选从 USB 5V 取电以获得更大功率)
   │
   └──→ [可选] MAX98357 VIN (从 5V 取电 → 3W 输出)
```

### 1.2 TP4056 锂电池充电电路

```
                  TP4056 (ESOP-8)
     +5V ─┬────── VCC(4) ──┐
          │                 │
         ┌┴┐              100µF/10V
         │ │ 10kΩ          │
         └┬┤              GND
          │  CHRG(1) ── 10kΩ ── LED_CHG (红色，充电中亮)
          │  STDBY(7)── 10kΩ ── LED_FULL (绿色，充满亮)
          │
          │  PROG(5)── R_PROG(1.2kΩ)── GND
          │            (1.2kΩ → 充电电流 ≈ 1000mA)
          │
          │  BAT(6)──┬──→ 18650 电池正极 (BAT+)
          │          │
          │         100µF/10V
          │          │
          │         GND
          │
          TEMP(2)── NTC 网络 (不需要时悬空)
          GND(3,8)── GND
```

**R_PROG 选值**:

| R_PROG (kΩ) | I_CHG (mA) |
|-------------|-----------|
| 1.2 | 1000 |
| 2.4 | 500 |
| 4.7 | 260 |

### 1.3 18650 电池保护板（DW01 + FS8205）

> 建议直接采购带保护板的 18650 电池成品，或集成在 PCB 上。

```
               DW01 (SOT-23-6)          FS8205 (TSSOP-8)
        ┌──────────────┐          ┌──────────────┐
BAT+ ───┤VCC(5)  OD(1) ├──────────┤S1(4) D1(5)  ├───→ PACK+
BAT- ───┤GND(4) OC(3) ├──────────┤S2(3) D2(6)  ├───→ PACK-
        │      CS(2)──┼──────────┤GND(2) GND(7)│
        └──────────────┘          └──────────────┘

PACK+ ───→ SS12D00 公共端 (总电源输入)
PACK- ───→ GND
```

### 1.4 电源开关 & LDO

```
         SS12D00 (拨动开关, SPDT)
    PACK+ ────┬── COM
              │
              └── ON ────→ AMS1117-3.3 VIN(3)
                           │
      AMS1117-3.3        1kΩ
      ┌────────────┐       │  (下拉到 GND 防止浮空)
      │ VIN(3) OUT(2)├─┬───┴── GND
      │ TAB/GND(4) │  │
      │ VOUT(1) NC │  │
      └────────────┘  │
           │          │
          GND        ┌┴┐
                     │ │ 10kΩ (EN 上拉到 3.3V)
                     └┬┤
                      │ 3.3V
                      │
                      ▼
             3.3V 主电源轨
```

**去耦电容配置**:

| 位置 | 电容值 | 数量 |
|------|--------|------|
| TP4056 VCC → GND | 100µF/10V 电解 + 100nF 陶瓷 | 各1 |
| TP4056 BAT → GND | 100µF/10V 电解 | 1 |
| AMS1117-3.3 VIN → GND | 10µF/16V 钽 + 100nF 陶瓷 | 各1 |
| AMS1117-3.3 OUT → GND | 10µF/16V 钽 + 100nF 陶瓷 | 各1 |
| 3.3V 轨 每 2 个 IC 一组 | 100nF 陶瓷 × 每组 | 每组 |

---

## Sheet 2: ESP32-S3 最小系统

### 2.1 ESP32-S3-WROOM-1 模组

> 模组引脚编号基于官方参考设计 `SCH_ESP32-S3-WROOM-1_V1.3`

```
                  ESP32-S3-WROOM-1 (SMD 41-pin + EPAD)
                  ┌─────────────────────────────────────┐
                  │                                     │
                  │  Pin 1 (右上角标记点)                │
                  │  ┌───┬──────┬───────┬──────┬───┐    │
                  │  │  1│ GND  │ GND   │ 41   │   │    │
                  │  │  2│ 3V3  │ GPIO4 │ 40   │   │    │
                  │  │  3│ EN   │ GPIO5 │ 39   │   │    │
                  │  │  4│ GPIO0│ GPIO6 │ 38   │   │    │
                  │  │  5│ GPIO1│ GPIO7 │ 37   │   │    │  ← Pin numbering
                  │  │  6│ GPIO2│ GPIO8 │ 36   │   │    │     (counterclockwise)
                  │  │  7│ GPIO3│ GPIO9 │ 35   │   │    │
                  │  │  8│ GPIO8│ GPIO10│ 34   │   │    │
                  │  │  9│ GPIO9│ GPIO11│ 33   │   │    │
                  │  │ 10│ GPIO10│GPIO12│ 32   │   │    │
                  │  │ 11│ GPIO11│GPIO13│ 31   │   │    │
                  │  │ 12│ GPIO12│GPIO14│ 30   │   │    │
                  │  │ 13│ GPIO13│GPIO15│ 29   │   │    │
                  │  │ 14│ GPIO14│GPIO16│ 28   │   │    │
                  │  │ 15│ GPIO15│GPIO17│ 27   │   │    │
                  │  │ 16│ GPIO16│GPIO18│ 26   │   │    │
                  │  │ 17│ GPIO17│GPIO19│ 25   │   │    │
                  │  │ 18│ GPIO18│GPIO20│ 24   │   │    │
                  │  │ 19│ GPIO21│ GPIO45│ 23  │   │    │
                  │  │ 20│ GPIO38│ GPIO46│ 22  │   │    │
                  │  │ 21│ GPIO39│ GPIO48│ 21  │   │    │
                  │  └───┴──────┴───────┴──────┴───┘    │
                  │                                     │
                  │              EPAD (GND)              │
                  └─────────────────────────────────────┘
                  
注：上图引脚编号为逻辑示意。实际 WROOM-1 N16R8 的物理引脚布局请参考乐鑫官方数据手册。
```

### 2.2 GPIO 分配表（Sheet 2 连接总览）

| 模组引脚# | 信号 | 接至 | 备注 |
|----------|------|------|------|
| Pin 2 | 3V3 | 3.3V 电源轨 | 模组主供电 |
| Pin 3 | EN | R=10kΩ → 3.3V + C=100nF → GND | RC 复位 |
| Pin 4 | GPIO0 | R=10kΩ → 3.3V + SW_BOOT → GND | 烧录模式 |
| Pin 5 | GPIO1 | SW_PLAY → GND | 内部上拉 |
| Pin 6 | GPIO2 | SW_STOP → GND | 内部上拉 |
| Pin 7 | GPIO3 | ADC 分压网络 → BAT+ / 悬空 | 可选电池检测 |
| Pin 8 | GPIO8 | SW_PREV → GND | 内部上拉 |
| Pin 9 | GPIO9 | SW_NEXT → GND | 内部上拉 |
| Pin 10 | GPIO10 | SD_CS | MicroSD 片选 |
| Pin 11 | GPIO11 | SD_MOSI | MicroSD MOSI |
| Pin 12 | GPIO12 | SD_MISO | MicroSD MISO |
| Pin 13 | GPIO13 | SD_SCLK | MicroSD SCK |
| Pin 14 | GPIO14 | SW_REW → GND | 内部上拉，快退 |
| Pin 15 | GPIO15 | SW_FF → GND | 内部上拉，快进 |
| Pin 16 | GPIO16 | **N/C** | 未使用，悬空 |
| Pin 17 | GPIO17 | OLED_SDA | I2C 数据 |
| Pin 18 | GPIO18 | OLED_SCL | I2C 时钟 |
| Pin 19 | GPIO21 | **N/C** | 未使用，悬空 |
| Pin 20 | GPIO38 | ENC_A (可选) | 编码器 A 相 |
| Pin 21 | GPIO39 | ENC_B (可选) | 编码器 B 相 |
| Pin 22 | GPIO48 | **N/C** | 1.8V 域，悬空 |
| Pin 23 | GPIO46 | **N/C** | Strapping，内部默认 |
| Pin 24 | GPIO20 | USB_D+ (Type-C D+) | USB Serial/JTAG |
| Pin 25 | GPIO19 | USB_D- (Type-C D-) | USB Serial/JTAG |
| Pin 26 | GPIO18 | (见 Pin 18) | — |
| Pin 27 | GPIO17 | (见 Pin 17) | — |
| Pin 28 | GPIO16 | N/C | — |
| Pin 29 | GPIO15 | (见 Pin 15) | — |
| Pin 30 | GPIO14 | (见 Pin 14) | — |
| Pin 31 | GPIO13 | (见 Pin 13) | — |
| Pin 32 | GPIO12 | (见 Pin 12) | — |
| Pin 33 | GPIO11 | (见 Pin 11) | — |
| Pin 34 | GPIO10 | (见 Pin 10) | — |
| Pin 35 | GPIO9 | (见 Pin 9) | — |
| Pin 36 | GPIO8 | (见 Pin 8) | — |
| Pin 37 | GPIO7 | I2S_MCLK / N/C | MAX98357 可选 MCLK |
| Pin 38 | GPIO6 | I2S_DOUT | MAX98357 DIN |
| Pin 39 | GPIO5 | I2S_WS | MAX98357 LRC |
| Pin 40 | GPIO4 | I2S_BCK | MAX98357 BCLK |
| Pin 41 | GND | GND | — |
| EPAD | GND | GND | 散热片 |

### 2.3 EN 复位电路

```
    3.3V ────┬── 10kΩ ──── EN (模组 Pin 3)
             │
            100nF
             │
            GND
```

> RC 时间常数 ≈ 1ms，确保上电后 VDD 稳定再释放 EN。

### 2.4 BOOT 模式选择

```
    3.3V ────┬── 10kΩ ──── GPIO0 (模组 Pin 4)  → 正常启动
             │
             SW_BOOT (轻触开关 6×6×5mm)
             │
            GND                          → 按住进下载模式
```

### 2.5 USB Serial/JTAG 接口

```
    USB Type-C (母座，16P)
    ┌─────────────────────────────┐
    │ A1 GND   B1 GND            │
    │ A2 D+    B2 D-             │
    │ A3 D-    B3 D+             │
    │ A4 VBUS  B4 VBUS           │
    │ A5 CC1   B5 CC2            │
    │ A6 DPLUS  B6 DPLUS         │
    │ A7 DN     B7 DN            │
    │ A8 SBU1  B8 SBU2           │
    └─────────────────────────────┘

    CC1 ─── 5.1kΩ ─── GND  (告诉源端是 5V 3A 模式)
    CC2 ─── 5.1kΩ ─── GND

    D+ ───┬───→ GPIO20 (模组 Pin 24, USB_DP)
          │
         27Ω (串联阻抗匹配)
          │
         ESD 保护 (TPD4E05U06 or similar)

    D- ───┬───→ GPIO19 (模组 Pin 25, USB_DN)
          │
         27Ω (串联阻抗匹配)
          │
         ESD 保护
```

> ESP32-S3 内置 USB Serial/JTAG 控制器，无需外部 USB-UART 桥接芯片。  
> GPIO19(USB_D-)、GPIO20(USB_D+) 不与其他功能复用。

### 2.6 去耦电容（模组附近）

| 位置 | 电容 | 数量 |
|------|------|------|
| 3V3 (Pin 2) → GND | 100nF + 10µF | 各1 |
| VBUS (Type-C) → GND | 1µF | 1 |
| 模组背面 EPAD | 多个 100nF 布满 | 2-4 |

---

## Sheet 3: I2S 音频功放

### 3.1 MAX98357A 连接图

```
                    MAX98357A (TQFN-20 / 模块)
        ┌─────────────────────────────────────┐
        │                                     │
  ┌─────┤ DIN(6)                              │
  │     │ BCLK(5)                             │
  │     │ LRC(4)                              │
  │     │                                     │
  │     │ MCLK(1) ── N/C (不需外部 MCLK)      │
  │     │                                     │
  │     │ VIN(9)──┬── 3.3V (或 5V 直连)       │
  │     │         │                           │
  │     │        4.7µF                        │
  │     │         │                           │
  │     │        GND                          │
  │     │                                     │
  │     │ GAIN(19)─── GND (12dB 增益)         │
  │     │                                     │
  │     │ SD_MODE(20)── 634kΩ ── 3.3V (Mono)  │
  │     │                                     │
  │     │ OUT+(12)──┬── 喇叭 +端              │
  │     │ OUT-(13)──┤── 喇叭 -端              │
  │     │           │                         │
  │     │           │  Speaker (3W 4Ω/8Ω)     │
  │     │           │                         │
  │     │ GND(3,7,8,10,14,15,16,17)─── GND    │
  │     │                                     │
  │     │ PGND(11,18)─── GND (电源地)         │
  │     └─────────────────────────────────────┘
  │
  ├──→ GPIO6(I2S_DOUT)
  ├──→ GPIO4(I2S_BCK)
  └──→ GPIO5(I2S_WS)
```

### 3.2 SD_MODE 偏置电阻选值

| VDD | 模式 | 电阻值 | 计算公式 |
|-----|------|--------|---------|
| 3.3V | Mono (L+R)/2 | **634 kΩ** | RLARGE = 222.2 × 3.3 - 100 = 633.3kΩ |
| 3.3V | Right only | 210 kΩ | RSMALL = 94.0 × 3.3 - 100 = 210.2kΩ |
| 3.3V | Left only | 直连 VDD | — |
| 3.3V | Shutdown | GND | < 0.16V |

> 本项目使用单喇叭，**SD_MODE → 634kΩ → 3.3V**，输出 (L+R)/2 混合单声道。

### 3.3 喇叭输出

```
    MAX98357
    OUT+(12)──┬─── 喇叭红色线 (+)
              │
              ┌┴┐
              │ │ 喇叭 3W 4Ω / 8Ω
              └┬┘ (Φ28mm, 钕磁)
              │
    OUT-(13)──┴─── 喇叭黑色线 (-)
```

### 3.4 [可选] 3.5mm 耳机座

```
              3.5mm 耳机座 (PJ-342)
              ┌──────────────┐
MAX98357      │               │
OUT+ ─┬──────┤ TIP (L)       │
      │      │ RING (R)      │
      │  ┌───┤ SLEEVE (GND)  │
      │  │   └──────────────┘
      │  │      │
      ┌┴┐┌┴┐   GND
      │ ││ │   (插入时 Tip/Ring 断开喇叭)
      │100Ω │
      └┬┘└┬┘
       │   │
MAX98357   │
OUT- ──────┘
```

> 注：耳机座可选用带机械开关的型号（插入时断开喇叭）。串联 100Ω 电阻保护耳机。

---

## Sheet 4: MicroSD 卡 (SPI)

### 4.1 MicroSD 卡座电路

```
                         MicroSD 卡座 (Push-Push, 9-pin)
                         ┌─────────────────────────────────┐
  SPI2_HOST              │                                 │
  GPIO10(SD_CS) ────────┤ CS (Pin 2)                      │
                         │                                 │
  GPIO11(SD_MOSI) ──────┤ MOSI/DI (Pin 3)                 │
                         │                                 │
  GPIO12(SD_MISO) ──────┤ MISO/DO (Pin 7)                 │
                         │                                 │
  GPIO13(SD_SCLK) ──────┤ SCLK (Pin 5)                    │
                         │                                 │
  3.3V ──────────────────┤ VCC (Pin 4)                    │
                         │                                 │
  GND ───────────────────┤ GND (Pin 6)                    │
                         │                                 │
                         │ CD/DAT3 (Pin 8)                 │
                         │   (卡检测，可选)                 │
                         │                                 │
                         │ DAT2 (Pin 1)                    │
                         │   (SPI 模式下未使用，悬空)       │
                         └─────────────────────────────────┘
```

### 4.2 上拉电阻

| 信号 | 上拉 | 位置 | 备注 |
|------|------|------|------|
| SD_CS | 10kΩ → 3.3V | 卡座 CS 引脚 | 防止 CS 浮空误操作 |
| SD_MOSI | 10kΩ → 3.3V | 卡座 DI 引脚 | 可选项，保证空闲状态确定 |
| SD_MISO | **不接** | — | 由 SD 卡驱动，外部不上拉 |
| SD_SCLK | **不接** | — | 推挽输出，不上拉 |
| CD/DAT3 | 10kΩ → 3.3V | 卡座 Pin 8 | 上拉保持高电平 |

### 4.3 信号完整性（可选）

```
    ESP32 ────┬── 22Ω ──── CS ───→ 卡座
              │
    ESP32 ────┬── 22Ω ──── MOSI ──→ 卡座
              │
    ESP32 ────────────── MISO ───← 卡座   (MISO 不串联)
              │
    ESP32 ────┬── 22Ω ──── SCK ───→ 卡座
              │
             GND

    建议在各信号与 GND 之间贴 10pF 电容 (靠近卡座侧) 以抑制 EMI。
```

> SD 卡 SPI 速率最高 40MHz，走线长度 < 5cm 时串联电阻可省略。

---

## Sheet 5: OLED 显示屏 (I2C)

### 5.1 SSD1306/SSD1315 OLED 连接

```
                  SSD1306 0.96" OLED 模块 (I2C, 128×64)
                  ┌─────────────────────────────────────┐
                  │                                     │
  I2C_NUM_0       │                                     │
  GPIO17(SDA) ────┤ SDA (Pin 3)                        │
                  │                                     │
  GPIO18(SCL) ────┤ SCL (Pin 2)                        │
                  │                                     │
  3.3V ───────────┤ VCC (Pin 1)                        │
                  │                                     │
  GND ────────────┤ GND (Pin 4)                        │
                  │                                     │
                  │                                     │
                  │ SA0/ADDR (Pin 5) ── GND             │
                  │   (I2C 地址 = 0x3C)                  │
                  │                                     │
                  │ RESET (Pin 6) ── N/C 或 3.3V        │
                  │   (部分模块内部已上拉)               │
                  └─────────────────────────────────────┘
```

### 5.2 I2C 上拉电阻

```
    3.3V                               3.3V
     │                                   │
    ┌┴┐                                 ┌┴┐
    │ │ 4.7kΩ (1%)                      │ │ 4.7kΩ (1%)
    └┬┘                                 └┬┘
     │                                   │
     ├────────────── SDA (GPIO17)        ├────────────── SCL (GPIO18)
     │                                   │
    ┌┴┐ 可选 100pF                      ┌┴┐ 可选 100pF
    │ │ (EMI 滤波)                      │ │ (EMI 滤波)
    └┬┘                                 └┬┘
     │                                   │
    GND                                 GND
```

> I2C 上拉电阻选值：400kHz 时 4.7kΩ 标准值。若走线 > 10cm 可改用 2.2kΩ。

---

## Sheet 6: 用户输入

### 6.1 6 个按键（共用电路）

```
    所有 6 个按键使用相同电路：
    
    3.3V ──── 22Ω ──── GPIOx   (22Ω 限流，ESD 保护)
              (内部上拉 ≈ 10kΩ)
                        │
                        │
                      ┌─┴─┐
                      │   │ 轻触开关 6×6×5mm
                      │   │
                      └─┬─┘
                        │
                       GND

    GPIO 映射:
      SW_PLAY  → GPIO1  (BTN_PLAY_PAUSE)
      SW_STOP  → GPIO2  (BTN_STOP)
      SW_PREV  → GPIO8  (BTN_PREV)
      SW_NEXT  → GPIO9  (BTN_NEXT)
      SW_REW   → GPIO14 (BTN_REWIND)
      SW_FF    → GPIO15 (BTN_FAST_FORWARD)
```

> ESP32 内部上拉电阻约 10kΩ，按下读低电平。22Ω 串联电阻防止静电和过冲。  
> 实测去抖时间 30ms，由代码处理。

### 6.2 [可选] EC11 旋转编码器

```
                     EC11 (旋转编码器, 15 脉冲/转)
                     ┌─────────────────────────┐
                     │                         │
    GPIO38 ────┬────┤ Pin A (A 相)            │
               │    │                         │
              10nF  │ Pin B (B 相) ───────┬───┤ GPIO39
               │    │                     │   │
              GND   │                    10nF │
                    │                     │   │
                    │ Pin C (公共端) ──┬── GND │
                    │                 │       │
                    │ Pin D (按键) ───┤       │
                    │                 │       │
                    │ Pin E (按键公共) ─┤     │
                    │                     │   │
                    └─────────────────────┴───┘
                                        │
                                       GND
```

> EC11 公共端接 GND，A/B 相用 10nF 电容去抖。  
> 使用 ESP32 内部上拉（或外部 10kΩ → 3.3V）。  
> 编码器按键（SW_ENC）需另选 GPIO（如 GPIO21）或映射到已有功能。

### 6.3 [可选] 电池电压 ADC

```
    BAT+ (3.0~4.2V)──┬───┐
                      │   │
                     ┌┴┐  │
                     │ │  │ R1 = 100kΩ (1%, 0603)
                     └┬┘  │
                      │   │
                      ├───┼────── GPIO3 (ADC1_CH2)
                      │   │
                      │  100nF (去耦，靠近 GPIO3)
                      │   │
                     ┌┴┐  │
                     │ │  │ R2 = 100kΩ (1%, 0603)
                     └┬┘  │
                      │   │
                     GND  GND

    分压比: R2 / (R1 + R2) = 100 / (100 + 100) = 0.5
    量程: 4.2V × 0.5 = 2.1V (ESP32-S3 ADC 安全范围 0~2.4V)
    ADC 精度: ESP32-S3 ADC 有 12bit (0~4095)，内置参考 3.3V
```

**⚠️ GPIO3 是 Strapping 引脚**：启动期间电平决定了 JTAG 信号源选择。分压网络在 boot 时把 GPIO3 拉到约 2.1V（中间态），不影响正常启动。若遇到启动不稳，可改用 **GPIO4（非 strapping）+ 切换开关** 或加 P-MOSFET 隔离。

```
    改进方案：P-MOSFET 隔离
    BAT+ ──── R1 ─┬── P-MOSFET (Si2301) ──── GPIO3
                  │       G=GPIO_CTRL (高=断开)
                  R2      S=D, D=S
                  │
                 GND
```

---

## 附录 A: 关键器件选型说明

| 器件 | 选型理由 | 替代方案 |
|------|---------|---------|
| AMS1117-3.3 | 800mA 够用，低成本 | XC6206P332MR (300mA，更省电) |
| TP4056 | 标准 1A 充电，ESOP-8 散热好 | IP2312 (更小，带 NTC) |
| MAX98357A | I2S 输入，无需 MCU 配置，3W 输出 | MAX98357B (GAIN=GND 为 12dB，基准状态一致) |
| SSD1306 | 128×64 I2C，u8g2 广泛支持 | SSD1315 (兼容，略贵) |
| 轻触开关 6×6×5mm | 标准尺寸，手感适中 | 5×5×4.3mm (更小) |

## 附录 B: PCB 布局要点

| 要点 | 说明 |
|------|------|
| 1. 模组位置 | ESP32 模组靠近天线端，远离 GND 铜皮保持天线净空 |
| 2. 电源回路 | 大电流路径（电池→LDO→MAX98357）走线宽 ≥ 1mm |
| 3. 去耦电容 | 每个 IC 的电源引脚附近放 100nF，距离 < 5mm |
| 4. I2S 走线 | BCK/WS/DOUT 等长，远离时钟和电源，避免 90° 角 |
| 5. SD 卡 | 远离 D 类功放（MAX98357），避免数字噪声耦合 |
| 6. 喇叭走线 | 短而粗（≥ 0.8mm），远离敏感信号 |
| 7. GND 铜皮 | 完整 GND 平面，不同电源域单点接地 |
| 8. 天线净空 | PCB 天线区域不铺铜，不开窗，避开走线 |

## 附录 C: 模组尺寸与 PCB 建议

| 参数 | 值 |
|------|-----|
| PCB 层数 | 2 层（推荐 4 层，增强 GND + 电源） |
| PCB 尺寸 | 约 100mm × 60mm |
| 板厚 | 1.6mm |
| 铜厚 | 1oz (35µm) |
| 最小线宽/间距 | 0.3mm / 0.3mm (常规工艺) |
| 最小过孔 | 0.3mm 内径 / 0.6mm 外径 |

---

**修订记录**:

| 版本 | 日期 | 变更内容 |
|------|------|---------|
| 1.0 | 2026-07-09 | 初始版本，基于 HARDWARE_PIN_WIRING.md v1.1 + config.h |
