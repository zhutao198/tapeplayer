/**
 * @file config.h
 * @brief 引脚定义和全局配置常量
 *
 * V1.3: 基于 PADS Logic V9.5 原理图网表 (hardware/V1/audio_player.txt) 全面修正 IO 映射。
 *       所有 GPIO 定义以原理图为准。每个宏注释标注了对应的 U1 物理引脚号（网表证据）。
 *       映射来源：U1 part def (line 3147-3187) + 各 *SIGNAL* 段连接。
 */

#pragma once

#include "driver/gpio.h"

/* ============================================================
 * 音频输出 (I2S - MAX98357A, U8)
 * 原理图信号 → U1 引脚 → GPIO:
 *   I2S_SD   → U1.4  → IO4  (增益/声道选择: 悬空=9dB, 高=12dB, 低=15dB/差分)
 *   I2S_DIN  → U1.5  → IO5  (数据输入到 DAC, MAX98357 U8.1)
 *   I2S_BCLK → U1.6  → IO6  (位时钟, MAX98357 U8.16)
 *   I2S_LRC  → U1.7  → IO7  (左右时钟, MAX98357 U8.14)
 * 注: MAX98357A 无需 MCLK, I2S_MCLK 已删除。
 * ============================================================ */
#define I2S_BCK_IO          GPIO_NUM_6    // 原理图 I2S_BCLK (U1.6)
#define I2S_WS_IO           GPIO_NUM_7    // 原理图 I2S_LRC  (U1.7)
#define I2S_DOUT_IO         GPIO_NUM_5    // 原理图 I2S_DIN  (U1.5) — 对 DAC 是数据输入
#define I2S_SD_IO           GPIO_NUM_4    // 原理图 I2S_SD   (U1.4) — MAX98357 Pin4 = SD_MODE (采样率模式选择)
// MAX98357 SD_MODE 控制脚（I2S_SD 信号，接 MAX98357 Pin4，非 I2S 数据流）。
// 须由 MCU 拉到固定电平，否则悬空会导致采样率模式不确定、无声。
#define MAX98357_SD_MODE_GPIO        GPIO_NUM_4
#define MAX98357_SD_MODE_LEVEL       1   /* 拉高：采样率模式（原设计 SD_MODE 接 VDD 的意图）*/

/* ============================================================
 * MicroSD 卡 (SPI, U3)
 * 原理图信号 → U1 引脚 → GPIO:
 *   SD_CS   → U1.18 → IO10
 *   SD_MOSI → U1.19 → IO11
 *   SD_CLK  → U1.20 → IO12
 *   SD_MISO → U1.21 → IO13
 * 总线: SPI2_HOST
 * ⚠️ 代码原误将 SCLK/MISO 写反 (IO13/IO12)，现按原理图修正。
 * ============================================================ */
#define SD_SPI_HOST         SPI2_HOST
#define SD_CS_IO            GPIO_NUM_10   // 原理图 SD_CS   (U1.18) ✅
#define SD_MOSI_IO          GPIO_NUM_11   // 原理图 SD_MOSI (U1.19) ✅
#define SD_SCLK_IO          GPIO_NUM_12   // 原理图 SD_CLK  (U1.20) ⚠️修正: 原代码误用 IO13
#define SD_MISO_IO          GPIO_NUM_13   // 原理图 SD_MISO (U1.21) ⚠️修正: 原代码误用 IO12
#define SD_MOUNT_POINT      "/sdcard"

/* ============================================================
 * TFT 显示屏 (SPI - ST7789, 2.0", 320x240, J2)
 * 原理图信号 → U1 引脚 → GPIO:
 *   TFT_SDA  → U1.11 → IO18  (SPI MOSI)
 *   TFT_SCL  → U1.12 → IO8   (SPI SCK)
 *   TFT_DC   → U1.9  → IO16  (Data/Command 选择)
 *   TFT_RES  → U1.10 → IO17  (硬复位, 低有效)
 *   TFT_BLK  → U1.8  → IO15  (背光 PWM, LEDC 通道)
 *   TFT_CS   → 固定接地 (常选模式, 无需 GPIO 控制)
 * 总线: SPI3_HOST (独立于 SD 卡的 SPI2_HOST, 避免总线冲突)
 * ============================================================ */
#define DISPLAY_SPI_HOST    SPI3_HOST
#define DISPLAY_CS_IO       (-1)          // CS 固定接地, 常选模式
#define DISPLAY_DC_IO       GPIO_NUM_16   // TFT_DC  (U1.9)
#define DISPLAY_RESET_IO    GPIO_NUM_17   // TFT_RES (U1.10)
#define DISPLAY_BLK_IO      GPIO_NUM_15   // TFT_BLK (U1.8) — LEDC PWM 背光
#define DISPLAY_MOSI_IO     GPIO_NUM_18   // TFT_SDA (U1.11)
#define DISPLAY_SCLK_IO     GPIO_NUM_8    // TFT_SCL (U1.12)

/* 屏幕方向（后期可切换，无需改驱动）：
 *   0 = 横屏 (320x240)  —— 当前默认，匹配原 u8g2 布局
 *   1 = 竖屏 (240x320)
 * ST7789 物理面板为 240x320，方向由 esp_lcd 的 swap_xy/mirror 在初始化时设置。 */
#ifndef DISPLAY_ORIENTATION
#define DISPLAY_ORIENTATION 0
#endif
#if DISPLAY_ORIENTATION == 1
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#else
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#endif

/* ============================================================
 * 按键 (内部上拉, 按下为低电平)
 * 原理图信号 → U1 引脚 → GPIO:
 *   KEY_PLAY  → U1.17 → IO9   (RTC GPIO ✅ 可唤醒)
 *   KEY_STOP  → U1.22 → IO14  (RTC GPIO ✅ 可唤醒)
 *   KEY_PREV  → U1.23 → IO21  (RTC GPIO ✅ 可唤醒)
 *   KEY_NEXT  → U1.24 → IO47  (⚠️ 非 RTC GPIO, light-sleep 唤醒受限)
 *   KEY_REV   → U1.35 → IO42  (快退, 按住; ⚠️ 非 RTC GPIO)
 *   KEY_FF    → U1.34 → IO41  (快进, 按住; ⚠️ 非 RTC GPIO)
 * 注意:
 *   - IO1 现为 BAT_DET (电池 ADC), 不再作按键
 *   - IO2 现为 CHRG (充电状态), 不再作按键
 *   - IO15 现为 TFT_BLK (背光), 不再作 FF 键
 *   - KEY_NEXT/REV/FF 非 RTC GPIO, 若用 EXT1 唤醒需特殊处理 (见 main.cpp)
 * ============================================================ */
#define BTN_PLAY_PAUSE      GPIO_NUM_9    // KEY_PLAY  (U1.17)
#define BTN_STOP            GPIO_NUM_14   // KEY_STOP  (U1.22)
#define BTN_PREV            GPIO_NUM_21   // KEY_PREV  (U1.23) ⚠️修正: 原代码误用 IO8
#define BTN_NEXT            GPIO_NUM_47   // KEY_NEXT  (U1.24)
#define BTN_REWIND          GPIO_NUM_42   // KEY_REV   (U1.35) 快退(按住)
#define BTN_FAST_FORWARD    GPIO_NUM_41   // KEY_FF    (U1.34) 快进(按住)

/* 音量旋钮 — 连晟欣 LCK-TG001A-G1 拨轮开关:
 *   公共端 a → GND
 *   左拨 (A 向 24°) a-b 通 → BTN_VOL_DOWN (GPIO0) 经 10kΩ 上拉到 3.3V
 *   右拨 (B 向 24°) a-d 通 → BTN_VOL_UP   (GPIO3) 经 10kΩ 上拉到 3.3V
 *   下按 (C 向 1mm) a-c 通 → 纯机械电源开关 (不进 GPIO)
 * 注意: GPIO0/GPIO3 为 Strapping 引脚, 外部 10kΩ 上拉保证上电瞬间为高,
 *       不破坏启动模式 (GPIO0=高=SPI Boot, GPIO3=高=默认 JTAG 信号源)。
 *       拨轮自复位, 按下和拨动机械互锁, 开机时无长按风险。 */
#define BTN_VOL_DOWN        GPIO_NUM_0    // VOL-  左拨 (LCK a-b)
#define BTN_VOL_UP          GPIO_NUM_3    // VOL+  右拨 (LCK a-d)

#define BTN_DEBOUNCE_MS     30            // 按键去抖时间 (ms)
#define BTN_LONG_PRESS_MS   800           // 长按判定时间 (ms) — 所有按键短按/长按分界（R045）
#define BTN_SCAN_INTERVAL   20            // 按键扫描间隔 (ms)

/* 浏览模式长按连续移动曲目：间隔随按住时长加速缩短 (ms) */
#define BROWSE_REPEAT_MS_INIT  120         // 刚进入长按的起始间隔 (ms/曲)
#define BROWSE_REPEAT_MS_FAST  50          // 按住约 1.5s 后加速到 (ms/曲)
#define BROWSE_REPEAT_MS_MIN   30          // 最快间隔 (ms/曲)
#define BROWSE_HOLD_ACCEL_MS   1500        // 超过此时长后达到最快速度
#define BROWSE_PAGE_STEP       6           // 浏览模式快退/快进翻页步长 (曲, ≈一屏)

/* ============================================================
 * 电源管理与状态检测 (V1.1+)
 * 原理图信号 → U1 引脚 → GPIO:
 *   BAT_DET    → U1.39 → IO1   (电池电压 ADC1_CH0, LMV321 运放输出)
 *   CHRG       → U1.38 → IO2   (充电状态, 低=充电中)
 *   WS2812     → U1.25 → IO48  (⚠️ 1.8V 域, 需电平转换)
 *   SD_SD      → U1.31 → IO38  (SD 卡在位/写保护检测)
 *   POW_EN     → U1.33 → IO40  (电源锁存控制, 长按关机脉冲)
 *   LCD_POW_EN → U1.32 → IO39  (LCD 软电源开关, PMOS 低电平导通)
 * ============================================================ */
#define BAT_DET_IO          GPIO_NUM_1    // 电池电压检测 (U1.39)
#define CHRG_DET_IO         GPIO_NUM_2    // 充电状态指示 (U1.38)
#define WS2812_IO           GPIO_NUM_48   // RGB 状态指示灯 (U1.25, 3.3V 域 GPIO 推挽, 硬件经电平转换驱动灯珠)
#define SD_CD_IO            GPIO_NUM_38   // SD 卡在位检测 (U1.31, SD_SD)
/* SD_CD 电气: TF-015 机械 CD 开关 + 外部 10K 上拉(R6)到 3.3V。
 * 插入卡片时开关闭合把 SD_CD 拉低 → 低电平=已插入 (active-low)。
 * 若实际插座极性相反, 将 SD_CD_ACTIVE_LEVEL 改为 1 即可。 */
#define SD_CD_ACTIVE_LEVEL  0   // 0=低电平表示卡在位 (active-low)
#define POW_EN_IO           GPIO_NUM_40   // 电源锁存 (U1.33)
#define LCD_POW_EN_IO       GPIO_NUM_39   // LCD 软电源开关 (U1.32)

/* ============================================================
 * 磁带机加速参数
 * ============================================================ */
#define TAPE_SPEED_NORMAL   1.0f          // 正常速度
#define TAPE_SPEED_1        2.0f          // 第一档加速（慢速精确定位）
#define TAPE_SPEED_2        4.0f          // 第二档加速（贴近真实磁带倒带下限）
#define TAPE_SPEED_3        8.0f          // 最高加速（跳帧模式）

// 加速档位切换时间阈值 (进入变速态后的累计按住时长) — 无 1x 缓冲，进即 2x
// 档位: 0=2.0x(进入即), 1=4.0x, 2=8.0x
#define TAPE_ACCEL_STEP1_MS  0            // 进入变速态立即 2x（无 1x 缓冲，R045）
#define TAPE_ACCEL_STEP2_MS  5000         // 累计5.0s后进入4x
#define TAPE_ACCEL_STEP3_MS  8000         // 累计8.0s后进入8x（跳帧）

/* ============================================================
 * 音频配置
 * ============================================================ */
#define AUDIO_SAMPLE_RATE   44100

/* 逻辑音量档位 (V1.2): 15 档 (level 0..VOLUME_LEVEL_MAX), 线性 dB 映射 -96..+12 */
#define VOLUME_LEVELS       15            // 逻辑音量档位数
#define VOLUME_LEVEL_MAX    (VOLUME_LEVELS - 1)  // = 14, level 有效范围 0..14
#define AUDIO_OUTPUT_VOL    10            // 默认音量 (0..14, 约 -18.9 dB)

/* ============================================================
 * 播放列表
 * ============================================================ */
#define PLAYLIST_MAX_SIZE   256           // 最大曲目数
#define FILENAME_MAX_LEN    128           // 文件名最大长度

/* ============================================================
 * NVS 命名空间 (断点续播用)
 * ============================================================ */
#define NVS_NAMESPACE       "tapebook"
#define NVS_KEY_POSITION    "last_position"   // 播放位置(s)
#define NVS_KEY_TRACK       "last_track_idx"  // 当前曲目索引
#define NVS_KEY_VOLUME      "volume"          // 音量 0~14 (15 档逻辑音量)
#define NVS_KEY_PLAY_MODE   "play_mode"       // 播放模式 0=顺序
#define NVS_KEY_AUTO_OFF    "auto_off_min"    // 自动关机 (分钟, 0=禁用)
#define NVS_KEY_KEY_BEEP    "key_beep"        // 按键提示音 0/1 (R049c)
#define NVS_KEY_VOICE       "voice"           // 语音播报 0/1 (R049d stub)
#define NVS_KEY_EQ          "eq"              // EQ 模式 0..4 (R049d stub)

/* ============================================================
 * 按键阈值
 * R047：移除 BTN_DOUBLE_CLICK_MS（双击检测已从 button_manager 删除）；
 * 如未来需启用，恢复此宏 + button_manager.cpp 中的双击状态机。
 * ============================================================ */
#define BTN_EXTRA_LONG_MS    3000          // 超长按阈值 (按键锁定)
