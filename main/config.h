/**
 * @file config.h
 * @brief 引脚定义和全局配置常量
 */

#pragma once

#include "driver/gpio.h"

/* ============================================================
 * 音频输出 (I2S - MAX98357)
 * ============================================================ */
#define I2S_BCK_IO          GPIO_NUM_4
#define I2S_WS_IO           GPIO_NUM_5
#define I2S_DOUT_IO         GPIO_NUM_6
// MAX98357A 不需要 MCLK，I2S_MCLK_IO 删除（M-7）

/* ============================================================
 * MicroSD 卡 (SPI)
 * ============================================================ */
#define SD_SPI_HOST         SPI2_HOST
#define SD_CS_IO            GPIO_NUM_10
#define SD_MOSI_IO          GPIO_NUM_11
#define SD_MISO_IO          GPIO_NUM_12
#define SD_SCLK_IO          GPIO_NUM_13
#define SD_MOUNT_POINT      "/sdcard"

/* ============================================================
 * OLED 显示屏 (I2C - SSD1306)
 * ============================================================ */
#define DISPLAY_I2C_PORT    I2C_NUM_0
#define DISPLAY_SDA_IO      GPIO_NUM_17
#define DISPLAY_SCL_IO      GPIO_NUM_18
#define DISPLAY_WIDTH       128
#define DISPLAY_HEIGHT      64

/* ============================================================
 * 按键 (内部上拉，按下为低电平)
 * ============================================================ */
#define BTN_PLAY_PAUSE      GPIO_NUM_1
#define BTN_STOP            GPIO_NUM_2
#define BTN_PREV            GPIO_NUM_8
#define BTN_NEXT            GPIO_NUM_9
#define BTN_REWIND          GPIO_NUM_14   // 快退 (按住)
#define BTN_FAST_FORWARD    GPIO_NUM_15   // 快进 (按住)

#define BTN_DEBOUNCE_MS     30            // 按键去抖时间 (ms)
#define BTN_LONG_PRESS_MS   500           // 长按判定时间 (ms)
#define BTN_SCAN_INTERVAL   20            // 按键扫描间隔 (ms)

/* ============================================================
 * 磁带机加速参数
 * ============================================================ */
#define TAPE_SPEED_NORMAL   1.0f          // 正常速度
#define TAPE_SPEED_1        1.5f          // 第一档加速
#define TAPE_SPEED_2        2.0f          // 第二档加速（C3: 2.5→2.0）
#define TAPE_SPEED_3        3.0f          // 第三档加速（C3: 4.0→3.0）
#define TAPE_SPEED_4        8.0f          // 最高加速

// 加速档位切换时间阈值 (按住时长)
#define TAPE_ACCEL_STEP1_MS  800          // 0.8s后进入1.5x
#define TAPE_ACCEL_STEP2_MS  2000         // 2.0s后进入2.5x
#define TAPE_ACCEL_STEP3_MS  4000         // 4.0s后进入4x
#define TAPE_ACCEL_STEP4_MS  7000         // 7.0s后进入8x

/* ============================================================
 * 音频配置
 * ============================================================ */
#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_BUFFER_SIZE   (8 * 1024)    // 8KB 缓冲区
#define AUDIO_OUTPUT_VOL    70             // 默认音量 0-100
#define AUDIO_FILE_EXT_MAX  8              // 文件扩展名最大长度

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
#define NVS_KEY_VOLUME      "volume"          // 音量 0~100
#define NVS_KEY_PLAY_MODE   "play_mode"       // 播放模式 0=顺序

/* ============================================================
 * 按键阈值
 * ============================================================ */
#define BTN_DOUBLE_CLICK_MS  300           // 双击判定窗口 (ms)
#define BTN_EXTRA_LONG_MS    3000          // 超长按阈值 (按键锁定)
