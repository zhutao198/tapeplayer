# TapeBook 代码详细设计文档

| 文档信息 | |
|---------|---|
| 项目名称 | TapeBook — 磁带机风格听书机 |
| 文档版本 | 1.0 |
| 编制日期 | 2026-07-02 |
| 关联文档 | PRD.md / DESIGN.md / 现有源码 |

---

## 目录

1. [模块总览与依赖关系](#1-模块总览与依赖关系)
2. [settings — NVS 持久化模块](#2-settings--nvs-持久化模块)
3. [bookmark — 书签模块](#3-bookmark--书签模块)
4. [power_mgmt — 电源管理模块](#4-power_mgmt--电源管理模块)
5. [display — OLED 显示完善](#5-display--oled-显示完善)
6. [audio_player — 音频引擎完善](#6-audio_player--音频引擎完善)
7. [main — 主循环完善](#7-main--主循环完善)
8. [voice_prompt — 语音播报模块](#8-voice_prompt--语音播报模块)
9. [模块间交互序列图](#9-模块间交互序列图)
10. [内存预算与数据尺寸](#10-内存预算与数据尺寸)
11. [A-B 复读模块 (P2 stub)](#11-a-b-复读模块-p2-stub)

---

## 1. 模块总览与依赖关系

```
                    ┌──────────────────────────┐
                    │        main.cpp           │
                    │  状态机 + 事件分发          │
                    └────────┬──────────────────┘
                             │ 调用
            ┌────────┬───────┼───────┬────────┬────────┐
            │        │       │       │        │        │
     ┌──────▼──┐ ┌──▼─────┐ ┌▼─────┐ ┌▼──────┐ ┌▼──────┐ ┌▼──────┐
     │button_  │ │tape_   │ │audio │ │display│ │playlist│ │settings│
     │manager  │ │control │ │player│ │       │ │        │ │        │
     └─────────┘ └────────┘ └──────┘ └──────┘ └────────┘ └──────┘
                                         │           │        │
                                    ┌────▼──┐   ┌───▼──┐   ┌─▼──┐
                                    │u8g2   │   │FATFS │   │NVS │
                                    └───────┘   └──────┘   └────┘

     ┌──────────┐  ┌──────────┐  ┌──────────┐  (V1.1+ / V1.2+)
     │bookmark  │  │power_mgmt│  │voice_    │
     │          │  │          │  │prompt    │
     └──────────┘  └──────────┘  └──────────┘
```

**模块优先级与实现状态**：

| 模块 | 优先级 | 所属层次 | 状态 | 备注 |
|------|--------|----------|------|------|
| config.h | P1 | HAL/配置 | ✅ 已完成 | 引脚/参数配置 |
| button_manager | P1 | 业务逻辑 | ✅ 已完成 | 按键状态机 + dbl_click_en |
| tape_control | P1 | 业务逻辑 | ✅ 已完成 | 磁带档位管理 |
| playlist | P1 | 业务逻辑 | ✅ 已完成 | 结构体排序 + 递归扫描 |
| audio_player | P1 | 音频引擎 | ⚠️ 需完善 | 管道事件监听缺失 |
| display | P1 | 业务逻辑 | ⚠️ 需完善 | 底部提示行对齐、锁定图标 |
| main | P1 | 应用层 | ⚠️ 需完善 | 曲目播完处理、NVS 恢复 |
| settings | P1 | 业务逻辑 | ❌ 未实现 | NVS 读断点/音量/模式 |
| bookmark | P2 | 业务逻辑 | ❌ 未实现 | 书签增删查 |
| power_mgmt | P2 | 业务逻辑 | ❌ 未实现 | 电池 ADC + 休眠 + 屏幕保护 |
| ab_repeat | P2 | 业务逻辑 | ❌ 未实现（stub） | A-B 复读 |
| voice_prompt | P2 | 音频引擎 | ❌ 未实现 | 预录 WAV 播报 |

---

## 2. settings — NVS 持久化模块

### 2.1 头文件 `settings.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化设置模块（打开 NVS handle）
 */
void settings_init(void);

/**
 * @brief 保存断点信息到 NVS
 * @param track_idx  当前曲目索引
 * @param position_s 播放位置（秒）
 * @param file_name  文件名（用于校验）
 */
void settings_save_position(int track_idx, int position_s, const char *file_name);

/**
 * @brief 恢复上次断点
 * @param track_idx_out  输出：曲目索引
 * @param position_s_out 输出：播放位置（秒）
 * @return true=有断点可恢复 / false=无断点或文件已删除
 */
bool settings_load_position(int *track_idx_out, int *position_s_out);

/**
 * @brief 保存音量到 NVS
 */
void settings_save_volume(int volume);

/**
 * @brief 加载音量
 * @return 音量值 0-100，默认 AUDIO_OUTPUT_VOL
 */
int settings_load_volume(void);

/**
 * @brief 保存播放模式
 */
void settings_save_play_mode(int mode);

/**
 * @brief 加载播放模式
 * @return 播放模式 0-2，默认 0 (顺序)
 */
int settings_load_play_mode(void);

/**
 * @brief 保存定时关机设置
 */
void settings_save_auto_off(int minutes);

/**
 * @brief 加载定时关机设置
 * @return 分钟数，0=关闭
 */
int settings_load_auto_off(void);

#ifdef __cplusplus
}
#endif
```

### 2.2 实现 `settings.cpp`

```c
#include "settings.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "settings";
static nvs_handle_t g_nvs_handle = 0;

void settings_init(void)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS (0x%x), erasing...", err);
        nvs_flash_erase();
        nvs_flash_init();
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS still failed after erase, abort");
            return;
        }
    }
    ESP_LOGI(TAG, "NVS opened successfully");
}

void settings_save_position(int track_idx, int position_s, const char *file_name)
{
    if (!g_nvs_handle) return;
    nvs_set_u8(g_nvs_handle, NVS_KEY_TRACK, (uint8_t)track_idx);
    nvs_set_u32(g_nvs_handle, NVS_KEY_POSITION, (uint32_t)position_s);
    // 保存文件名用于校验（SD卡文件可能被删除）
    char key[32];
    snprintf(key, sizeof(key), "book_%d_name", track_idx);
    nvs_set_str(g_nvs_handle, key, file_name);
    nvs_commit(g_nvs_handle);
    ESP_LOGD(TAG, "Saved: track=%d pos=%ds name=%s", track_idx, position_s, file_name);
}

bool settings_load_position(int *track_idx_out, int *position_s_out)
{
    if (!g_nvs_handle) return false;

    uint8_t idx = 0;
    uint32_t pos = 0;
    esp_err_t err;

    err = nvs_get_u8(g_nvs_handle, NVS_KEY_TRACK, &idx);
    if (err != ESP_OK) return false;

    err = nvs_get_u32(g_nvs_handle, NVS_KEY_POSITION, &pos);
    if (err != ESP_OK) pos = 0;

    // 校验文件是否仍存在于 SD 卡
    char key[32];
    snprintf(key, sizeof(key), "book_%d_name", idx);
    char saved_name[FILENAME_MAX_LEN] = "";
    size_t required_size = FILENAME_MAX_LEN;
    err = nvs_get_str(g_nvs_handle, key, saved_name, &required_size);

    if (err == ESP_OK && saved_name[0] != '\0') {
        // 检查播放列表中是否有该文件
        char current_name[FILENAME_MAX_LEN] = "";
        if (playlist_get_name(idx, current_name, sizeof(current_name))) {
            if (strcasecmp(current_name, saved_name) == 0) {
                *track_idx_out = idx;
                *position_s_out = (int)pos;
                ESP_LOGI(TAG, "Resuming: track=%d pos=%ds", idx, (int)pos);
                return true;
            }
        }
        // 文件已不在列表中（被删除或 SD 卡换了）
        ESP_LOGW(TAG, "Saved file '%s' no longer exists, starting from beginning", saved_name);
    }

    return false;
}

void settings_save_volume(int volume)
{
    if (!g_nvs_handle) return;
    nvs_set_u8(g_nvs_handle, NVS_KEY_VOLUME, (uint8_t)volume);
    nvs_commit(g_nvs_handle);
}

int settings_load_volume(void)
{
    if (!g_nvs_handle) return AUDIO_OUTPUT_VOL;
    uint8_t vol = AUDIO_OUTPUT_VOL;
    esp_err_t err = nvs_get_u8(g_nvs_handle, NVS_KEY_VOLUME, &vol);
    if (err != ESP_OK) vol = AUDIO_OUTPUT_VOL;
    return (int)vol;
}

void settings_save_play_mode(int mode)
{
    if (!g_nvs_handle) return;
    nvs_set_u8(g_nvs_handle, NVS_KEY_PLAY_MODE, (uint8_t)mode);
    nvs_commit(g_nvs_handle);
}

int settings_load_play_mode(void)
{
    if (!g_nvs_handle) return 0;
    uint8_t mode = 0;
    esp_err_t err = nvs_get_u8(g_nvs_handle, NVS_KEY_PLAY_MODE, &mode);
    if (err != ESP_OK) mode = 0;
    return (int)mode;
}

void settings_save_auto_off(int minutes)
{
    if (!g_nvs_handle) return;
    nvs_set_u8(g_nvs_handle, "auto_off_min", (uint8_t)minutes);
    nvs_commit(g_nvs_handle);
}

int settings_load_auto_off(void)
{
    if (!g_nvs_handle) return 0;
    uint8_t min = 0;
    nvs_get_u8(g_nvs_handle, "auto_off_min", &min);
    return (int)min;
}
```

### 2.3 断点保存时机

> **完整断点恢复流程见 §9.1 "启动恢复断点"序列图**。

| 时机 | 触发位置 | 调用 |
|------|---------|------|
| 用户停止播放 | `main.cpp stop_playback()` | `settings_save_position(idx, pos, name)` |
| 切换曲目 | `main.cpp play_current_track()` | 先保存旧位置 |
| 曲目自然播完 | `main.cpp` 音频回调 | 保存旧位置（标记 ≥95% 清零） |
| 每 30 秒自动 | `main.cpp` 主循环计时 | `settings_save_position(...)` |
| 低电关机 | `power_mgmt` | 紧急保存 |
| 音量变化松开 | `main.cpp` Prev/Next RELEASE | `settings_save_volume(vol)` |
| 播放模式切换 | `main.cpp` cycle_play_mode() | `settings_save_play_mode(mode)` |

---

## 3. bookmark — 书签模块

### 3.1 头文件 `bookmark.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOKMARK_MAX_PER_FILE  10
#define BOOKMARK_KEY_PREFIX    "bm_"

typedef struct {
    int  position_s;     // 书签位置（秒）
    char label[16];      // 可选标签（如 "CH03"）
} bookmark_t;

/**
 * @brief 初始化书签模块
 */
void bookmark_init(void);

/**
 * @brief 在当前文件当前位置添加书签
 * @param file_idx  文件索引
 * @param position_s 位置（秒）
 * @return 书签编号(0~9)，-1=已满
 */
int bookmark_add(int file_idx, int position_s);

/**
 * @brief 删除指定书签
 */
bool bookmark_delete(int file_idx, int bm_idx);

/**
 * @brief 获取指定文件的书签列表
 * @param file_idx  文件索引
 * @param out       输出数组
 * @param max_count 数组容量
 * @return 实际书签数
 */
int bookmark_list(int file_idx, bookmark_t *out, int max_count);

/**
 * @brief 跳转到指定书签
 * @param file_idx  文件索引
 * @param bm_idx    书签编号
 * @return 书签位置（秒），-1=不存在
 */
int bookmark_jump(int file_idx, int bm_idx);

#ifdef __cplusplus
}
#endif
```

### 3.2 NVS 存储格式

```
Namespace: "tapebook"
Key:  "bm_{file_idx}_{bm_idx}_pos"  → uint32 (秒数)
Key:  "bm_{file_idx}_{bm_idx}_lbl"  → string (标签)
```

每个文件最多 10 个书签，占用 20 个 NVS key。256 文件 × 20 keys = 5120 keys，
但实际只保存有书签的文件，NVS 24KB 足够。

---

## 4. power_mgmt — 电源管理模块

### 4.1 头文件 `power_mgmt.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BAT_STATE_NORMAL = 0,   // > 15%
    BAT_STATE_LOW = 1,      // 5% ~ 15%
    BAT_STATE_CRITICAL = 2, // < 5%
} bat_state_t;

/**
 * @brief 初始化电源管理（ADC + 定时器）
 */
void power_mgmt_init(void);

/**
 * @brief 每 1 秒调用一次（在软件定时器中）
 */
void power_mgmt_tick(void);

/**
 * @brief 获取电量百分比 (0~100)
 */
int power_mgmt_get_battery_percent(void);

/**
 * @brief 获取电池状态
 */
bat_state_t power_mgmt_get_state(void);

/**
 * @brief 检查是否需要关机保护
 */
bool power_mgmt_should_shutdown(void);

/**
 * @brief 设置/获取自动休眠超时（分钟）
 */
void power_mgmt_set_sleep_timeout(int minutes);
int  power_mgmt_get_sleep_timeout(void);

/**
 * @brief 设置定时关机（分钟）
 * @param minutes 0=关闭, 15/30/60/90
 */
void power_mgmt_set_auto_off(int minutes);

/**
 * @brief 检查定时关机是否到期
 */
bool power_mgmt_auto_off_expired(void);

#ifdef __cplusplus
}
#endif
```

### 4.2 电池 ADC 读取

```c
// GPIO3 (ADC1_CH2) → 1:1 分压 → 100nF 去耦
// 18650 满充 4.2V → ADC 读 ~2.1V → 百分比 100%
// 18650 标称 3.7V → ADC 读 ~1.85V → 百分比 ~50%
// 18650 截止 3.0V → ADC 读 ~1.5V → 百分比 0%

static int read_battery_percent(void)
{
    // ADC1_CH2 对应 GPIO3
    // 使用 esp_adc_cal 读取并校准
    int raw = adc1_get_raw(ADC1_CHANNEL_2);
    if (raw < 0) return 0;

    // 12-bit ADC: raw = 0~4095, 参考电压 ~1.1V
    // 分压后: V_bat = 2 × V_adc
    // V_adc = raw × 1.1 / 4095
    // V_bat = 2 × raw × 1.1 / 4095

    float v_bat = 2.0f * (float)raw * 1.1f / 4095.0f;

    // 线性映射: 3.0V=0%, 4.2V=100%（简化模型）
    int pct = (int)((v_bat - 3.0f) / (4.2f - 3.0f) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
```

### 4.2b 屏幕保护与休眠区分

DESIGN §5.7.2 电源管理状态机有 SCREENSAVER 状态，但当前仅实现了 `power_mgmt_should_sleep()`。需要补充屏幕保护判定：

```c
static uint64_t g_last_screen_activity_us = 0;
static int      g_screen_off_timeout_min = 2;  // 默认 2 分钟无操作后屏幕变暗

bool power_mgmt_should_screen_off(void)
{
    if (g_screen_off_timeout_min <= 0) return false;
    uint64_t elapsed_s = (esp_timer_get_time() - g_last_screen_activity_us) / 1000000;
    return elapsed_s >= (uint64_t)(g_screen_off_timeout_min * 60);
}

void power_mgmt_record_screen_activity(void)  // 按键事件时调用
{
    g_last_screen_activity_us = esp_timer_get_time();
}
```

**屏幕保护 vs 休眠边界**：
- **屏幕保护**：OLED 变暗/关闭，CPU 继续运行，音频继续播放。2 分钟无操作触发。
- **系统休眠**：进入 light-sleep，CPU 暂停，按键 GPIO 中断唤醒。5 分钟无操作触发。
- 流程：先屏幕保护 → 再系统休眠，两阶段独立计时。

### 4.3 自动休眠逻辑

```c
static uint64_t g_last_activity_us = 0;
static int      g_sleep_timeout_min = 5; // 默认 5 分钟

void power_mgmt_record_activity(void)  // 主循环在按键事件时调用
{
    g_last_activity_us = esp_timer_get_time();
}

bool power_mgmt_should_sleep(void)
{
    if (g_sleep_timeout_min <= 0) return false;
    uint64_t elapsed_s = (esp_timer_get_time() - g_last_activity_us) / 1000000;
    return elapsed_s >= (uint64_t)(g_sleep_timeout_min * 60);
}
```

### 4.4 定时关机

```c
static int      g_auto_off_min = 0;        // 0=关闭
static uint64_t g_auto_off_start_us = 0;   // 设置时的时间戳

void power_mgmt_set_auto_off(int minutes)
{
    g_auto_off_min = minutes;
    g_auto_off_start_us = (minutes > 0) ? esp_timer_get_time() : 0;
    settings_save_auto_off(minutes);
}

bool power_mgmt_auto_off_expired(void)
{
    if (g_auto_off_min <= 0) return false;
    uint64_t elapsed_s = (esp_timer_get_time() - g_auto_off_start_us) / 1000000;
    return elapsed_s >= (uint64_t)(g_auto_off_min * 60);
}
```

---

## 5. display — OLED 显示完善

### 5.1 需要修改的要点

1. **底部操作提示行**：与 PRD/DESIGN 对齐

```
当前显示: "<Prev  Play  Stop  Next>  FF^RW"
应为:     "RW  <<  >  #  >>  FF   VOL-/+"
```

2. **状态栏增加图标**：

```
▶ 03/12  L  B85  V70  →
```
- L = 锁定图标
- B85 = 电量百分比
- V70 = 音量百分比
- → = 顺序播放模式图标

3. **锁定状态特殊显示**：显示大 `L` 图标居中

4. **文件名滚动**：长文件名超过 21 字符时水平滚动

### 5.2 display_update 完善伪代码

```c
void display_update(...)
{
    u8g2_ClearBuffer(&u8g2);

    // === 状态栏 (y=0~10) ===
    u8g2_SetFont(&u8g2, u8g2_font_5x8_tf);

    char status[30];
    const char *icon = state_icon(state);   // > / || / # / >> / <<
    const char *mode_icon = mode_str(g_play_mode);  // → / ↻ / ↻1

    // 如果锁定，状态栏显示 L
    if (g_key_locked) {
        snprintf(status, sizeof(status), "L %02d/%03d", track_idx, total);
    } else {
        snprintf(status, sizeof(status), "%s %02d/%03d V%d %s",
                 icon, track_idx, total, volume, mode_icon);
    }
    u8g2_DrawStr(&u8g2, 0, 8, status);

    // === 文件名 (y=14~28) ===
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
    draw_scrolling_filename(track_name);

    // === 进度条 (y=30~42) ===
    draw_progress_bar(current_sec, total_sec);

    // === 时间+速度 (y=46~56) ===
    u8g2_SetFont(&u8g2, u8g2_font_5x8_tf);
    char time_str[32];
    format_time(current_sec, cur, sizeof(cur));
    format_time(total_sec, tot, sizeof(tot));
    if (gear > 0) {
        snprintf(time_str, sizeof(time_str), "%s/%s [%s]", cur, tot, gear_str(gear));
    } else {
        snprintf(time_str, sizeof(time_str), "%s/%s", cur, tot);
    }
    u8g2_DrawStr(&u8g2, 0, 54, time_str);

    // === 底部提示 (y=58~64) ===
    u8g2_SetFont(&u8g2, u8g2_font_4x6_tf);
    u8g2_DrawStr(&u8g2, 0, 63, "RW << > # >> FF VOL-/+");
    // 或在音量调节时临时显示 "VOL: 45%"

    u8g2_SendBuffer(&u8g2);
}
```

### 5.3 播放模式图标映射

```c
static const char *mode_icon_str(int mode)
{
    switch (mode) {
    case PLAY_MODE_SEQUENCE:  return "->";   // → 顺序
    case PLAY_MODE_REPEAT_ALL: return "~>";   // ↻ 全循环
    case PLAY_MODE_REPEAT_ONE: return "~1";   // ↻1 单曲
    default: return "->";
    }
}
```

---

## 6. audio_player — 音频引擎完善

### 6.1 管道事件监听（缺失项）

当前 `audio_player.cpp` 没有监听 ESP-ADF 管道事件。需要添加：

```c
// 在 audio_player_play() 中注册事件监听器
static audio_event_iface_handle_t g_evt_iface = NULL;

// 初始化时创建事件接口
void audio_player_init(void)
{
    // ... I2S writer 创建 ...

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    g_evt_iface = audio_event_iface_init(&evt_cfg);
}

// 在 play() 中注册
audio_pipeline_set_listener(g_pipeline, g_evt_iface);

// 在 tick() 中检查事件
void audio_player_tick(void)
{
    // ... 跳帧逻辑 ...

    // 检查管道事件
    audio_event_iface_msg_t msg;
    esp_err_t ret = audio_event_iface_listen(g_evt_iface, &msg, 0); // 非阻塞
    if (ret == ESP_OK) {
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
            if (msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                // 更新总时长和采样率信息
                int duration = 0;
                // 从 msg.data 解码获取 duration
                g_total_duration_ms = duration * 1000;
                ESP_LOGI(TAG, "Duration: %ds", duration);
            }
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_PIPELINE &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
            if (msg.data == AEL_STATUS_STATE_FINISHED) {
                // 曲目播完
                ESP_LOGI(TAG, "Track finished naturally");
                g_is_playing = false;
                if (g_status_cb) g_status_cb(0, g_user_data);
            }
        }
    }
}
```

### 6.2 快退时 1.5x/2.5x 也需 seek

当前跳帧逻辑：

```c
bool need_seek = (abs_speed >= 4.0f) || (mode == TAPE_MODE_REWIND);
```

快退所有档位都 seek（正确，因为没有"倒放"能力），但快进 1.5x/2.5x 不跳帧（仅变速）。
这是正确的设计——1.5x/2.5x 靠 I2S 采样率提高实现变调加速，无需 seek。

### 6.3 SD 卡挂载需在 main.cpp 中完成

当前 `playlist_scan()` 假设 SD 卡已挂载。需要在 `init_storage()` 中先挂载：

```c
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static sdmmc_card_t *g_sd_card = NULL;

static bool mount_sd_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40MHz

    sdspi_device_config_t device_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    device_cfg.host_id = SD_SPI_HOST;
    device_cfg.gpio_cs = SD_CS_IO;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host,
                                              &device_cfg, &mount_config, &g_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (0x%x)", ret);
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted: %s", g_sd_card->cid.name);
    return true;
}

static void init_storage(void)
{
    if (!mount_sd_card()) {
        display_show_no_card();
        g_app_state = APP_STATE_IDLE;
        return;
    }

    int count = playlist_scan(SD_MOUNT_POINT);
    ESP_LOGI(TAG, "Found %d audio files on SD card", count);

    if (count == 0) {
        display_show_no_files();
        g_app_state = APP_STATE_IDLE;
    } else {
        // 从 NVS 恢复断点
        int saved_idx = 0, saved_pos = 0;
        if (settings_load_position(&saved_idx, &saved_pos)) {
            g_current_track = saved_idx;
            g_app_state = APP_STATE_STOPPED;
            ESP_LOGI(TAG, "Resuming from track %d at %ds", saved_idx, saved_pos);
            // 播放时从 saved_pos seek
        } else {
            g_current_track = 0;
            g_app_state = APP_STATE_STOPPED;
        }
    }
}
```

---

## 7. main — 主循环完善

### 7.1 曲目播完处理（缺失项）

> **完整断点恢复流程见 §9.1 "启动恢复断点"序列图**。

当前 `main.cpp` 没有处理曲目自然播完的情况。需要添加音频回调：

```c
static void on_track_finished(int state, void *user_data)
{
    ESP_LOGI(TAG, "Track finished callback");

    // 保存当前位置（标记为已听完）
    char name[FILENAME_MAX_LEN] = "";
    playlist_get_name(g_current_track, name, sizeof(name));
    settings_save_position(g_current_track, 0, name);  // 位置归零=从头

    // 根据播放模式决定下一首
    switch (g_play_mode) {
    case PLAY_MODE_SEQUENCE:
        // 顺序播放：不是最后一首 → 播下一首；最后一首 → 停止
        if (g_current_track < playlist_count() - 1) {
            g_current_track = playlist_next();
            play_current_track();
        } else {
            g_app_state = APP_STATE_STOPPED;
            ESP_LOGI(TAG, "Playlist finished");
        }
        break;

    case PLAY_MODE_REPEAT_ALL:
        // 全部循环：回到第一首
        g_current_track = playlist_next();  // 循环
        play_current_track();
        break;

    case PLAY_MODE_REPEAT_ONE:
        // 单曲循环：重新播放当前曲目
        play_current_track();
        break;
    }
}

// 在 app_main() 中注册
audio_player_set_callback(on_track_finished, NULL);
```

### 7.2 播放时 seek 到断点位置

> **完整断点恢复流程见 §9.1 "启动恢复断点"序列图**。

```c
static void play_current_track(void)
{
    // 保存旧曲目位置
    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
        char old_name[FILENAME_MAX_LEN] = "";
        playlist_get_name(g_current_track, old_name, sizeof(old_name));
        settings_save_position(g_current_track,
                               audio_player_get_position(), old_name);
    }

    char filepath[FILENAME_MAX_LEN * 2];
    if (playlist_get_path(g_current_track, filepath, sizeof(filepath))) {
        if (audio_player_play(filepath)) {
            // 如果有断点位置且不是已听完的
            int saved_pos = g_seek_on_play_position;
            if (saved_pos > 0) {
                audio_player_seek(saved_pos);
                g_seek_on_play_position = 0;
            }
            g_app_state = APP_STATE_PLAYING;
            ESP_LOGI(TAG, "Now playing: %s", filepath);
        }
    }
}
```

### 7.3 每 30 秒自动保存断点

```c
static uint64_t g_last_auto_save_us = 0;
#define AUTO_SAVE_INTERVAL_US  (30 * 1000000)  // 30 秒

// 在主循环中
if (g_app_state == APP_STATE_PLAYING) {
    uint64_t now = esp_timer_get_time();
    if ((now - g_last_auto_save_us) >= AUTO_SAVE_INTERVAL_US) {
        char name[FILENAME_MAX_LEN] = "";
        playlist_get_name(g_current_track, name, sizeof(name));
        settings_save_position(g_current_track, audio_player_get_position(), name);
        g_last_auto_save_us = now;
    }
}
```

### 7.4 按键锁定解锁后恢复正确状态

当前解锁后硬设为 `APP_STATE_PLAYING`，应恢复锁定前的状态：

```c
static app_state_t g_state_before_lock = APP_STATE_STOPPED;

// 锁定时
g_key_locked = true;
g_state_before_lock = g_app_state;
g_app_state = APP_STATE_LOCKED;

// 解锁时
g_key_locked = false;
g_app_state = g_state_before_lock;
```

### 7.5 完善的主循环流程图

```
app_main()
  │
  ├── init_hardware()     → NVS / OLED / 按键 / 磁带控制 / 音频
  ├── init_storage()      → 挂载SD / 扫描文件 / 恢复断点
  ├── settings_init()     → 打开NVS
  ├── 加载音量/模式       → settings_load_volume() / load_play_mode()
  │
  └── while(1) 主循环
      ├── handle_button_events()   → 按键事件 → 状态变更
      ├── tape_control_tick()      → 档位计算
      ├── audio_player_tick()      → 管道维护 + 跳帧 + 事件监听
      ├── update_display()         → 200ms 刷新
      ├── auto_save_position()     → 30s 自动保存
      ├── power_mgmt_tick()        → 电量检测 (低频)
      ├── 检查定时关机             → power_mgmt_auto_off_expired()
      ├── 检查自动休眠             → power_mgmt_should_sleep()
      ├── power_mgmt_record_activity()  → 按键时更新活动时间
      │
      └── vTaskDelay(20ms)
```

### 7.6 `main.cpp` 新增全局变量

```c
static app_state_t    g_state_before_lock = APP_STATE_STOPPED;
static int            g_seek_on_play_position = 0;  // 恢复断点时的 seek 目标
static uint64_t       g_last_auto_save_us = 0;
```

---

## 8. voice_prompt — 语音播报模块

### 8.1 头文件 `voice_prompt.h`

```c
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播报当前播放状态
 * "正在播放：三体第一部，第15分30秒"
 */
void voice_prompt_status(void);

/**
 * @brief 播报电量
 * "电量百分之八十五"
 */
void voice_prompt_battery(void);

/**
 * @brief 播报指定文本（从 /voice/ 目录找 WAV 文件）
 * @param text_id 文本标识，如 "playing", "paused", "volume_70"
 */
void voice_prompt_play(const char *text_id);

#ifdef __cplusplus
}
#endif
```

### 8.2 实现方案

语音播报通过**临时切换音频管道**播放预录 WAV：

```c
// /voice/ 目录下的文件映射
// zh_playing.wav  → "正在播放"
// zh_paused.wav   → "已暂停"
// zh_volume_70.wav → "音量百分之七十"
// zh_battery_85.wav → "电量百分之八十五"

// 播报流程：
// 1. 暂停当前音频管道
// 2. 创建临时管道: fatfs_reader → wav_decoder → i2s_writer
// 3. 播放提示 WAV 文件
// 4. WAV 播完 → 销毁临时管道 → 恢复原管道
```

> 注：此模块为 V1.2+ 功能，V1.0 仅保留接口 stub。

---

## 9. 模块间交互序列图

### 9.1 启动恢复断点

```
app_main()
  │
  ├─ nvs_flash_init()
  ├─ settings_init()          → nvs_open("tapebook")
  ├─ display_init() + splash
  ├─ button_manager_init()
  ├─ tape_control_init()
  ├─ audio_player_init()
  │
  ├─ mount_sd_card()
  ├─ playlist_scan("/sdcard") → 递归扫描 → 排序
  ├─ settings_load_position() → 读 idx + pos + 校验文件名
  │   ├─ 文件存在 → g_current_track=idx, g_seek_on_play_position=pos
  │   └─ 文件不存在 → 从头开始
  ├─ settings_load_volume()   → audio_player_set_volume(vol)
  ├─ settings_load_play_mode() → g_play_mode = mode
  │
  └─ 主循环开始
```

### 9.2 快进完整交互

```
T+0ms   用户按下FF键
  │ button_manager: IDLE → DEBOUNCE
T+30ms  去抖完成
  │ button_manager: DEBOUNCE → PRESSED
T+500ms 长按触发
  │ button_manager: PRESSED → LONG_PRESS → event=LONG_PRESS
  │ main: tape_control_ff_press() → enter FF mode, gear=0
  │ main: audio_player_set_speed(1.0) → i2s_stream_set_clk(44100)
  │ main: g_app_state = FAST_FORWARD
T+800ms 档位1
  │ tape_control_tick(): elapsed>=800 → gear=1, speed=1.5
  │ main: audio_player_set_speed(1.5) → i2s_stream_set_clk(66150)
  │ audio_player_tick(): abs_speed<4.0 && mode!=REW → 不跳帧
T+2000ms 档位2
  │ tape_control_tick(): elapsed>=2000 → gear=2, speed=2.5
  │ main: audio_player_set_speed(2.5) → i2s_stream_set_clk(96000)
  │ audio_player_tick(): 不跳帧（2.5x仅变速）
T+4000ms 档位3
  │ tape_control_tick(): elapsed>=4000 → gear=3, speed=4.0
  │ audio_player_tick(): abs_speed>=4.0 → 每50ms seek跳帧
  │   seek(target_ms = cur + 50*4.0 = cur + 200ms)
T+7000ms 档位4
  │ tape_control_tick(): elapsed>=7000 → gear=4, speed=8.0
  │ audio_player_tick(): 每50ms seek跳帧
  │   seek(target_ms = cur + 50*8.0 = cur + 400ms)
T+8500ms 用户松开FF键
  │ button_manager: HOLD → IDLE → event=RELEASE
  │ main: tape_control_ff_release() → exit mode
  │ main: audio_player_set_speed(1.0) → i2s_stream_set_clk(44100)
  │ main: g_app_state = PLAYING
```

### 9.3 音量调节交互

```
用户长按Next键
  │
T+500ms button_manager: LONG_PRESS event
  │ main: g_vol_hold_counter++ → 0 % 5 ≠ 0 → 不调
T+520ms HOLD event (20ms later)
  │ main: g_vol_hold_counter=1 → 1 % 5 ≠ 0
T+540ms HOLD
  │ main: g_vol_hold_counter=2 → 2 % 5 ≠ 0
...
T+600ms HOLD
  │ main: g_vol_hold_counter=5 → 5 % 5 == 0 → vol += 1
  │ audio_player_set_volume(vol+1)
T+620ms HOLD → counter=6
T+700ms HOLD → counter=10 → 10 % 5 == 0 → vol += 1
...
用户松开
  │ RELEASE → g_vol_hold_counter = 0
  │ settings_save_volume(vol)
```

---

## 10. 内存预算与数据尺寸

### 10.1 DRAM 占用

| 变量 | 类型 | 大小 | 备注 |
|------|------|------|------|
| `g_buttons[6]` | btn_ctx_t × 6 | ~240 B | 按键状态机 |
| `g_app_state` 等 | 5 个全局 | ~20 B | 主状态 |
| `g_speed_steps[4]` | speed_step_t × 4 | ~32 B | 磁带档位表 |
| FreeRTOS 栈 | 8KB | 8192 B | 主任务 |
| NVS handle | nvs_handle_t | ~8 B | |
| audio pipeline handles | 3 个指针 | ~12 B | |
| **DRAM 合计** | | **~8.5 KB** | 余量充足（~400KB 可用） |

### 10.2 PSRAM 占用

| 变量 | 类型 | 大小 | 备注 |
|------|------|------|------|
| `g_items[256]` | playlist_item_t × 256 | ~96 KB | 名称128+路径256 |
| `g_audio_buf` | int16_t × 8192 | ~16 KB | 音频缓冲 |
| ESP-ADF 解码器堆 | 动态 | ~50-100 KB | 解码器内部 |
| FATFS 缓冲 | 动态 | ~4 KB | |
| **PSRAM 合计** | | **~166-216 KB** | 8MB PSRAM 余量巨大 |

### 10.3 NVS 占用

| Key | 大小 | 数量 | 合计 |
|-----|------|------|------|
| last_track_idx | 1 B | 1 | 1 B |
| last_position | 4 B | 1 | 4 B |
| volume | 1 B | 1 | 1 B |
| play_mode | 1 B | 1 | 1 B |
| auto_off_min | 1 B | 1 | 1 B |
| ab_repeat | 4 B | 1 | 4 B |
| book_N_name | ~64 B | ≤20 | ~1.3 KB |
| book_N_pos | 4 B | ≤20 | 80 B |
| bm_N_N_pos | 4 B | ≤2560 | ~10 KB (理论) |
| **NVS 合计** | | | **~12 KB** (24KB 分区足够) |

---

## 11. A-B 复读模块 (P2 stub)

> 本模块为 V1.1+ 功能，V1.0 仅保留接口 stub 和 NVS key 定义，确保数据格式向后兼容。

### 11.1 头文件 `ab_repeat.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AB_STATE_IDLE = 0,    // 未设置
    AB_STATE_A_SET = 1,   // A 点已标记
    AB_STATE_ACTIVE = 2,  // A-B 区间循环中
} ab_state_t;

/**
 * @brief 初始化 A-B 复读模块
 */
void ab_repeat_init(void);

/**
 * @brief 处理 FF+RW 组合键事件（标记 A/B 点或取消）
 * 操作流程（PRD §4.2.10）：
 *   第1次: 标记A点 → AB_STATE_A_SET
 *   第2次: 标记B点 → AB_STATE_ACTIVE（循环播放）
 *   第3次: 取消区间 → AB_STATE_IDLE
 */
ab_state_t ab_repeat_toggle(void);

/**
 * @brief 检查当前播放位置是否到达 B 点（循环回跳判定）
 * @param current_pos_s 当前位置（秒）
 * @return true=应跳回A点 / false=继续正常播放
 */
bool ab_repeat_should_loop(int current_pos_s);

/**
 * @brief 获取 A 点位置（秒）
 */
int ab_repeat_get_a(void);

/**
 * @brief 获取 B 点位置（秒）
 */
int ab_repeat_get_b(void);

/**
 * @brief 获取当前状态
 */
ab_state_t ab_repeat_get_state(void);

/**
 * @brief 设置循环次数（0=无限）
 */
void ab_repeat_set_loop_count(int count);

#ifdef __cplusplus
}
#endif
```

### 11.2 NVS 存储

```
Namespace: "tapebook"
Key: "ab_repeat"  → uint32  → 高16bit=A点秒数, 低16bit=B点秒数
                        值=0 表示无A-B区间
```

与 DESIGN §5.5.1 定义完全一致。V1.0 版本中 NVS key 已预留，但读写逻辑暂不实现。

### 11.3 状态机

```
       ┌──────────┐ FF+RW组合键 ┌──────────┐ FF+RW组合键 ┌──────────┐
       │  IDLE    ├───────────►│  A_SET   ├───────────►│  ACTIVE  │
       │(无区间)  │            │(A点已标记) │            │(A-B循环) │
       └──────────┘            └──────────┘            └──────────┘
                                                         │ FF+RW组合键
                                                         ▼
                                                    回到 IDLE
```

### 11.4 V1.0 实现范围

- ✅ NVS key `ab_repeat` 已在 config.h / DESIGN 中定义
- ✅ 接口头文件 `ab_repeat.h` 已定义（stub）
- ❌ 实际循环播放逻辑（B点回跳A点）待 V1.1 实现
- ❌ 组合键检测（FF+RW同时按）待 V1.1 实现
- ❌ 循环次数配置待 V1.1 实现

---

## 附录：需要新增的文件

| 文件 | 优先级 | 说明 |
|------|--------|------|
| `settings.h` / `settings.cpp` | P1 | NVS 持久化（断点、音量、模式） |
| `bookmark.h` / `bookmark.cpp` | P2 | 书签增删查 |
| `power_mgmt.h` / `power_mgmt.cpp` | P2 | 电池ADC、休眠、屏幕保护、定时关机 |
| `ab_repeat.h` / `ab_repeat.cpp` | P2 | A-B 复读 stub（V1.1+ 完整实现） |
| `voice_prompt.h` / `voice_prompt.cpp` | P2 | 语音播报 stub |

### `CMakeLists.txt` 更新

```cmake
idf_component_register(
    SRCS
        "main.cpp"
        "button_manager.cpp"
        "tape_control.cpp"
        "playlist.cpp"
        "display.cpp"
        "audio_player.cpp"
        "settings.cpp"
        # "bookmark.cpp"        # V1.2+
        # "ab_repeat.cpp"       # V1.1+
        # "power_mgmt.cpp"      # V1.1+
        # "voice_prompt.cpp"    # V1.2+
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        nvs_flash
        esp_timer
        fatfs
        esp_adc_cal    # 新增：电池ADC校准
)
```

### `sdkconfig.defaults` 新增

```
# ADC 校准（电池检测）
CONFIG_ADC_CAL_EFUSE_TP_ENABLE=y
CONFIG_ADC_CAL_EFUSE_VREF_ENABLE=y
```
