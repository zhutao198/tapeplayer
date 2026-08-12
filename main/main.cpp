/**
 * @file main.cpp
 * @brief ESP32-S3 听书机主程序
 *
 * 主循环逻辑：
 * 1. 按键扫描 → 事件分发（支持短按/双击/长按/超长按/HOLD/RELEASE）
 * 2. 磁带控制器 tick → 档位切换
 * 3. 音频播放器 tick → 管道维护/跳帧/事件监听
 * 4. 设置自动保存 → 每 30 秒保存断点
 * 5. 电源管理 tick → 电量检测/定时关机
 * 6. 显示屏刷新
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_sleep.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

#include "config.h"
#include "button_manager.h"
#include "tape_control.h"
#include "playlist.h"
#include "display.h"
#include "audio_player.h"
#include "settings.h"
#include "power_mgmt.h"
#include "menu.h"
#include "ota_sd.h"
#include "esp_ota_ops.h"

#include "bookmark.h"
#include "led_strip.h"

static const char *TAG = "main";

/* ============================================================
 * 播放模式（与 DESIGN 8.2 一致）
 * ============================================================ */
typedef enum {
    PLAY_MODE_SEQUENCE = 0,    // 顺序播放：播完列表后停止
    PLAY_MODE_REPEAT_ALL,      // 全部循环
    PLAY_MODE_REPEAT_ONE,      // 单曲循环
} play_mode_t;

/* ============================================================
 * 全局状态（与 DESIGN 8.3 一致）
 * ============================================================ */
typedef enum {
    APP_STATE_IDLE,            // 空闲 (无文件)
    APP_STATE_STOPPED,         // 停止
    APP_STATE_PLAYING,         // 播放中
    APP_STATE_PAUSED,          // 暂停
    APP_STATE_FAST_FORWARD,    // 快进（磁带模式）
    APP_STATE_REWIND,          // 快退（磁带模式）
    APP_STATE_BROWSING,        // 文件夹浏览
    APP_STATE_MENU,            // 统一设置菜单 (R049)
    APP_STATE_OTA,             // TF 卡固件升级向导 (R049c 真实化)
} app_state_t;

// 所有全局变量单任务访问，无需 volatile（M-9/L-8: 设计确认 OK）
static app_state_t    g_app_state = APP_STATE_IDLE;
static int            g_current_track = 0;
static uint64_t       g_last_display_update = 0;
static int64_t        g_next_loop_deadline = 0;
static int            g_vol_down_counter = 0;  // 音量减长按计数器（每 5 步调 1 级）
static int            g_vol_up_counter = 0;    // 音量加长按计数器（每 5 步调 1 级）
static int            g_seek_on_play_position = 0;  // 断点恢复 seek 目标（秒）
// g_last_auto_save_us: 与 auto_save/settings_flush/power_mgmt 耦合，单任务下 OK（M-15: 设计级，可接受）
static uint64_t       g_last_auto_save_us = 0;
static play_mode_t    g_play_mode = PLAY_MODE_SEQUENCE;

// 延迟处理：曲目播完 → 主循环处理下一首（避免在回调内嵌套调 play）
static bool           g_pending_track_finished = false;
static int            g_pending_track_next = -1;
static int            g_pending_track_seek = 0;

// 延迟 NVS 保存（避免回调内同步写 NVS）
static int            g_pending_save_track = -1;
static int            g_pending_save_position = 0;

static int            g_browse_index = 0;              // 浏览模式选中索引
static app_state_t    g_state_before_browse = APP_STATE_STOPPED;
static app_state_t    g_state_before_menu   = APP_STATE_STOPPED;
static uint32_t       g_browse_repeat_ms = 0;          // 浏览长按连续移动基准时刻 (hold_ms)

// 组合键 REW+STOP：两键在 COMBO_WINDOW_US 内先后/同时短按 → 跳到当前曲首
static uint64_t       g_combo_rew_us = 0;
static uint64_t       g_combo_stop_us = 0;

// R049c：信息屏覆盖（OTA/USB/关于等桩提示，1.5s 后自动消失）
static char     g_info_title[32] = {0};
static char     g_info_text[64] = {0};
static uint64_t g_info_until_us = 0;
static bool     g_info_active = false;
static bool     g_ota_in_progress = false;  // OTA 写入中：独占 SD 卡，屏蔽主循环插拔处理

/**
 * 浏览模式长按连续移动的间隔 (ms/曲)：随按住时长加速缩短。
 * 刚进入长按用 BROWSE_REPEAT_MS_INIT，超过 BROWSE_HOLD_ACCEL_MS 后用最快间隔。
 */
static uint32_t browse_repeat_interval(uint32_t hold_ms)
{
    if (hold_ms >= BROWSE_HOLD_ACCEL_MS) {
        return BROWSE_REPEAT_MS_MIN;
    }
    if (hold_ms >= BROWSE_HOLD_ACCEL_MS / 2) {
        return BROWSE_REPEAT_MS_FAST;
    }
    return BROWSE_REPEAT_MS_INIT;
}

static sdmmc_card_t   *g_sd_card = NULL;  // SD 卡句柄
static uint64_t    g_last_sd_check_us = 0;
static bool        g_sd_inserted = false; // SD 卡在位(去抖后提交状态)
static int         g_sd_cd_raw = -1;      // SD_CD 原始电平(去抖用)
static int         g_sd_cd_stable_cnt = 0;// 同电平连续采样次数

#define AUTO_SAVE_INTERVAL_US  (30 * 1000000)  // 30 秒自动保存
#define SD_CHECK_INTERVAL_US   (5 * 1000000)   // 5 秒检查 SD 卡状态
#define COMBO_WINDOW_US        (250 * 1000)    // 组合键 REW+STOP 判定窗：250ms 内两键短按算同时

/* ============================================================
 * 辅助：保存当前断点
 * ============================================================ */
static void save_current_position(void)
{
    // R034-002：包含 FF/RW 态，避免快进/快退中掉电后续播点停留在上次普通播放位置
    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
        g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
        char name[FILENAME_MAX_LEN] = "";
        playlist_get_name(g_current_track, name, sizeof(name));
        settings_save_position(g_current_track, audio_player_get_position(), name);
        // S8：seek/切歌后立即 flush，避免断电丢失最近一次断点
        settings_flush();
    }
}

/* ============================================================
 * 辅助：停止/播放/跳转
 * ============================================================ */
static void stop_playback(void)
{
    save_current_position();
    // 缓存当前位置，供 STOPPED→播放 续播（主循环不再清零，见 BTN_ID_PLAY_PAUSE）。
    // 必须在 audio_player_stop() 之前读取，管道销毁后 get_position 失效。
    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
        g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
        g_seek_on_play_position = audio_player_get_position();
    }
    audio_player_stop();
    // 统一退出磁带模式（不限 FF/RW）
    if (g_app_state == APP_STATE_FAST_FORWARD) {
        tape_control_ff_release();
    } else if (g_app_state == APP_STATE_REWIND) {
        tape_control_rewind_release();
    }
    g_app_state = APP_STATE_STOPPED;
}

/* 组合键 REW+STOP：跳到当前曲首（从头）。
 * 仅播放相关态有效：播放/暂停/快进退态立即 seek(0) 并落盘；停止态则把续播位置设为 0。 */
static void jump_to_track_start(void)
{
    if (g_app_state == APP_STATE_FAST_FORWARD)      tape_control_ff_release();
    else if (g_app_state == APP_STATE_REWIND)       tape_control_rewind_release();

    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
        g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
        audio_player_set_speed(TAPE_SPEED_NORMAL);
        audio_player_seek(0);
        save_current_position();   // 落盘曲首位置（0）
        if (g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
            g_app_state = APP_STATE_PLAYING;
        }
    } else if (g_app_state == APP_STATE_STOPPED) {
        g_seek_on_play_position = 0;   // 停止态：下次播放从头
    }
    ESP_LOGI(TAG, "Combo REW+STOP: jump to track start");
}

static void play_current_track(void)
{
    char filepath[FILENAME_MAX_LEN * 4];  // R032-001: 扩到 *4 与 playlist 路径缓冲一致，避免接收时截断
    if (playlist_get_path(g_current_track, filepath, sizeof(filepath))) {
        if (audio_player_play(filepath)) {
            // 如果有断点位置（从 NVS 恢复或切换曲目时指定）
            if (g_seek_on_play_position > 0) {
                audio_player_seek(g_seek_on_play_position);
                g_seek_on_play_position = 0;
            }
            // R034-001：依据 tape_control_get_mode() 还原状态，避免 FF/RW 跨曲时
            // g_app_state 与 tape mode 不一致（行为对但图标/状态机错位）
            tape_mode_t m = tape_control_get_mode();
            if (m == TAPE_MODE_FAST_FORWARD) {
                g_app_state = APP_STATE_FAST_FORWARD;
            } else if (m == TAPE_MODE_REWIND) {
                g_app_state = APP_STATE_REWIND;
            } else {
                g_app_state = APP_STATE_PLAYING;
            }
            g_last_auto_save_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Now playing: %s", filepath);
        }
    }
}

/* 切换播放模式 */
static void cycle_play_mode(void)
{
    g_play_mode = (play_mode_t)((g_play_mode + 1) % 3);
    const char *mode_str[] = {"SEQ", "ALL", "ONE"};
    ESP_LOGI(TAG, "Play mode: %s", mode_str[g_play_mode]);
    settings_save_play_mode((int)g_play_mode);
    display_set_play_mode((int)g_play_mode);
}

/* 短按跳转 ±10s */
static void skip_seconds(int seconds)
{
    if (g_app_state != APP_STATE_PLAYING && g_app_state != APP_STATE_PAUSED) return;

    int cur = audio_player_get_position();
    int new_pos = cur + seconds;
    if (new_pos < 0) new_pos = 0;
    int duration = audio_player_get_duration();
    if (duration > 0 && new_pos > duration) new_pos = duration;
    audio_player_seek(new_pos);
    save_current_position();   // R032-104: seek 后立即保存断点并 flush，避免断电丢失
    ESP_LOGI(TAG, "Skip %ds → pos=%d", seconds, new_pos);
}

/* ============================================================
 * 曲目播完回调
 * ============================================================ */
static void on_track_finished(int state, void *user_data)
{
    ESP_LOGI(TAG, "Track finished naturally");

    // 异步：仅记下需要保存的位置和下一曲，主循环中执行
    g_pending_save_track = g_current_track;
    g_pending_save_position = 0;

    // 根据播放模式决定下一首（仅记下目标，主循环中执行跳转）
    switch (g_play_mode) {
    case PLAY_MODE_SEQUENCE:
        if (g_current_track < playlist_count() - 1) {
            g_pending_track_next = playlist_next();
            g_pending_track_seek = 0;
            g_pending_track_finished = true;
        } else {
            g_app_state = APP_STATE_STOPPED;
            ESP_LOGI(TAG, "Playlist finished (sequence mode)");
        }
        break;
    case PLAY_MODE_REPEAT_ALL:
        g_pending_track_next = playlist_next();
        g_pending_track_seek = 0;
        g_pending_track_finished = true;
        break;
    case PLAY_MODE_REPEAT_ONE:
        g_pending_track_next = g_current_track;
        g_pending_track_seek = 0;
        g_pending_track_finished = true;
        break;
    }
}

/* ============================================================
 * 统一菜单宿主回调 (R049) — 由 menu.cpp 调用
 * ============================================================ */
void app_menu_exit(void)
{
    menu_close();
    g_app_state = g_state_before_menu;
}

void app_enter_browse(void)
{
    if (playlist_count() == 0) {
        menu_close();
        g_app_state = g_state_before_menu;
        return;
    }
    g_state_before_browse = g_state_before_menu;
    menu_close();
    g_app_state = APP_STATE_BROWSING;
    g_browse_index = g_current_track;
    ESP_LOGI(TAG, "Enter browse via menu");
}

int app_get_play_mode(void)
{
    return (int)g_play_mode;
}

void app_set_play_mode(int m)
{
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    g_play_mode = (play_mode_t)m;
    settings_save_play_mode(m);
    display_set_play_mode(m);
}

/* R049c：信息屏（桩功能提示） */
void app_show_info(const char *title, const char *text)
{
    strncpy(g_info_title, title ? title : "", sizeof(g_info_title) - 1);
    g_info_title[sizeof(g_info_title) - 1] = '\0';
    strncpy(g_info_text, text ? text : "", sizeof(g_info_text) - 1);
    g_info_text[sizeof(g_info_text) - 1] = '\0';
    g_info_until_us = esp_timer_get_time() + 1500000;  // 显示 1.5s
    g_info_active = true;
    ESP_LOGI(TAG, "Info: %s", title);
}

/* R049c：按键提示音（设置开启且非播放态时在菜单/浏览/停止态播放） */
void app_play_beep(void)
{
    if (settings_load_key_beep()) {
        audio_player_play_beep();
    }
}

/* R049c：进入/退出 TF 卡固件升级向导（由 menu.cpp 的 app_ota_enter 调用） */
void app_enter_ota(void)
{
    // 升级前确保停止播放，独占 SD 卡
    save_current_position();
    audio_player_stop();
    g_ota_in_progress = true;
    menu_close();
    g_app_state = APP_STATE_OTA;
    ota_sd_begin();
}

void app_ota_exit(void)
{
    g_ota_in_progress = false;
    g_app_state = g_state_before_menu;
}

/* ============================================================
 * 处理按键事件
 * ============================================================ */
static void handle_button_events(void)
{
    btn_event_info_t events[8];
    int n = button_manager_scan(events, sizeof(events) / sizeof(events[0]));

    /* 统一菜单路由 (R049): 菜单打开时所有按键交给菜单处理 */
    if (menu_is_open()) {
        menu_handle_button(events, n);
        return;
    }

    /* R049c OTA 升级向导路由：独立于菜单，确认/锁死/任意键重启 */
    if (g_app_state == APP_STATE_OTA) {
        ota_sd_handle_button(events, n);
        return;
    }

    /* 统一菜单入口: 长按 STOP 在播放相关态打开菜单 (取代原"长按 STOP 进浏览") */
    for (int j = 0; j < n; j++) {
        if (events[j].id == BTN_ID_STOP &&
            events[j].event == BTN_EVENT_LONG_PRESS &&
            (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
             g_app_state == APP_STATE_STOPPED)) {
            g_state_before_menu = g_app_state;
            menu_open();
            g_app_state = APP_STATE_MENU;
            return;
        }
    }

    /* 组合键 REW+STOP 跳曲首：先检测本帧是否两键同时短按（排除浏览/空闲态） */
    uint64_t now = esp_timer_get_time();
    bool in_play_ctx = (g_app_state != APP_STATE_BROWSING && g_app_state != APP_STATE_IDLE);
    bool frame_rew = false, frame_stop = false;
    if (in_play_ctx) {
        for (int j = 0; j < n; j++) {
            if (events[j].event == BTN_EVENT_SHORT_PRESS) {
                if (events[j].id == BTN_ID_REWIND) frame_rew = true;
                if (events[j].id == BTN_ID_STOP)    frame_stop = true;
            }
        }
    }
    bool combo_done = false;
    if (frame_rew && frame_stop) {
        jump_to_track_start();
        combo_done = true;
        g_combo_rew_us = 0;
        g_combo_stop_us = 0;
    }

    for (int i = 0; i < n; i++) {
        btn_event_info_t *e = &events[i];

        /* 记录用户活动（先于锁定检查，避免锁定状态误触发休眠） */
        if (e->event != BTN_EVENT_NONE) {
            power_mgmt_record_activity();
        }

        /* 浏览模式：上一首/下一首 短按上/下一曲；长按/持续按住则加速连续移动；
           播放 选择，停止 退出，停止长按 给选中曲加书签 */
        if (g_app_state == APP_STATE_BROWSING) {
            int total = playlist_count();
            switch (e->id) {
            case BTN_ID_PREV:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_browse_index = (g_browse_index - 1 + total) % total;
                    g_browse_repeat_ms = 0;
                } else if (e->event == BTN_EVENT_LONG_PRESS) {
                    g_browse_repeat_ms = e->hold_ms;   // 锚定起点，避免长按瞬间重复跳
                } else if (e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    uint32_t step = browse_repeat_interval(e->hold_ms);
                    if (e->hold_ms - g_browse_repeat_ms >= step) {
                        g_browse_repeat_ms = e->hold_ms;
                        g_browse_index = (g_browse_index - 1 + total) % total;
                    }
                } else if (e->event == BTN_EVENT_RELEASE) {
                    g_browse_repeat_ms = 0;
                }
                break;
            case BTN_ID_NEXT:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_browse_index = (g_browse_index + 1) % total;
                    g_browse_repeat_ms = 0;
                } else if (e->event == BTN_EVENT_LONG_PRESS) {
                    g_browse_repeat_ms = e->hold_ms;
                } else if (e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    uint32_t step = browse_repeat_interval(e->hold_ms);
                    if (e->hold_ms - g_browse_repeat_ms >= step) {
                        g_browse_repeat_ms = e->hold_ms;
                        g_browse_index = (g_browse_index + 1) % total;
                    }
                } else if (e->event == BTN_EVENT_RELEASE) {
                    g_browse_repeat_ms = 0;
                }
                break;
            case BTN_ID_PLAY_PAUSE:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_current_track = g_browse_index;
                    playlist_set_index(g_current_track);
                    g_seek_on_play_position = 0;
                    g_app_state = g_state_before_browse;
                    play_current_track();
                }
                break;
            case BTN_ID_REWIND:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    int first = 0;
                    g_browse_index = (g_browse_index - BROWSE_PAGE_STEP < first)
                                      ? first : g_browse_index - BROWSE_PAGE_STEP;
                } else if (e->event == BTN_EVENT_LONG_PRESS ||
                           e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    g_browse_index = 0;          // 跳到列表头
                }
                g_browse_repeat_ms = 0;
                break;
            case BTN_ID_FAST_FORWARD: {
                int last = (total > 0) ? total - 1 : 0;
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_browse_index = (g_browse_index + BROWSE_PAGE_STEP > last)
                                      ? last : g_browse_index + BROWSE_PAGE_STEP;
                } else if (e->event == BTN_EVENT_LONG_PRESS ||
                           e->event == BTN_EVENT_HOLD ||
                           e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                    g_browse_index = last;        // 跳到列表尾
                }
                g_browse_repeat_ms = 0;
                break;
            }
            case BTN_ID_STOP:
                if (e->event == BTN_EVENT_SHORT_PRESS) {
                    g_app_state = g_state_before_browse;
                } else if (e->event == BTN_EVENT_LONG_PRESS) {
                    int bm = bookmark_add(g_browse_index, 0);
                    if (bm >= 0) ESP_LOGI(TAG, "Bookmark added at track %d (slot %d)", g_browse_index, bm);
                    else        ESP_LOGW(TAG, "Bookmark add failed at track %d", g_browse_index);
                }
                break;
            default:
                break;
            }
            continue;
        }

        switch (e->id) {

        /* --- 播放/暂停 --- */
        case BTN_ID_PLAY_PAUSE:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 按键提示音
                if (g_app_state == APP_STATE_STOPPED || g_app_state == APP_STATE_IDLE) {
                    g_current_track = playlist_current_index();
                    // 不再清零：沿用 stop_playback/init_storage 缓存的位置（0 = 从头）
                    play_current_track();
                } else if (g_app_state == APP_STATE_PLAYING) {
                    audio_player_pause();
                    g_app_state = APP_STATE_PAUSED;
                } else if (g_app_state == APP_STATE_PAUSED) {
                    audio_player_resume();
                    g_app_state = APP_STATE_PLAYING;
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                /* 长按：切换播放模式（顺序 → 列表循环 → 单曲循环） */
                cycle_play_mode();
            }
            break;

        /* --- 停止 --- */
        case BTN_ID_STOP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 按键提示音
                if (combo_done) {
                    // 本帧已作为组合键处理（两键同按），跳过单独逻辑，避免重复 stop/skip
                } else if (g_combo_rew_us && (now - g_combo_rew_us) < COMBO_WINDOW_US) {
                    // 与稍早的 REW 短按构成组合键 → 跳曲首
                    jump_to_track_start();
                    g_combo_rew_us = 0;
                    g_combo_stop_us = 0;
                } else {
                    g_combo_stop_us = now;   // 记录单独 STOP，等待可能的 REW 组合
                    stop_playback();
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                /* 长按：进入浏览界面 */
                if (playlist_count() > 0) {
                    g_state_before_browse = g_app_state;
                    g_browse_index = g_current_track;
                    g_app_state = APP_STATE_BROWSING;
                    ESP_LOGI(TAG, "Enter browse mode, selected track %d", g_browse_index);
                }
            }
            break;

        /* --- 上一首 --- */
        case BTN_ID_PREV:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 按键提示音
                if (g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND)
                    break;   // 按住快进/快退期间忽略切歌（磁带机互锁）
                save_current_position();  // 切换前保存旧位置
                int prev = playlist_prev();  // R032-107: 空列表返回 -1，避免污染 g_current_track
                if (prev >= 0) {
                    g_current_track = prev;
                    playlist_set_index(prev);
                    g_seek_on_play_position = 0;  // 新曲目从头
                    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                        play_current_track();
                    }
                }
            }
            /* R042: 长按/持续按住音量调节已迁出至专用 VOL± 键 (GPIO0/GPIO3) */
            break;

        /* --- 下一首 --- */
        case BTN_ID_NEXT:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                app_play_beep();  // R049c 按键提示音
                if (g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND)
                    break;   // 按住快进/快退期间忽略切歌（磁带机互锁）
                save_current_position();
                int next = playlist_next();  // R032-107: 空列表返回 -1，避免污染 g_current_track
                if (next >= 0) {
                    g_current_track = next;
                    playlist_set_index(next);
                    g_seek_on_play_position = 0;
                    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                        play_current_track();
                    }
                }
            }
            /* R042: 长按/持续按住音量调节已迁出至专用 VOL± 键 (GPIO0/GPIO3) */
            break;

        /* --- 音量减 (LCK 左拨, GPIO0) --- */
        case BTN_ID_VOL_DOWN:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                g_vol_down_counter = 0;
                int vol = audio_player_get_volume();
                if (vol > 0) {
                    audio_player_set_volume(vol - 1);
                    display_show_volume(audio_player_get_volume());
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS ||
                       e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                /* 拨轮自复位场景下 LONG_PRESS/HOLD 极少触发; 兜底保留连续减 */
                g_vol_down_counter++;
                if (g_vol_down_counter % 5 == 0) {
                    int vol = audio_player_get_volume();
                    if (vol > 0) {
                        audio_player_set_volume(vol - 1);
                        display_show_volume(audio_player_get_volume());
                    }
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                g_vol_down_counter = 0;
                settings_save_volume(audio_player_get_volume());  // 松开时保存音量
            }
            break;

        /* --- 音量加 (LCK 右拨, GPIO3) --- */
        case BTN_ID_VOL_UP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                g_vol_up_counter = 0;
                int vol = audio_player_get_volume();
                if (vol < VOLUME_LEVEL_MAX) {
                    audio_player_set_volume(vol + 1);
                    display_show_volume(audio_player_get_volume());
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS ||
                       e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                g_vol_up_counter++;
                if (g_vol_up_counter % 5 == 0) {
                    int vol = audio_player_get_volume();
                    if (vol < VOLUME_LEVEL_MAX) {
                        audio_player_set_volume(vol + 1);
                        display_show_volume(audio_player_get_volume());
                    }
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                g_vol_up_counter = 0;
                settings_save_volume(audio_player_get_volume());
            }
            break;

        /* --- 快进 --- */
        case BTN_ID_FAST_FORWARD:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                skip_seconds(5);            // R045：短按跳 5 秒
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                // 进入变速态：仅在长按首次触发一次（避免与 HOLD 重复调用 press）
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    if (g_app_state == APP_STATE_PAUSED) {
                        audio_player_resume();
                    }
                    skip_seconds(5);            // R046：先继承短按基准跳进 5 秒，避免"刚过长按反而倒退更少"的断层
                    tape_control_ff_press();
                    audio_player_set_speed(tape_control_get_speed());
                    g_app_state = APP_STATE_FAST_FORWARD;
                    g_combo_rew_us = 0; g_combo_stop_us = 0;  // 进入变速态，放弃未完成的组合键计时
                }
            } else if (e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                // 保持态：变速档位由 tape_control_tick() 按按住时长自动升档，
                // 此处仅确保速度同步（press 已在 LONG_PRESS 调过，不重复调用）。
                if (g_app_state == APP_STATE_FAST_FORWARD) {
                    audio_player_set_speed(tape_control_get_speed());
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                tape_control_ff_release();
                audio_player_set_speed(TAPE_SPEED_NORMAL);
                g_app_state = APP_STATE_PLAYING;
                g_combo_rew_us = 0; g_combo_stop_us = 0;  // 退出变速态，清空组合键计时
            }
            break;

        /* --- 快退 --- */
        case BTN_ID_REWIND:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                if (combo_done) {
                    // 本帧已作为组合键处理（两键同按），跳过单独逻辑，避免重复 stop/skip
                } else if (g_combo_stop_us && (now - g_combo_stop_us) < COMBO_WINDOW_US) {
                    // 与稍早的 STOP 短按构成组合键 → 跳曲首
                    jump_to_track_start();
                    g_combo_rew_us = 0;
                    g_combo_stop_us = 0;
                } else {
                    g_combo_rew_us = now;    // 记录单独 REW，等待可能的 STOP 组合
                    skip_seconds(-5);        // R045：短按后退 5 秒
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                // 进入变速态：仅在长按首次触发一次（避免与 HOLD 重复调用 press）
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    if (g_app_state == APP_STATE_PAUSED) {
                        audio_player_resume();
                    }
                    skip_seconds(-5);           // R046：先继承短按基准后退 5 秒，避免"刚过长按反而倒退更少"的断层
                    tape_control_rewind_press();
                    audio_player_set_speed(tape_control_get_speed());
                    g_app_state = APP_STATE_REWIND;
                    g_combo_rew_us = 0; g_combo_stop_us = 0;  // 进入变速态，放弃未完成的组合键计时
                }
            } else if (e->event == BTN_EVENT_HOLD ||
                       e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                // 保持态：变速档位由 tape_control_tick() 自动升档，press 已调过，不重复
                if (g_app_state == APP_STATE_REWIND) {
                    audio_player_set_speed(tape_control_get_speed());
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                tape_control_rewind_release();
                audio_player_set_speed(TAPE_SPEED_NORMAL);
                g_app_state = APP_STATE_PLAYING;
                g_combo_rew_us = 0; g_combo_stop_us = 0;  // 退出变速态，清空组合键计时
            }
            break;

        default:
            break;
        }
    }
}

/* ============================================================
 * 更新显示屏
 * ============================================================ */
static void update_display(void)
{
    // R049c：信息屏覆盖优先（OTA/USB/关于等桩提示），不受 200ms 节流限制
    if (g_info_active) {
        uint64_t t = esp_timer_get_time();
        if (t < g_info_until_us) {
            display_show_info(g_info_title, g_info_text);
            return;
        }
        g_info_active = false;
    }

    /* R049c OTA 升级界面：优先渲染，不受 200ms 节流限制 */
    if (g_app_state == APP_STATE_OTA) {
        ota_sd_render();
        return;
    }

    uint64_t now = esp_timer_get_time();
    if ((now - g_last_display_update) < 200000) return;
    g_last_display_update = now;

    if (g_app_state == APP_STATE_MENU) {
        return;  // 菜单由自身渲染 (menu_render → display_show_menu)
    }

    if (g_app_state == APP_STATE_BROWSING) {
        int total = playlist_count();
        int scroll = g_browse_index - (BROWSE_VISIBLE_LINES / 2);
        if (scroll < 0) scroll = 0;
        int max_scroll = total - BROWSE_VISIBLE_LINES;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll > max_scroll) scroll = max_scroll;

        char lines[BROWSE_VISIBLE_LINES][24];
        int count = total - scroll;
        if (count > BROWSE_VISIBLE_LINES) count = BROWSE_VISIBLE_LINES;

        for (int i = 0; i < count; i++) {
            int idx = scroll + i;
            char name[FILENAME_MAX_LEN];
            playlist_get_name(idx, name, sizeof(name));
            snprintf(lines[i], sizeof(lines[i]), "%s%.21s",
                     (idx == g_browse_index) ? ">" : " ", name);
        }
        display_show_browse(g_browse_index, total, lines, count);
        return;
    }

    player_state_t disp_state;
    switch (g_app_state) {
    case APP_STATE_PLAYING:      disp_state = PLAYER_STATE_PLAYING;  break;
    case APP_STATE_FAST_FORWARD: disp_state = PLAYER_STATE_FAST_FORWARD; break;
    case APP_STATE_REWIND:       disp_state = PLAYER_STATE_REWIND;   break;
    case APP_STATE_PAUSED:       disp_state = PLAYER_STATE_PAUSED;   break;
    case APP_STATE_STOPPED:
    case APP_STATE_IDLE:
    default:                     disp_state = PLAYER_STATE_STOPPED;  break;
    }

    char track_name[FILENAME_MAX_LEN] = "";
    if (!playlist_get_name(g_current_track, track_name, sizeof(track_name))) {
        snprintf(track_name, sizeof(track_name), "Track %d", g_current_track + 1);
    }

    int position = audio_player_get_position();
    int duration = audio_player_get_duration();
    float speed  = tape_control_get_speed();
    int gear     = tape_control_get_gear();
    int volume   = audio_player_get_volume();
    int total    = playlist_count();

    display_update(disp_state, track_name,
                   g_current_track + 1, total,
                   position, duration,
                   speed, gear, volume);
}

/* ============================================================
 * 挂载 SD 卡
 * ============================================================ */
static bool mount_sd_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = true,  // R028/M1: 单 FAT 节省内存（嵌入式单用户）
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;  // 显式确认（SDSPI_HOST_DEFAULT 已设但保留显式）

    sdspi_device_config_t device_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    device_cfg.host_id = SD_SPI_HOST;
    device_cfg.gpio_cs = SD_CS_IO;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host,
                                              &device_cfg, &mount_config,
                                              &g_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (0x%x)", ret);
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted: %s", g_sd_card->cid.name);
    return true;
}

/* ============================================================
 * 唤醒 GPIO 辅助 (ESP32-S3 仅 GPIO0~21 为 RTC GPIO, 可作 ext1 唤醒源)
 * 新版按键映射中 NEXT(IO47)/REW(IO42)/FF(IO41) 非 RTC GPIO,
 * 不能用于 light/deep sleep 唤醒, 须从唤醒掩码中排除。
 * ============================================================ */
static bool is_rtc_wakeup_gpio(gpio_num_t g)
{
    return (g >= GPIO_NUM_0 && g <= GPIO_NUM_21);
}

static uint64_t build_rtc_wakeup_mask(void)
{
    uint64_t mask = 0;
    const gpio_num_t btns[] = {
        BTN_PLAY_PAUSE, BTN_STOP, BTN_PREV, BTN_NEXT, BTN_REWIND, BTN_FAST_FORWARD
    };
    for (int i = 0; i < 6; i++) {
        if (is_rtc_wakeup_gpio(btns[i])) {
            mask |= (1ULL << btns[i]);
        }
    }
    return mask;
}

/* ============================================================
 * 初始化外设
 * ============================================================ */
/* ============================================================
 * WS2812 状态指示灯 (IO48)
 * 采用 ESP-IDF led_strip 组件（RMT 驱动单颗 WS2812）。
 * WS2812 数据线经硬件电平转换；MCU 侧以 GPIO 推挽输出驱动。
 * 颜色语义：蓝=充电中 / 红=电量极低 / 橙=电量低 / 绿=播放中 / 灭=空闲。
 * ============================================================ */
static led_strip_handle_t s_ws2812 = NULL;

static void indicator_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ws2812) return;
    esp_err_t r1 = led_strip_set_pixel(s_ws2812, 0, r, g, b);
    esp_err_t r2 = led_strip_refresh(s_ws2812);
    if (r1 != ESP_OK || r2 != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 set failed: %s / %s", esp_err_to_name(r1), esp_err_to_name(r2));
    }
}

static void indicator_led_init(void)
{
    if (s_ws2812) return;
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_IO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .flags = { .with_dma = false },
    };
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_ws2812);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init failed: %s", esp_err_to_name(ret));
        return;
    }
    indicator_led_set(0, 0, 0); // 上电默认熄灭
}

static void init_hardware(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  TapeBook - Tape-Style Audiobook Player");
    ESP_LOGI(TAG, "  ESP32-S3-WROOM-1 (Octal PSRAM)");
    ESP_LOGI(TAG, "========================================");

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. Settings（打开 NVS handle）
    settings_init();

    // 3. 显示屏
    display_init();
    display_show_splash();

    // 4. 按键
    button_manager_init();
    menu_init();

    // 5. 磁带控制器
    tape_control_init();

    // 5.5 MAX98357 SD_MODE (GPIO4 = I2S_SD → Pin4)：采样率模式选择脚，须由 MCU 拉到固定电平，
    //     否则悬空会导致采样率模式不确定、无声。
    {
        gpio_config_t sd_mode_cfg = {
            .pin_bit_mask = (1ULL << MAX98357_SD_MODE_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&sd_mode_cfg));
        gpio_set_level(MAX98357_SD_MODE_GPIO, MAX98357_SD_MODE_LEVEL);
    }

    // 5.6 SD 卡在位检测 (IO38): 输入, 外部 10K 上拉(R6)到 3.3V, active-low(插入=低)
    {
        gpio_config_t cd_cfg = {
            .pin_bit_mask = (1ULL << SD_CD_IO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cd_cfg));
    }

    // 6. 音频播放器
    audio_player_init();
    audio_player_set_callback(on_track_finished, NULL);

    // 7. 电源管理
    power_mgmt_init();

    // 7.5 状态指示灯 (WS2812, IO48)
    indicator_led_init();

    // 8. 书签模块
    bookmark_init();

    // 9. 看门狗初始化（10 秒超时，给 callback 内 pipeline 操作留余量）
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t wdt_err = esp_task_wdt_init(&twdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_reconfigure(&twdt_config);
    } else {
        ESP_ERROR_CHECK(wdt_err);
    }
    esp_task_wdt_add(NULL);  // 订阅当前任务

    // 10. 加载持久化设置
    int vol = settings_load_volume();
    audio_player_set_volume(vol);
    g_play_mode = (play_mode_t)settings_load_play_mode();
    display_set_play_mode((int)g_play_mode);

    // 11. 校验所有按键 GPIO 是否为合法 RTC 唤醒源 (新版映射部分按键非 RTC GPIO)
    const gpio_num_t wakeup_gpios[] = {
        BTN_PLAY_PAUSE, BTN_STOP, BTN_PREV, BTN_NEXT, BTN_REWIND, BTN_FAST_FORWARD
    };
    for (size_t i = 0; i < sizeof(wakeup_gpios) / sizeof(wakeup_gpios[0]); i++) {
        if (!esp_sleep_is_valid_wakeup_gpio(wakeup_gpios[i])) {
            ESP_LOGW(TAG, "GPIO %d is NOT a valid RTC wakeup source; "
                          "excluded from sleep wakeup mask", wakeup_gpios[i]);
        }
    }
}

/* ============================================================
 * 初始化存储：挂载 SD → 扫描 → 恢复断点
 * ============================================================ */
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
        // 尝试从 NVS 恢复断点
        int saved_idx = 0, saved_pos = 0;
        if (settings_load_position(&saved_idx, &saved_pos)) {
            g_current_track = saved_idx;
            playlist_set_index(g_current_track);
            g_seek_on_play_position = saved_pos;
            g_app_state = APP_STATE_STOPPED;  // 等待用户按播放
            ESP_LOGI(TAG, "Resuming from track %d at %ds (press Play to start)", saved_idx, saved_pos);
        } else {
            g_current_track = 0;
            playlist_set_index(0);
            g_app_state = APP_STATE_STOPPED;
        }
    }
}

/* ============================================================
 * 主任务
 * ============================================================ */
extern "C" void app_main(void)
{
    init_hardware();
    init_storage();

    /* OTA 启动回滚确认：若从 OTA 分区启动且运行健康，标记有效（防 boot loop 假死砖） */
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running && (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
                        running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Marked OTA app valid (rollback cancelled)");
        }
    }

    // 初始化 SD 卡在位状态 (避免启动误报插拔提示)
    {
        int boot_lvl = gpio_get_level(SD_CD_IO);
        g_sd_cd_raw = boot_lvl;
        g_sd_cd_stable_cnt = 3;   // 视为已稳定, 不触发插入/弹出事件
        g_sd_inserted = (boot_lvl == SD_CD_ACTIVE_LEVEL);
        display_set_sd_present_init(g_sd_inserted);
    }

    ESP_LOGI(TAG, "System ready. Waiting for user input...");

    g_next_loop_deadline = esp_timer_get_time();

    while (1) {
        // 1. 处理按键事件
        handle_button_events();

        // 2. 磁带控制器 tick
        tape_control_tick();

        // 3. 快进/快退速度更新
        tape_mode_t mode = tape_control_get_mode();
        if (mode != TAPE_MODE_NORMAL) {
            audio_player_set_speed(tape_control_get_speed());
        }

        // 4. 音频播放器 tick（管道维护/跳帧/事件监听）
        audio_player_tick();

        // 5. 异步处理曲目播完（避免在回调内嵌套 pipeline 操作）
        if (g_pending_track_finished) {
            g_pending_track_finished = false;
            g_current_track = g_pending_track_next;
            playlist_set_index(g_current_track);
            g_seek_on_play_position = g_pending_track_seek;
            play_current_track();
        }

        // 5b. 异步保存持久化状态（回调中仅暂存，settings_flush() 负责落盘）
        if (g_pending_save_track >= 0) {
            char name[FILENAME_MAX_LEN] = "";
            playlist_get_name(g_pending_save_track, name, sizeof(name));
            settings_save_position(g_pending_save_track, g_pending_save_position, name);
            settings_flush();   // R032-104: 播完后立即落盘，避免 30s 自动保存窗口内断电丢断点
            g_pending_save_track = -1;
        }

        // 6. 每 30 秒自动保存断点 + 批量 flush NVS（播放/暂停/FF/RW 均保存，R034-002 / R035-004）
        {
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_auto_save_us) >= AUTO_SAVE_INTERVAL_US) {
                g_last_auto_save_us = now;
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
                    g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
                    save_current_position();
                    settings_flush();
                    // R035-010：自动保存也算用户活动，重置 auto-off 计时
                    power_mgmt_record_activity();
                }
            }
        }

        // 7. 定时关机检查（含 FF/RW）
        if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED ||
            g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
            if (power_mgmt_auto_off_expired()) {
                ESP_LOGI(TAG, "Auto-off timer expired, stopping playback");
                audio_player_stop();
                g_app_state = APP_STATE_STOPPED;
                power_mgmt_set_auto_off(0);
                // R034-007：触发后落盘清零 NVS，避免重启 power_mgmt_init 重新武装
                settings_save_auto_off(0);
                settings_flush();
            }
        }

        // 7b. 电源管理 tick（1Hz 周期性任务）
        {
            static uint64_t last_power_tick = 0;
            uint64_t now = esp_timer_get_time();
            if ((now - last_power_tick) >= 1000000) {
                last_power_tick = now;
                power_mgmt_tick();

                // WS2812 状态指示灯颜色更新
                if (power_mgmt_is_charging()) {
                    indicator_led_set(0, 0, 255);            // 蓝：充电中
                } else {
                    bat_state_t st = power_mgmt_get_state();
                    if (st == BAT_STATE_CRITICAL) {
                        indicator_led_set(255, 0, 0);        // 红：电量极低
                    } else if (st == BAT_STATE_LOW) {
                        indicator_led_set(255, 80, 0);      // 橙：电量低
                    } else if (g_app_state == APP_STATE_PLAYING ||
                               g_app_state == APP_STATE_PAUSED ||
                               g_app_state == APP_STATE_FAST_FORWARD ||
                               g_app_state == APP_STATE_REWIND) {
                        indicator_led_set(0, 255, 0);        // 绿：播放中
                    } else {
                        indicator_led_set(0, 0, 0);          // 灭：空闲
                    }
                }

                // 电量极低时保存状态并软关机 (脉冲 POW_EN 硬断电)
                if (power_mgmt_should_shutdown()) {
                    ESP_LOGE(TAG, "Battery critical, saving state and powering off");
                    save_current_position();
                    settings_flush();
                    audio_player_stop();
                    // 仅 RTC GPIO 可作唤醒源; 设置掩码供 power_off 的 deep-sleep 兜底
                    uint64_t wakeup_mask = build_rtc_wakeup_mask();
                    if (wakeup_mask) {
                        esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
                    }
                    power_mgmt_power_off();   // 脉冲 POW_EN 释放电源锁存 (含 deep-sleep 兜底)
                }
            }
        }

        // 7c. 自动休眠（5 分钟无操作进入 light sleep，按键 GPIO 唤醒）
        // S2：播放中（PLAYING/PAUSED）不休眠，否则听书会被打断
        // S3：sleep 前释放磁带状态机，避免唤醒后档位残留
        if ((g_app_state == APP_STATE_STOPPED || g_app_state == APP_STATE_IDLE) &&
            power_mgmt_should_sleep()) {
            ESP_LOGI(TAG, "Idle timeout, entering light sleep");

            save_current_position();   // R032-103: sleep 前保存断点（FF/RW 不会进入此分支，已在此前释放）
            settings_flush();
            audio_player_stop();
            g_app_state = APP_STATE_IDLE;

            {
                uint64_t wakeup_mask = build_rtc_wakeup_mask();
                if (wakeup_mask) {
                    esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
                }
            }
            esp_light_sleep_start();

            ESP_LOGI(TAG, "Woke from light sleep");
            g_next_loop_deadline = esp_timer_get_time();

            // 唤醒后恢复为 STOPPED 状态（保持曲目选中，用户按 Play 继续）
            g_app_state = APP_STATE_STOPPED;

            // L4: 唤醒后恢复断点位置（仅当 saved_track == g_current_track）
            {
                int saved_idx = 0, saved_pos = 0;
                if (settings_load_position(&saved_idx, &saved_pos) &&
                    saved_idx == g_current_track) {
                    g_seek_on_play_position = saved_pos;
                    ESP_LOGI(TAG, "Wakeup resume: track %d at %ds", saved_idx, saved_pos);
                }
            }

            power_mgmt_record_activity();
        }

        // 8. SD 卡插拔管理 (基于 SD_CD 机械开关实时检测, 软件去抖)
        //    OTA 写入期间独占 SD 卡：跳过插拔/读校验处理，避免与升级写竞争
        if (!g_ota_in_progress) {
            int lvl = gpio_get_level(SD_CD_IO);
            // 去抖: 同一电平需连续稳定若干次(≈60ms)才提交
            if (lvl != g_sd_cd_raw) {
                g_sd_cd_raw = lvl;
                g_sd_cd_stable_cnt = 0;
            } else if (g_sd_cd_stable_cnt < 255) {
                g_sd_cd_stable_cnt++;
            }

            if (g_sd_cd_stable_cnt >= 3) {
                bool present = (lvl == SD_CD_ACTIVE_LEVEL);
                if (present && (g_sd_card == NULL)) {
                    // 插入事件: 挂载 + 扫描
                    ESP_LOGI(TAG, "SD card inserted");
                    if (mount_sd_card()) {
                        int count = playlist_scan(SD_MOUNT_POINT);
                        ESP_LOGI(TAG, "Found %d audio files on SD card", count);
                        if (count > 0) {
                            g_current_track = 0;
                            playlist_set_index(0);
                            g_app_state = APP_STATE_STOPPED;
                        } else {
                            display_show_no_files();
                            g_app_state = APP_STATE_IDLE;
                        }
                    } else {
                        display_show_no_card();
                        g_app_state = APP_STATE_IDLE;
                    }
                    display_set_sd_present(true);
                } else if (!present && g_sd_inserted) {
                    // 弹出事件: 停止 + 卸载
                    ESP_LOGI(TAG, "SD card removed");
                    if (g_sd_card != NULL) {
                        audio_player_stop();
                        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_sd_card);
                        g_sd_card = NULL;
                    }
                    display_show_no_card();
                    g_app_state = APP_STATE_IDLE;
                    display_set_sd_present(false);
                }
                g_sd_inserted = present;
            }

            // 后备: 已挂载时每 5 秒读扇区 0 探测异常拔出 (脏拔/读错误)
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_sd_check_us) >= SD_CHECK_INTERVAL_US) {
                g_last_sd_check_us = now;
                if (g_sd_card != NULL) {
                    uint32_t buf;
                    esp_err_t ret = sdmmc_read_sectors(g_sd_card, (uint8_t *)&buf, 0, 1);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "SD card removed (read fail)!");
                        audio_player_stop();
                        display_show_no_card();
                        g_app_state = APP_STATE_IDLE;
                        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_sd_card);
                        g_sd_card = NULL;
                        // 不改动 g_sd_inserted: 由 SD_CD 去抖逻辑决定后续(重新挂载或置灰)
                    }
                }
            }
        }

        // 9. 看门狗复位
        esp_task_wdt_reset();

        // 10. 更新显示屏
        update_display();

        // 11. 休眠，控制循环频率（基于绝对时间对齐，补偿前序耗时）
        {
            int64_t now = esp_timer_get_time();
            if (now < g_next_loop_deadline) {
                vTaskDelay(pdMS_TO_TICKS((g_next_loop_deadline - now) / 1000));
            }
            g_next_loop_deadline += BTN_SCAN_INTERVAL * 1000;
        }
    }
}
