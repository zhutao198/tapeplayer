# TapeBook 硬件引脚接线图

> **数据来源**：`config.h`（权威）、`README.md`、`DESIGN.md` §3.2、`PRD.md` §7.2、`HARDWARE_MODULE_MIGRATION.md`  
> **数据版本**：v1.1（R005 修订）  
> **主控**：ESP32-S3-WROOM-1 N16R8（量产）/ WROOM-2 N32R16V（开发）  
> **显示屏**：SSD1315（与 SSD1306 命令兼容，I2C 地址 0x3C）

---

## 1. 总览接线图

```
ESP32-S3                    MAX98357 (I2S DAC)
┌──────────┐                ┌──────────────┐
│ GPIO4    ├──► BCK ───────►│ BCLK          │
│ GPIO5    ├──► WS  ───────►│ LRC           │
│ GPIO6    ├──► DOUT ──────►│ DIN           │
│ GPIO7    ├──► MCLK ──────►│ MCLK (可选)    │
│ 3.3V     ├───────────────►│ VIN           │
│ GND      ├───────────────►│ GND           │
│          │                │ GAIN ──► GND  │
│          │                │ SD_MODE─►634kΩ─►3.3V │
└──────────┘                └──────┬───────┘
                                  │
                              ┌───▼───┐
                              │ Speaker│ 3W 4Ω/8Ω
                              └───────┘

ESP32-S3                    MicroSD (SPI)
┌──────────┐                ┌──────────────┐
│ GPIO10   ├──► CS ────────►│ CS            │
│ GPIO11   ├──► MOSI ──────►│ MOSI (DI)     │
│ GPIO12   ├──◄ MISO ──────┤ MISO (DO)     │
│ GPIO13   ├──► SCK ───────►│ SCK (CLK)     │
│ 3.3V     ├───────────────►│ VCC           │
│ GND      ├───────────────►│ GND           │
└──────────┘                └──────────────┘

ESP32-S3                    SSD1306 OLED (I2C)
┌──────────┐                ┌──────────────┐
│ GPIO17   ├──► SDA ───────►│ SDA           │
│ GPIO18   ├──► SCL ───────►│ SCL           │
│ 3.3V     ├───────────────►│ VCC           │
│ GND      ├───────────────►│ GND           │
└──────────┘                └──────────────┘

ESP32-S3                    按键 (6个，对GND)
┌──────────┐
│ GPIO1    ├──┬──► 播放/暂停
│ GPIO2    ├──┼──► 停止
│ GPIO8    ├──┼──► 上一首
│ GPIO9    ├──┼──► 下一首
│ GPIO14   ├──┼──► 快退 (Rewind)
│ GPIO15   ├──┼──► 快进 (Fast Forward)
│ GND      ├──┴── 所有按键另一端
└──────────┘
  (内部上拉，按下=低电平)

ESP32-S3                    EC11 旋转编码器 (可选)
┌──────────┐                ┌──────────────┐
│ GPIO38   ├──◄ A ─────────►│ A 相          │
│ GPIO39   ├──◄ B ─────────►│ B 相          │
│ GND      ├──◄ C ─────────►│ 公共端        │
└──────────┘                └──────────────┘
  (内部上拉，中断触发)

ESP32-S3                    电池电压检测 (可选)
┌──────────┐
│ GPIO3    ├──◄── 100kΩ ────┬── BAT+ (3.0-4.2V)
│          │                │
│          │               100kΩ
│          │                │
│ GND      ├────────────────┴── GND
│          │    (1:1 分压，ADC1_CH2)
└──────────┘
     100nF ── GND (去耦，建议加)
```

---

## 2. 完整引脚分配表

| GPIO | 功能 | 外设 | 方向 | 上下拉 | 备注 |
|------|------|------|------|--------|------|
| **GPIO1** | BTN_PLAY | 播放/暂停 | IN | 上拉 | 按下低电平 |
| **GPIO2** | BTN_STOP | 停止 | IN | 上拉 | 按下低电平 |
| **GPIO3** | BAT_ADC | 电池检测 | IN | — | ADC1_CH2，1:1 分压；⚠️ Strapping 引脚（JTAG 信号源选择），分压网络可能影响 boot |
| **GPIO4** | I2S_BCK | MAX98357 | OUT | — | 位时钟 |
| **GPIO5** | I2S_WS | MAX98357 | OUT | — | 字选择（LRC） |
| **GPIO6** | I2S_DOUT | MAX98357 | OUT | — | 数据输出 |
| **GPIO7** | I2S_MCLK | MAX98357 | OUT | — | 主时钟（APLL，可选） |
| **GPIO8** | BTN_PREV | 上一首 | IN | 上拉 | 短按=上一曲，长按=音量- |
| **GPIO9** | BTN_NEXT | 下一首 | IN | 上拉 | 短按=下一曲，长按=音量+ |
| **GPIO10** | SD_CS | MicroSD | OUT | — | SPI 片选 |
| **GPIO11** | SD_MOSI | MicroSD | OUT | — | SPI 数据输出 |
| **GPIO12** | SD_MISO | MicroSD | IN | — | SPI 数据输入 |
| **GPIO13** | SD_SCLK | MicroSD | OUT | — | SPI 时钟（最高 40MHz） |
| **GPIO14** | BTN_REW | 快退 | IN | 上拉 | 短按=跳-10s，长按=加速倒带 |
| **GPIO15** | BTN_FF | 快进 | IN | 上拉 | 短按=跳+10s，长按=加速快进 |
| **GPIO17** | OLED_SDA | SSD1306 | I/O | 上拉 | I2C 数据线 |
| **GPIO18** | OLED_SCL | SSD1306 | OUT | 上拉 | I2C 时钟线（400kHz） |
| **GPIO38** | ENC_A | EC11 编码器 | IN | 上拉 | A 相（中断触发） |
| **GPIO39** | ENC_B | EC11 编码器 | IN | 上拉 | B 相（中断触发） |
| **GPIO0** | BOOT | 启动模式 | — | — | 保留（自动下载） |

### 未使用的 GPIO（可扩展）

| GPIO | WROOM-1 N16R8 | WROOM-2 N32R16V |
|------|--------------|----------------|
| GPIO33/34 | **可用** ✅ | 被 Octal SPI 占用 ❌ |
| GPIO35/36/37 | 被 Quad SPI 占用 ❌ | 被 Octal SPI 占用 ❌ |
| GPIO47/48 | N/A（N16R8 用 R8V 芯片无此引脚）| 1.8V 域（需电平转换）⚠️ |

---

## 3. 各外设详细接线

### 3.1 MAX98357A I2S 功放

| MAX98357A 引脚 | 接 ESP32 | 接其他 |
|-------------|----------|--------|
| **BCLK** | GPIO4 | — |
| **LRC** | GPIO5 | — |
| **DIN** | GPIO6 | — |
| **MCLK** | GPIO7（可选） | 可不接 |
| **VIN** | 3.3V 或 5V（USB） | 3.3V≈1W，5V≈3.2W（4Ω） |
| **GND** | GND | — |
| **GAIN** | — | **GND**（12dB 增益） |
| **SD_MODE** | — | **634kΩ 接 VDD**（Mono (L+R)/2 混合单声道） |

**SD_MODE 真值表（MAX98357A 规格书 Table 5）**：

| SD_MODE 状态 | 电压阈值 | 输出模式 |
|---|---|---|
| GND | < 0.16V | Shutdown（静音） |
| 通过 RLARGE 接 VDD | 0.16~0.77V | **Mono (L+R)/2** ← 单喇叭推荐 |
| 通过 RSMALL 接 VDD | 0.77~1.4V | Right channel |
| VDD | > 1.4V | Left channel |

**电阻选值公式**（规格书 Page 17）：

| VDDIO | RLARGE（Mono） | RSMALL（Right） |
|---|---|---|
| 1.8V | 300 kΩ | 69.8 kΩ |
| **3.3V**（本项目） | **634 kΩ** | 210.2 kΩ |
| 5.0V | 1011 kΩ | 370 kΩ |

**注**：SD_MODE 内部有 100kΩ 下拉到 GND（RPD=100kΩ typ），悬空 = Shutdown；外部上拉电阻按公式 `RLARGE(kΩ) = 222.2 × VDDIO - 100`（Mono）或 `RSMALL(kΩ) = 94.0 × VDDIO - 100`（Right）计算。不同型号（A/B/C/D）SD_MODE 行为可能不同，请以实际 datasheet 为准。

### 3.2 MicroSD 卡（SPI 模式）

| MicroSD 引脚 | 接 ESP32 | 备注 |
|-------------|----------|------|
| **CS** | GPIO10 | 片选，低电平有效 |
| **MOSI (DI)** | GPIO11 | 数据输入 |
| **MISO (DO)** | GPIO12 | 数据输出 |
| **SCK (CLK)** | GPIO13 | 时钟（SPI2_HOST，最高 40MHz） |
| **VCC** | 3.3V | — |
| **GND** | GND | — |

### 3.3 SSD1315 OLED（I2C，与 SSD1306 命令兼容）

| OLED 引脚 | 接 ESP32 | 备注 |
|----------|----------|------|
| **SDA** | GPIO17 | I2C 数据（I2C_NUM_0） |
| **SCL** | GPIO18 | I2C 时钟（400kHz） |
| **VCC** | 3.3V | — |
| **GND** | GND | — |
| **ADDR** | — | 默认 0x3C（SA0=GND 时） |

**注**：实际芯片为 SSD1315，与 SSD1306 命令集兼容（u8g2 的 `u8g2_Setup_ssd1306_i2c_128x64_noname_f` 可直接驱动）。I2C 从机地址 = `0x3C`（SA0=0 时）或 `0x3D`（SA0=1 时）。

### 3.4 按键矩阵

| 按键 | 接 ESP32 | 另一端 | 代码宏 | 功能 |
|------|----------|--------|--------|------|
| 播放/暂停 | GPIO1 | GND | `BTN_PLAY_PAUSE` | 播放/暂停切换，超长按 3s 锁定 |
| 停止 | GPIO2 | GND | `BTN_STOP` | 停止，双击=语音播报，长按=文件夹浏览 |
| 上一首 | GPIO8 | GND | `BTN_PREV` | 上一曲，长按=音量- |
| 下一首 | GPIO9 | GND | `BTN_NEXT` | 下一曲，长按=音量+ |
| 快退 | GPIO14 | GND | `BTN_REWIND` | 跳-10s，长按=加速倒带 |
| 快进 | GPIO15 | GND | `BTN_FAST_FORWARD` | 跳+10s，长按=加速快进 |

> 所有按键使用 ESP32 内部上拉电阻，无需外部上拉；按下读低电平。

### 3.5 EC11 旋转编码器（可选，V1.1+）

EC11 编码器共 5 个引脚（旋转 A/B/C + 按键 D/E）：

| EC11 引脚 | 接 ESP32 | 备注 |
|----------|----------|------|
| **A 相** | GPIO38 | 中断触发 |
| **B 相** | GPIO39 | 中断触发 |
| **公共端 (C)** | GND | — |
| **按键 (D)** | GPIO（可选） | 按下静音/确认，需另选 GPIO 接入 |
| **按键公共端 (E)** | GND | 与 D 配对 |
| **A/B 去耦** | — | 建议 A/B 对 GND 各接 10nF 电容消抖 |

### 3.6 电池电压检测（可选，V1.2+）

```
BAT+ (3.0~4.2V)
    │
   ┌┴┐
   │ │ 100kΩ (R1)
   └┬┘
    ├──── GPIO3 (ADC1_CH2)
    │
   ┌┴┐
   │ │ 100kΩ (R2)
   └┬┘
    │
   GND

  GPIO3 ── 100nF ── GND  (建议加去耦)
```

- 分压比：1:1（4.2V → 2.1V，在 ADC 安全范围 0~2.4V 内）
- 电池 4.2V → ADC 读数 ~2.1V → 100%
- 电池 3.0V → ADC 读数 ~1.5V → 0%

---

## 4. 供电方案

```
USB 5V (Type-C)
   │
   ▼
TP4056 充电管理 ────► 18650 锂电池 (3.7V 2600mAh)
   │
   ▼
拨动开关 (总电源)
   │
   ▼
AMS1117-3.3 LDO (800mA)
   │
   ├──► ESP32-S3 (3.3V)
   ├──► MAX98357 VIN (或直连 5V)
   ├──► SSD1306 VCC (3.3V)
   ├──► MicroSD VCC (3.3V)
   └──► EC11 上拉 (3.3V)
```

### 电流预算

| 模块 | 工作电流 | 峰值 |
|------|---------|------|
| ESP32-S3（WiFi/BT 关） | 50 mA | 240 mA |
| MAX98357（中等音量） | 80 mA | 250 mA |
| SSD1306 OLED | 20 mA | 30 mA |
| MicroSD 读写 | 30 mA | 100 mA |
| **合计** | **~180 mA** | **~620 mA** |

> 2600mAh 电池理论续航 ≈ 14 小时（实际约 10-12 小时）

---

## 5. 模组差异说明

| 项 | WROOM-2 N32R16V（开发） | WROOM-1 N16R8（量产） |
|---|------------------------|---------------------|
| Flash | 32MB Octal SPI | 16MB Quad SPI |
| PSRAM | 16MB Octal | 8MB Octal |
| VDD_SPI | **1.8V** | **3.3V** |
| 被 SPI 占用 | GPIO33/34/35/36/37（5个） | GPIO35/36/37（3个） |
| 释放的 GPIO | — | **GPIO33/GPIO34** ✅ |
| SDK 配置 | `configure.bat wroom-2-n32r16v` | `configure.bat wroom-1-n16r8` |
| 分区表 | `partitions.csv`（单 factory） | `partitions_ota.csv`（支持 OTA） |

> **切换命令**：`configure.bat wroom-1-n16r8` 或 `wroom-2-n32r16v`

---

## 6. 关键设计约束

1. **I2S 引脚**：GPIO4/5/6/7 在 WROOM-1/WROOM-2 上均可用 ✅
2. **I2C 引脚**：GPIO17/18 使用 I2C_NUM_0，是安全可用的普通 GPIO（非 USB-JTAG）。ESP32-S3 的 USB Serial/JTAG 引脚为 **GPIO19(USB_D-)/GPIO20(USB_D+)**，本项目未使用 ✅
3. **UART0**：ESP32-S3 的 U0TXD=**GPIO43**、U0RXD=**GPIO44**，与 GPIO1/2 无关。本项目按键 GPIO1/2 不冲突 ✅
4. **GPIO3 Strapping ⚠️**：GPIO3 是 strapping 引脚（JTAG 信号源选择），用作 BAT_ADC 时 1:1 分压网络会在 boot 期间将其拉至 ~2.1V（中间状态），**可能影响启动行为**。建议：改用非 strapping 引脚（如 GPIO4/5），或加 P-MOSFET 在 boot 后导通分压电路
5. **Strapping 引脚**：GPIO0/45/46 启动期间不能拉错电平，本项目未作业务使用 ✅
   - GPIO45 控制 **VDD_SPI 电压**（0=1.8V，1=3.3V）。量产 WROOM-1 N16R8 的 VDD_SPI=3.3V，与该项目无关；若改用其他模组需注意
   - GPIO46 控制 **ROM log 打印**，不影响功能
6. **高阻引脚**：所有未用 GPIO 悬空或配置为下拉，降低功耗
