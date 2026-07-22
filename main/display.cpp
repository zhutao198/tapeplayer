/**
 * @file display.cpp
 * @brief SSD1306 OLED 显示实现 (使用 u8g2 库)
 *
 * 屏幕布局 (128x64):
 *
 *  ┌──────────────────────┐
 *  │  ▶ 01/12   ██░░ 88%  │  Line 0: 状态图标 + 曲目编号 + 音量
 *  │ 文件名.mp3           │  Line 1: 曲目名称 (滚动)
 *  │  ████████████████░░  │  Line 2: 进度条
 *  │  01:23 / 45:67  ▶▶2x │  Line 3: 时间 / 速度
 *  └──────────────────────┘
 */

#include "display.h"
#include "config.h"
#include "tape_control.h"
#include "esp_timer.h"

// u8g2 库头文件，如果使用简化方案可以用 SSD1306 驱动
// 这里演示使用 u8g2 的接口

// R035-017：本块注释原声称"经 idf_component.yml 拉取 u8g2"，但 yml 中 dependencies 被整体注释，
// 实际仍由 components/u8g2_esp32_hal + components/u8g2 手动源码方式提供。
// 2026-07-03 R002: 从手动源码 components/u8g2 切换为 idf component 依赖（待 R003 启用，参见 R035-018）
// 2026-07-22 R035-017: 回归手动源码（u8g2_esp32_hal 已存在），与 idf_component.yml 当前状态一致

#ifdef CONFIG_USE_U8G2

#include "u8g2.h"
#include "esp_err.h"
#include "esp_log.h"
#include "u8g2_esp32_hal.h"

static const char *TAG = "display";

static u8g2_t u8g2;

/* 脏区检查：指纹相同则跳过 SendBuffer，降低 I2C 流量 */
static uint32_t g_display_fp = 0;
static uint64_t g_display_last_update_us = 0;
static bool     g_display_sleep = false;
static bool     g_display_initialized = false;   // R032-207: I2C 未就绪时阻止对未初始化 u8g2 的调用

static uint32_t calc_fingerprint(player_state_t state,
                                  int track_idx, int total,
                                  int current_sec, int total_sec,
                                  float speed, int gear, int volume)
{
    uint32_t h = (uint32_t)state;
    h = h * 31 + (uint32_t)track_idx;
    h = h * 31 + (uint32_t)total;
    h = h * 31 + (uint32_t)current_sec;
    h = h * 31 + (uint32_t)total_sec;
    h = h * 31 + (uint32_t)(int)(speed * 10);
    h = h * 31 + (uint32_t)gear;
    h = h * 31 + (uint32_t)volume;
    return h;
}

#define SCREEN_SAVER_TIMEOUT_US  (30 * 1000000ULL)  // 30s 无变化进入屏保

static u8g2_esp32_hal_t s_u8g2_hal;

void display_init(void)
{
    s_u8g2_hal.sda = DISPLAY_SDA_IO;
    s_u8g2_hal.scl = DISPLAY_SCL_IO;
    if (u8g2_esp32_hal_init(s_u8g2_hal) != ESP_OK) {
        ESP_LOGE(TAG, "u8g2 I2C init failed, display disabled");
        return;
    }

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
    u8x8_SetI2CAddress(&u8g2.u8x8, 0x3C << 1);

    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_ClearBuffer(&u8g2);
    u8g2_SendBuffer(&u8g2);
    g_display_initialized = true;   // R032-207
}

void display_show_splash(void)
{
    if (!g_display_initialized) return;   // R032-207
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&u8g2, 10, 20, "Audiobook Player");
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&u8g2, 20, 40, "ESP32-S3 Edition");
    u8g2_DrawStr(&u8g2, 15, 55, "Loading SD card...");
    u8g2_SendBuffer(&u8g2);
}

void display_show_no_files(void)
{
    if (!g_display_initialized) return;   // R032-207
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&u8g2, 15, 25, "No Audio Files");
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&u8g2, 5, 45, "Put .mp3/.flac/.wav");
    u8g2_DrawStr(&u8g2, 10, 55, "files on SD card");
    u8g2_SendBuffer(&u8g2);
}

void display_show_no_card(void)
{
    if (!g_display_initialized) return;   // R032-207
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(&u8g2, 10, 25, "No SD Card");
    u8g2_SetFont(&u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&u8g2, 5, 45, "Please insert SD card");
    u8g2_SendBuffer(&u8g2);
}

static const char *state_icon(player_state_t state)
{
    switch (state) {
    case PLAYER_STATE_PLAYING:       return ">";
    case PLAYER_STATE_PAUSED:        return "||";
    case PLAYER_STATE_STOPPED:       return "#";
    case PLAYER_STATE_FAST_FORWARD:  return ">>";
    case PLAYER_STATE_REWIND:        return "<<";
    case PLAYER_STATE_LOCKED:        return "[]";   // M2：锁定态独立图标
    default:                         return "?";
    }
}

static const char *gear_str(int gear)
{
    // R035-006：标签与 tape_control.cpp 的 g_speed_steps[] 联动（单源），
    // 修改档位定义时无需同步 display.cpp。
    static char s_gear_buf[8];
    float speed = tape_control_get_gear_speed(gear);
    if (speed <= 0.0f) return "";
    snprintf(s_gear_buf, sizeof(s_gear_buf), "%.1fx", speed);
    return s_gear_buf;
}

static void format_time(int seconds, char *buf, size_t size)
{
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0) {
        snprintf(buf, size, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, size, "%02d:%02d", m, s);
    }
}

void display_update(player_state_t state,
                    const char *track_name,
                    int track_idx, int total,
                    int current_sec, int total_sec,
                    float speed, int gear, int volume)
{
    if (!g_display_initialized) return;   // R032-207

    uint64_t now = esp_timer_get_time();

    /* 脏区检查：帧内容无变化则跳过 I2C 刷新 */
    uint32_t fp = calc_fingerprint(state, track_idx, total,
                                    current_sec, total_sec, speed, gear, volume);
    if (fp == g_display_fp) {
        /* 屏保：如果内容持续不变超过 30s，关闭显示 */
        if (!g_display_sleep &&
            (now - g_display_last_update_us) >= SCREEN_SAVER_TIMEOUT_US) {
            u8g2_SetPowerSave(&u8g2, 1);
            g_display_sleep = true;
        }
        return;
    }
    g_display_fp = fp;
    g_display_last_update_us = now;

    /* 从屏保唤醒 */
    if (g_display_sleep) {
        u8g2_SetPowerSave(&u8g2, 0);
        g_display_sleep = false;
    }

    u8g2_ClearBuffer(&u8g2);

    /* --- Line 0: 状态行 --- */
    u8g2_SetFont(&u8g2, u8g2_font_5x8_tf);
    char line0[32];
    snprintf(line0, sizeof(line0), "%s %02d/%03d  Vol:%d%%",
             state_icon(state), track_idx, total, volume);
    u8g2_DrawStr(&u8g2, 0, 8, line0);

    /* --- Line 1: 文件名 (截断) --- */
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    char fname[22];
    snprintf(fname, sizeof(fname), "%.21s", track_name);
    u8g2_DrawStr(&u8g2, 0, 21, fname);

    /* --- Line 2: 进度条 --- */
    u8g2_DrawFrame(&u8g2, 0, 30, 128, 12);
    if (total_sec > 0) {
        int bar_w = (int)(126.0f * current_sec / total_sec);
        if (bar_w < 0) bar_w = 0;
        if (bar_w > 126) bar_w = 126;
        if (bar_w > 0) {
            // R032-308：暂停态用反色填充进度条以"灰显/冻结"，与 PLAYING 的实心填充区分；
            // 位置数值（current_sec）仍准确，仅视觉提示已暂停。
            if (state == PLAYER_STATE_PAUSED) {
                u8g2_SetDrawColor(&u8g2, 0);
                u8g2_DrawBox(&u8g2, 1, 31, bar_w, 10);
                u8g2_SetDrawColor(&u8g2, 1);
            } else {
                u8g2_DrawBox(&u8g2, 1, 31, bar_w, 10);
            }
        }
    }

    /* --- Line 3: 时间 + 速度 --- */
    u8g2_SetFont(&u8g2, u8g2_font_5x8_tf);
    char time_buf[48];
    char cur[16], tot[16];
    format_time(current_sec, cur, sizeof(cur));
    format_time(total_sec, tot, sizeof(tot));
    const char *gs = gear_str(gear);
    if (gs && gs[0]) {
        snprintf(time_buf, sizeof(time_buf), "%s / %s [%s]",
                 cur, tot, gs);
    } else {
        snprintf(time_buf, sizeof(time_buf), "%s / %s", cur, tot);
    }
    u8g2_DrawStr(&u8g2, 0, 55, time_buf);

    /* --- Line 4: 底部操作提示 --- */
    u8g2_SetFont(&u8g2, u8g2_font_4x6_tf);
    // R035-008：原文 "<Prev  Play  Stop  Next>  FF^RW" 28 字符 × ~5 px advance ≈ 140 px 超 128 px 屏宽，
    // 缩短为 "<Prev Stop Next> FF/RW" 22 字符 × ~5 px ≈ 110 px，留余量
    u8g2_DrawStr(&u8g2, 0, 63, "<Prev Stop Next> FF/RW");

    u8g2_SendBuffer(&u8g2);
}

#define BROWSE_HEADER_Y      8
#define BROWSE_LINE_HEIGHT   8
#define BROWSE_FIRST_Y       16

void display_show_browse(int selected, int total, char lines[][24], int count)
{
    if (!g_display_initialized) return;   // R032-207
    if (count <= 0) return;

    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_5x8_tf);

    char header[32];
    snprintf(header, sizeof(header), "> Browse [%d/%d]", selected + 1, total);
    u8g2_DrawStr(&u8g2, 0, BROWSE_HEADER_Y, header);

    int shown = count;
    if (shown > BROWSE_VISIBLE_LINES) shown = BROWSE_VISIBLE_LINES;

    for (int i = 0; i < shown; i++) {
        u8g2_DrawStr(&u8g2, 0, BROWSE_FIRST_Y + i * BROWSE_LINE_HEIGHT, lines[i]);
    }

    u8g2_SendBuffer(&u8g2);
}

#else // CONFIG_USE_U8G2 未启用时的空实现

#include "esp_log.h"

static const char *TAG = "display";

void display_init(void) {
    ESP_LOGW(TAG, "u8g2 not enabled, using serial output only");
}

void display_show_splash(void) {
    ESP_LOGI(TAG, "=== Audiobook Player ===");
}

void display_show_no_files(void) {
    ESP_LOGI(TAG, "No audio files found!");
}

void display_show_no_card(void) {
    ESP_LOGI(TAG, "No SD card inserted!");
}

void display_show_browse(int selected, int total, char lines[][24], int count) {
    ESP_LOGI(TAG, "[Browse %d/%d]", selected + 1, total);
}

void display_update(player_state_t state,
                    const char *track_name,
                    int track_idx, int total,
                    int current_sec, int total_sec,
                    float speed, int gear, int volume)
{
    const char *st = "STOP";
    if (state == PLAYER_STATE_PLAYING) st = "PLAY";
    else if (state == PLAYER_STATE_PAUSED) st = "PAUS";
    else if (state == PLAYER_STATE_FAST_FORWARD) st = "FF>>";
    else if (state == PLAYER_STATE_REWIND) st = "<<RW";
    else if (state == PLAYER_STATE_LOCKED) st = "LOCK";   // M2：锁定态

    ESP_LOGI(TAG, "[%d/%d] %s | %s | %ds/%ds | %.1fx vol:%d",
             track_idx, total, st, track_name, current_sec, total_sec, speed, volume);
}

#endif // CONFIG_USE_U8G2
