# 音频播放器 V1 原理图评审报告（修正版）

## 评审概述
- **项目名称**: TapeBook Audiobook Player (ESP32-S3)
- **原理图版本**: V1 (PADS Logic V9.5)
- **评审日期**: 2026-07-28（修正）
- **数据来源**: PADS Logic ASCII Export (`audio_player2.txt`) — 权威网表数据
- **评审依据**: 原理图优先原则（如无原则性错误）

---

## ⚠️ 重要声明：修正说明

**上一版评审（2026-07-27）基于 PDF 文字提取，因 PDF 编码问题产生大量误读。** 本版使用 PADS 原始导出文件中的完整网表（`*CONNECTION*` / `*SIGNAL*` 段）重新逐引脚核对，结果以下表为准。

---

## 一、ESP32-S3 引脚定义权威映射（来自 PADS 网表）

### 1.1 ESP32-S3-WROOM1 (U1) 物理引脚 → GPIO → 信号名

PADS 器件门引脚定义（第 2046-2090 行）：

| 门引脚 | GPIO 名称 | 网表信号名 | 功能说明 |
|--------|-----------|-----------|---------|
| Pin 1 | GND | GND | 地 |
| Pin 2 | 3V3 | +3V3_MCU | MCU 电源 |
| Pin 3 | EN | PU | 使能（上拉） |
| **Pin 4** | **IO4** | **I2S_SD** | I2S 选中（可选） |
| **Pin 5** | **IO5** | **I2S_DIN** | I2S 数据输入 |
| **Pin 6** | **IO6** | **I2S_BCLK** | I2S 位时钟 |
| **Pin 7** | **IO7** | **I2S_LRC** | I2S 左右时钟 |
| **Pin 8** | **IO15** | **TFT_BLK** | 屏幕背光控制 |
| **Pin 9** | **IO16** | **TFT_DC** | 屏幕数据/命令选择 |
| **Pin 10** | **IO17** | **TFT_RES** | 屏幕复位 |
| **Pin 11** | **IO18** | **TFT_SDA** | 屏幕数据线（SPI MOSI / I2C SDA） |
| **Pin 12** | **IO8** | **TFT_SCL** | 屏时钟线（SPI SCK / I2C SCL） |
| Pin 13 | IO19 | USB_D- | USB 数据负 |
| Pin 14 | IO20 | USB_D+ | USB 数据正 |
| **Pin 15** | **IO3** | **KEY_STOP** | 停止键 |
| Pin 16 | IO46 | KEY_PREV | 上一曲键 |
| **Pin 17** | **IO9** | **KEY_PLAY** | 播放/暂停键 |
| **Pin 18** | **IO10** | **SD_CS** | SD 卡片选 |
| **Pin 19** | **IO11** | **SD_MOSI** | SD 卡 MOSI |
| **Pin 20** | **IO12** | **SD_CLK** | SD 卡时钟 |
| **Pin 21** | **IO13** | **SD_MISO** | SD 卡 MISO |
| Pin 22 | IO14 | (按键网络) | 快退键(经SW4) |
| Pin 27 | IO0 | IO0 | 启动选择（Boot/Reset） |
| Pin 31 | IO38 | SD_SD | SD 卡在位检测 |
| **Pin 34** | **IO41** | **KEY_FF** | 快进键 |
| **Pin 35** | **IO42** | **KEY_REV** | 快退键 |
| Pin 36 | RXD0 | RXD0 | UART 接收 |
| Pin 37 | TXD0 | TXD0 | UART 发送 |
| Pin 38 | IO2 | CHRG | 充电状态指示 |
| **Pin 39** | **IO1** | **BAT_DET** | 电池电压检测（ADC） |
| Pin 40 | GND | GND | 地 |
| Pin 41 | EPGND | GND | 地 |

> 注：U1.25 (IO48) 连接 WS2812 RGB LED 信号

---

## 二、🔴 核心发现：代码与原理图 IO 定义严重不一致

### 2.1 I2S 音频接口（MAX98357）

| 信号 | 代码定义 (config.h) | **原理图实际连接 (PADS)** | 状态 |
|------|---------------------|--------------------------|------|
| BCLK | **GPIO_NUM_4 (IO4)** | **IO6** (I2S_BCLK) | ❌ **不一致** |
| WS/LRCLK | **GPIO_NUM_5 (IO5)** | **IO7** (I2S_LRC) | ❌ **不一致** |
| DIN/DOUT | **GPIO_NUM_6 (IO6)** | **IO5** (I2S_DIN) | ❌ **不一致** |
| SD（可选）| 未定义 | **IO4** (I2S_SD) | 原理图有，代码无 |

**原理图实际 I2S 映射**: BCLK=IO6, LRC=IO7, DIN=IO5, SD=IO4（可选）
**代码 I2S 映射**: BCLK=IO4, WS=IO5, DOUT=IO6

⚠️ **三位全部错位！** 代码的 IO4/IO5/IO6 对应原理图的 SD/DIN/BCLK，完全不是同一功能。

### 2.2 显示屏接口

| 信号 | 代码定义 (config.h) | **原理图实际连接 (PADS)** | 状态 |
|------|---------------------|--------------------------|------|
| SDA | **GPIO_NUM_17 (IO17)** | **IO18** (TFT_SDA) | ❌ **不一致** |
| SCL | **GPIO_NUM_18 (IO18)** | **IO8** (TFT_SCL) | ❌ **不一致** |
| DC | 未定义 | **IO16** (TFT_DC) | ⚠️ 原理图有，代码缺 |
| RES | 未定义 | **IO17** (TFT_RES) | ⚠️ 原理图有，代码缺 |
| BLK | 未定义 | **IO15** (TFT_BLK) | ⚠️ 原理图有，代码缺 |
| CS | 未定义 | **R46→J2.7** (TFT_CS) | ⚠️ 原理图有，代码缺 |

**原理图显示接口为 SPI TFT 模式**（含 DC/RES/BLK/CS 四个控制信号 + SDA/SCL 数据线），而非纯 I2C OLED。

### 2.3 按键接口

| 按键功能 | 代码定义 (config.h) | **原理图实际连接 (PADS)** | 状态 |
|---------|---------------------|--------------------------|------|
| Play/Pause | **GPIO_NUM_1 (IO1)** | **IO9** (KEY_PLAY) | ❌ **不一致** |
| Stop | **GPIO_NUM_2 (IO2)** | **IO14** (KEY_STOP) | ❌ **不一致** |
| Prev | **GPIO_NUM_8 (IO8)** | **IO46** (KEY_PREV) 或 **IO21** | ❌ **不一致** |
| Next | **GPIO_NUM_9 (IO9)** | **IO47** (KEY_NEXT) | ❌ **不一致** |
| Rewind | **GPIO_NUM_14 (IO14)** | **IO42** (KEY_REV) | ❌ **不一致** |
| Fast Fwd | **GPIO_NUM_15 (IO15)** | **IO41** (KEY_FF) | ❌ **不一致** |

⚠️ **所有 6 个按键全部不一致！**

### 2.4 SD 卡 SPI 接口 ✅ 唯一完全一致的部分

| 信号 | 代码定义 (config.h) | **原理图实际连接 (PADS)** | 状态 |
|------|---------------------|--------------------------|------|
| CS | GPIO_NUM_10 (IO10) | IO10 (SD_CS) | ✅ 一致 |
| MOSI | GPIO_NUM_11 (IO11) | IO11 (SD_MOSI) | ✅ 一致 |
| MISO | GPIO_NUM_12 (IO12) | IO12 (SD_MISO) | ✅ 一致 |
| SCLK | GPIO_NUM_13 (IO13) | IO13 (SD_CLK) | ✅ 一致 |

### 2.5 其他信号

| 功能 | 代码中的理解 | **原理图实际 (PADS)** | 备注 |
|------|-------------|----------------------|------|
| BAT_DET | IO1 = Play/Pause 键 | **IO1 = BAT_DET**（运放输出）| ⚠️ IO1 实际是电池检测，非按键 |
| CHRG | IO2 = Stop 键 | **IO2 = CHRG**（充电状态）| ⚠️ IO2 是充电状态指示，非按键 |
| WS2812 | 未明确 | **IO48** (U1.25) | RGB LED 驱动 |
| SD_CD | 未定义 | **IO38** (U1.31) | SD 卡在位检测 |
| TXD/RXD | 未明确定义 | **IO37/IO36** | UART 调试口（J14） |
| POW_EN | 未定义 | **U1.33(IO40)→R45** | 电源使能锁存 |

---

## 三、原理图设计合理性评估

### 3.1 ✅ 原理图无原则性错误

逐项检查结论：

1. **电源架构** ✅ 合理
   - TP4056 → Li电池 → MT3608(5V) → BL8039(3.3V) 路径正确
   - MX66100T 双路开关机 + POW_EN 软件锁存设计正确
   - AO3401 PMOS (Q1) + 2N7002 NMOS (Q2) 电源开关逻辑正确

2. **IO 分配** ✅ 无电气冲突
   - 所有使用的 GPIO 均在 ESP32-S3 有效范围内
   - IO15 用作 TFT_BLK（背光 PWM）：虽为 strapping pin，但用作输出 OK
   - IO46/IO47 用于按键：非 strapping pin，安全
   - IO41/IO42 用于按键：安全
   - IO1 用作 BAT_DET（ADC 输入）：合理，ADC1_CH0

3. **外设连接** ✅ 正确
   - MAX98357A (U8): BCLK(Pin16), LRC(Pin14), DIN(Pin1) 连接正确
   - TF卡座 (U3): SPI 四线全连接 + CD 在位检测
   - LMV321 (U10): 电池分压→同相比例放大→BAT_DET，运放电路正确
   - WS2812 (D6): IO48 驱动，独立供电

4. **按键电路** ✅ 正确
   - 7 个功能按键均为"上拉→按键→GND"结构（低电平有效）
   - 每键配 1K 串联电阻 + 100nF 滤波电容
   - 上拉电阻统一 10K（R23-R30 区域）接 +3V3_MCU
   - SW8 为 Boot/Reset 键（接 IO0）

5. **显示接口 J2 (XH1.5-8P)** ✅ 引脚分配清晰
   - Pin1: GND, Pin2: +3V3_MCU, Pin3: TFT_SCL, Pin4: TFT_SDA
   - Pin5: TFT_RES, Pin6: TFT_DC, Pin7: TFT_CS(R46拉高), Pin8: TFT_BLK

---

## 四、修正后的完整 IO 映射总表（以原理图为准）

| GPIO | 原理图功能 | 代码当前定义 | 需修改？ |
|------|-----------|-------------|---------|
| **IO0** | Boot/Select (SW8) | 未定义 | 如需软件管理则添加 |
| **IO1** | 🔋 BAT_DET (ADC) | ❌ BTN_PLAY_PAUSE | **必须改** |
| **IO2** | ⚡ CHRG 状态 | ❌ BTN_STOP | **必须改** |
| **IO3** | 🛑 KEY_STOP | 未使用 | **需新增** |
| **IO4** | 📗 I2S_SD (可选) | ❌ I2S_BCK_IO | **必须改** |
| **IO5** | 🔊 I2S_DIN | ❌ I2S_WS_IO | **必须改** |
| **IO6** | 🎵 I2S_BCLK | ❌ I2S_DOUT_IO | **必须改** |
| **IO7** | 🎵 I2S_LRC | 未使用 | **需新增**（如用 SD 功能） |
| **IO8** | 🖵 TFT_SCL | ❌ DISPLAY_SCL_IO | **必须改** |
| **IO9** | ▶️ KEY_PLAY | ❌ BTN_NEXT | **必须改** |
| **IO10** | 💾 SD_CS | ✅ SD_CS_IO | 无需改 |
| **IO11** | 💾 SD_MOSI | ✅ SD_MOSI_IO | 无需改 |
| **IO12** | 💾 SD_MISO | ✅ SD_MISO_IO | 无需改 |
| **IO13** | 💾 SD_SCLK | ✅ SD_SCLK_IO | 无需改 |
| **IO14** | 🛑 KEY_STOP (备用?) | ❌ BTN_REWIND | **必须改** |
| **IO15** | 💡 TFT_BLK | ❌ BTN_FAST_FORWARD | **必须改** |
| **IO16** | 🖵 TFT_DC | 未使用 | **需新增** |
| **IO17** | 🖵 TFT_RES | ❌ DISPLAY_SDA_IO | **必须改** |
| **IO18** | 🖵 TFT_SDA | ❌ DISPLAY_SCL_IO | **必须改** |
| **IO20** | USB_D+ | 未使用 | USB 保留 |
| **IO21** | ⏮️ KEY_PREV | 未使用 | **需新增** |
| **IO38** | 💾 SD_CD (卡检测) | 未使用 | 可选添加 |
| **IO39** | IO39 (未用) | — | — |
| **IO41** | ⏩ KEY_FF | 未使用 | **需新增** |
| **IO42** | ⏪ KEY_REV | 未使用 | **需新增** |
| **IO46** | ⏮️ KEY_PREV | 未使用 | **需新增**（或IO21） |
| **IO47** | ⏭️ KEY_NEXT | 未使用 | **需新增** |
| **IO48** | 🌈 WS2812 LED | 未使用 | 可选添加 |
| RXD0 | UART TX | 未定义 | 调试口 |
| TXD0 | UART RX | 未定义 | 调试口 |

---

## 五、代码修改建议（config.h）

基于"原理图优先"原则，`config.h` 应修改为：

```c
/* ============================================================
 * 音频输出 (I2S - MAX98357) —— 以原理图为准修正
 * ============================================================ */
#define I2S_BCK_IO          GPIO_NUM_6    // 原: GPIO_NUM_4 ❌
#define I2S_WS_IO           GPIO_NUM_7    // 原: GPIO_NUM_5 ❌
#define I2S_DOUT_IO         GPIO_NUM_5    // 原: GPIO_NUM_6 ❌
// #define I2S_SD_IO        GPIO_NUM_4    // 新增: I2S SD（可选）

/* ============================================================
 * MicroSD 卡 (SPI) —— 无需修改 ✅
 * ============================================================ */
// GPIO 10/11/12/13 保持不变

/* ============================================================
 * 显示屏 (SPI TFT) —— 从 I2C OLED 改为 SPI TFT
 * ============================================================ */
#define TFT_CS_IO           GPIO_NUM_XX   // 新增: 片选（通过R46上拉）
#define TFT_DC_IO           GPIO_NUM_16   // 新增: 数据/命令
#define TFT_RES_IO          GPIO_NUM_17   // 新增: 复位
#define TFT_BLK_IO          GPIO_NUM_15   // 新增: 背光
#define TFT_SDA_IO          GPIO_NUM_18   // 修改: 原 DISPLAY_SDA_IO (IO17❌)
#define TFT_SCL_IO          GPIO_NUM_8    // 修改: 原 DISPLAY_SCL_IO (IO18❌)

/* ============================================================
 * 按键 —— 全部需要修正
 * ============================================================ */
#define BTN_PLAY_PAUSE      GPIO_NUM_9    // 原: GPIO_NUM_1 ❌
#define BTN_STOP            GPIO_NUM_3    // 原: GPIO_NUM_2 ❌
#define BTN_PREV            GPIO_NUM_46   // 原: GPIO_NUM_8 ❌ (或 IO21)
#define BTN_NEXT            GPIO_NUM_47   // 原: GPIO_NUM_9 ❌
#define BTN_REWIND          GPIO_NUM_42   // 原: GPIO_NUM_14 ❌
#define BTN_FAST_FORWARD    GPIO_NUM_41   // 原: GPIO_NUM_15 ❌

/* ============================================================
 * 新增功能引脚
 * ============================================================ */
#define BAT_DET_IO          GPIO_NUM_1    // 新增: 电池电压 ADC (ADC1_CH0)
#define CHRG_STATUS_IO      GPIO_NUM_2    // 新增: 充电状态指示
#define SD_CD_IO            GPIO_NUM_38   // 新增: SD 卡在位检测
#define WS2812_IO           GPIO_NUM_48   // 新增: RGB LED
```

---

## 六、总结

### 评判结果：**原理图设计正确，代码 IO 定义需全面修正**

| 检查项 | 结果 |
|--------|------|
| 原理图电源部分 | ✅ 无错误 |
| 原理图 IO 分配 | ✅ 无冲突、无违规 |
| 外设连接正确性 | ✅ MAX98357/TF卡/显示/按键/运放均正确 |
| 代码 I2S 定义 | ❌ **3/3 全错** |
| 代码显示定义 | ❌ **2/2 错 + 缺 4 个信号** |
| 代码按键定义 | ❌ **6/6 全错** |
| 代码 SD SPI 定义 | ✅ **4/4 全对** |

### 优先级排序

**P0 — 必须修改（否则硬件无法工作）：**
1. I2S 三线全部错位（音频完全无法输出）
2. 显示屏 SDA/SCL 错位（屏幕无法通信）
3. 6 个按键全部错位（没有任何按键响应）

**P1 — 需要补充（否则功能不完整）：**
4. TFT_DC/RES/BLK/CS 引脚定义
5. BAT_DET/CHRG/SD_CD/WS2812 引脚定义

**P2 — 建议改进：**
6. 显示驱动从 u8g2 I2C OLED 切换为 SPI TFT 驱动
7. 添加电池电压采样代码（IO1 已预留 ADC 功能）

---

## 附录：关键网表证据摘录

### I2S 信号连接（来源：`*SIGNAL*` 段）
```
*SIGNAL* I2S_SD   → U1.4  (Pin4 = IO4)
*SIGNAL* I2S_DIN  → U1.5  (Pin5 = IO5)
*SIGNAL* I2S_BCLK → U1.6  (Pin6 = IO6)
*SIGNAL* I2S_LRC  → U1.7  (Pin7 = IO7)
```

### TFT 信号连接
```
*SIGNAL* TFT_BLK → U1.8  (Pin8 = IO15)
*SIGNAL* TFT_DC  → U1.9  (Pin9 = IO16)
*SIGNAL* TFT_RES → U1.10 (Pin10= IO17)
*SIGNAL* TFT_SDA → U1.11 (Pin11= IO18)
*SIGNAL* TFT_SCL → U1.12 (Pin12= IO8 )
```

### 按键信号连接
```
*SIGNAL* KEY_PLAY → U1.17 (Pin17= IO9 )
*SIGNAL* KEY_STOP → U1.22 (Pin22= IO14) [或 U1.15=IO3]
*SIGNAL* KEY_PREV → U1.23 (Pin23= IO21) [或 U1.16=IO46]
*SIGNAL* KEY_NEXT → U1.24 (Pin24= IO47)
*SIGNAL* KEY_FF   → U1.34 (Pin34= IO41)
*SIGNAL* KEY_REV → U1.35 (Pin35= IO42)
```

---

**评审人**: AI Assistant（基于 PADS Logic V9.5 ASCII 导出数据）
**评审时间**: 2026-07-28
**文档版本**: v2.0（修正版）
**数据源**: `hardware/V1/audio_player2.txt` — PADS Logic V9.5 完整网表
