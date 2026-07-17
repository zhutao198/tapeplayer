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
#include "voice_prompt.h"
#include "bookmark.h"

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
    APP_STATE_LOCKED,          // 按键锁定
    APP_STATE_BROWSING,        // 文件夹浏览
} app_state_t;

// 所有全局变量单任务访问，无需 volatile（M-9/L-8: 设计确认 OK）
static app_state_t    g_app_state = APP_STATE_IDLE;
static int            g_current_track = 0;
static bool           g_key_locked = false;
static app_state_t    g_state_before_lock = APP_STATE_STOPPED;
static uint64_t       g_last_display_update = 0;
static int64_t        g_next_loop_deadline = 0;
static int            g_vol_hold_counter = 0;  // 音量长按计数器（每 5 步=100ms 调 1 级）
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

static sdmmc_card_t   *g_sd_card = NULL;  // SD 卡句柄
static uint64_t    g_last_sd_check_us = 0;

#define AUTO_SAVE_INTERVAL_US  (30 * 1000000)  // 30 秒自动保存
#define SD_CHECK_INTERVAL_US   (5 * 1000000)   // 5 秒检查 SD 卡状态

/* ============================================================
 * 辅助：保存当前断点
 * ============================================================ */
static void save_current_position(void)
{
    if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
        char name[FILENAME_MAX_LEN] = "";
        playlist_get_name(g_current_track, name, sizeof(name));
        settings_save_position(g_current_track, audio_player_get_position(), name);
    }
}

/* ============================================================
 * 辅助：停止/播放/跳转
 * ============================================================ */
static void stop_playback(void)
{
    save_current_position();
    audio_player_stop();
    // 统一退出磁带模式（不限 FF/RW）
    if (g_app_state == APP_STATE_FAST_FORWARD) {
        tape_control_ff_release();
    } else if (g_app_state == APP_STATE_REWIND) {
        tape_control_rewind_release();
    }
    g_app_state = APP_STATE_STOPPED;
}

static void play_current_track(void)
{
    char filepath[FILENAME_MAX_LEN * 2];
    if (playlist_get_path(g_current_track, filepath, sizeof(filepath))) {
        if (audio_player_play(filepath)) {
            // 如果有断点位置（从 NVS 恢复或切换曲目时指定）
            if (g_seek_on_play_position > 0) {
                audio_player_seek(g_seek_on_play_position);
                g_seek_on_play_position = 0;
            }
            g_app_state = APP_STATE_PLAYING;
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
 * 处理按键事件
 * ============================================================ */
static void handle_button_events(void)
{
    btn_event_info_t events[8];
    int n = button_manager_scan(events, sizeof(events) / sizeof(events[0]));

    for (int i = 0; i < n; i++) {
        btn_event_info_t *e = &events[i];

        /* 记录用户活动（先于锁定检查，避免锁定状态误触发休眠） */
        if (e->event != BTN_EVENT_NONE) {
            power_mgmt_record_activity();
        }

        /* 按键锁定模式下，仅响应解锁操作 */
        if (g_key_locked) {
            if (e->id == BTN_ID_PLAY_PAUSE && e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                g_key_locked = false;
                g_app_state = g_state_before_lock;
                ESP_LOGI(TAG, "Key lock released, state restored to %d", g_app_state);
            }
            continue;
        }

        /* 浏览模式：Prev/Next 滚动，Play 选择，STOP 退出 */
        if (g_app_state == APP_STATE_BROWSING) {
            if (e->event != BTN_EVENT_SHORT_PRESS) continue;
            int total = playlist_count();
            switch (e->id) {
            case BTN_ID_PREV:
                g_browse_index = (g_browse_index - 1 + total) % total;
                break;
            case BTN_ID_NEXT:
                g_browse_index = (g_browse_index + 1) % total;
                break;
            case BTN_ID_PLAY_PAUSE:
                g_current_track = g_browse_index;
                playlist_set_index(g_current_track);
                g_seek_on_play_position = 0;
                g_app_state = g_state_before_browse;
                play_current_track();
                break;
            case BTN_ID_STOP:
                g_app_state = g_state_before_browse;
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
                if (g_app_state == APP_STATE_STOPPED || g_app_state == APP_STATE_IDLE) {
                    g_current_track = playlist_current_index();
                    g_seek_on_play_position = 0;
                    play_current_track();
                } else if (g_app_state == APP_STATE_PLAYING) {
                    audio_player_pause();
                    g_app_state = APP_STATE_PAUSED;
                } else if (g_app_state == APP_STATE_PAUSED) {
                    audio_player_resume();
                    g_app_state = APP_STATE_PLAYING;
                }
            } else if (e->event == BTN_EVENT_DOUBLE_CLICK) {
                cycle_play_mode();
            } else if (e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                // R028/H1: 锁定前退出磁带模式，避免 light sleep 唤醒后状态泄漏
                if (g_app_state == APP_STATE_FAST_FORWARD) tape_control_ff_release();
                else if (g_app_state == APP_STATE_REWIND) tape_control_rewind_release();
                g_state_before_lock = g_app_state;
                g_key_locked = true;
                g_app_state = APP_STATE_LOCKED;
                ESP_LOGI(TAG, "Key lock engaged (state saved: %d)", g_state_before_lock);
            }
            break;

        /* --- 停止 --- */
        case BTN_ID_STOP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                stop_playback();
            } else if (e->event == BTN_EVENT_DOUBLE_CLICK) {
                int pos = audio_player_get_position();
                int bm = bookmark_add(g_current_track, pos);
                if (bm >= 0) {
                    ESP_LOGI(TAG, "Bookmark added at %ds (slot %d)", pos, bm);
                } else {
                    ESP_LOGW(TAG, "Bookmark add failed at %ds", pos);
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
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
                g_vol_hold_counter = 0;
                save_current_position();  // 切换前保存旧位置
                g_current_track = playlist_prev();
                playlist_set_index(g_current_track);
                g_seek_on_play_position = 0;  // 新曲目从头
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    play_current_track();
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS || e->event == BTN_EVENT_HOLD) {
                /* 长按/持续按住 → 音量减（每 100ms 减 1 级） */
                g_vol_hold_counter++;
                if (g_vol_hold_counter % 5 == 0) {
                    int vol = audio_player_get_volume();
                    if (vol > 0) {
                        audio_player_set_volume(vol - 1);
                    }
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                g_vol_hold_counter = 0;
                settings_save_volume(audio_player_get_volume());  // 松开时保存音量
            }
            break;

        /* --- 下一首 --- */
        case BTN_ID_NEXT:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                g_vol_hold_counter = 0;
                save_current_position();
                g_current_track = playlist_next();
                playlist_set_index(g_current_track);
                g_seek_on_play_position = 0;
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    play_current_track();
                }
            } else if (e->event == BTN_EVENT_LONG_PRESS || e->event == BTN_EVENT_HOLD) {
                g_vol_hold_counter++;
                if (g_vol_hold_counter % 5 == 0) {
                    int vol = audio_player_get_volume();
                    if (vol < 100) {
                        audio_player_set_volume(vol + 1);
                    }
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                g_vol_hold_counter = 0;
                settings_save_volume(audio_player_get_volume());
            }
            break;

        /* --- 快进 --- */
        case BTN_ID_FAST_FORWARD:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                skip_seconds(10);
            } else if (e->event == BTN_EVENT_LONG_PRESS || e->event == BTN_EVENT_HOLD) {
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    if (g_app_state == APP_STATE_PAUSED) {
                        audio_player_resume();
                    }
                    tape_control_ff_press();
                    audio_player_set_speed(tape_control_get_speed());
                    g_app_state = APP_STATE_FAST_FORWARD;
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                tape_control_ff_release();
                audio_player_set_speed(TAPE_SPEED_NORMAL);
                g_app_state = APP_STATE_PLAYING;
            }
            break;

        /* --- 快退 --- */
        case BTN_ID_REWIND:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                skip_seconds(-10);
            } else if (e->event == BTN_EVENT_LONG_PRESS || e->event == BTN_EVENT_HOLD) {
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    if (g_app_state == APP_STATE_PAUSED) {
                        audio_player_resume();
                    }
                    tape_control_rewind_press();
                    audio_player_set_speed(tape_control_get_speed());
                    g_app_state = APP_STATE_REWIND;
                }
            } else if (e->event == BTN_EVENT_RELEASE) {
                tape_control_rewind_release();
                audio_player_set_speed(TAPE_SPEED_NORMAL);
                g_app_state = APP_STATE_PLAYING;
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
    uint64_t now = esp_timer_get_time();
    if ((now - g_last_display_update) < 200000) return;
    g_last_display_update = now;

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
    case APP_STATE_LOCKED:       disp_state = PLAYER_STATE_STOPPED;  break;
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
 * 初始化外设
 * ============================================================ */
static void init_hardware(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  TapeBook - Tape-Style Audiobook Player");
    ESP_LOGI(TAG, "  ESP32-S3-WROOM-2 (N32R16V / Octal PSRAM)");
    ESP_LOGI(TAG, "========================================");

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. Settings（打开 NVS handle）
    settings_init();

    // 3. 显示屏
    display_init();
    display_show_splash();

    // 4. 按键
    button_manager_init();

    // 5. 磁带控制器
    tape_control_init();

    // 6. 音频播放器
    audio_player_init();
    audio_player_set_callback(on_track_finished, NULL);

    // 7. 电源管理
    power_mgmt_init();

    // 8. 书签模块
    bookmark_init();

    // 9. 看门狗初始化（10 秒超时，给 callback 内 pipeline 操作留余量）
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&twdt_config);
    esp_task_wdt_add(NULL);  // 订阅当前任务

    // 10. 加载持久化设置
    int vol = settings_load_volume();
    audio_player_set_volume(vol);
    g_play_mode = (play_mode_t)settings_load_play_mode();

    // 11. 验证所有按键 GPIO 可唤醒（RTC IO 范围检查）
    const gpio_num_t wakeup_gpios[] = {
        BTN_PLAY_PAUSE, BTN_STOP, BTN_PREV, BTN_NEXT, BTN_REWIND, BTN_FAST_FORWARD
    };
    for (size_t i = 0; i < sizeof(wakeup_gpios) / sizeof(wakeup_gpios[0]); i++) {
        assert(esp_sleep_is_valid_wakeup_gpio(wakeup_gpios[i]));
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

        // 5b. 异步 NVS 保存（避免在回调内同步写 NVS）
        if (g_pending_save_track >= 0) {
            char name[FILENAME_MAX_LEN] = "";
            playlist_get_name(g_pending_save_track, name, sizeof(name));
            settings_save_position(g_pending_save_track, g_pending_save_position, name);
            g_pending_save_track = -1;
        }

        // 6. 每 30 秒自动保存断点 + 批量 flush NVS（仅在播放/暂停时写入）
        {
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_auto_save_us) >= AUTO_SAVE_INTERVAL_US) {
                g_last_auto_save_us = now;
                if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
                    save_current_position();
                    settings_flush();
                }
            }
        }

        // 7. 定时关机检查
        if (g_app_state == APP_STATE_PLAYING || g_app_state == APP_STATE_PAUSED) {
            if (power_mgmt_auto_off_expired()) {
                ESP_LOGI(TAG, "Auto-off timer expired, stopping playback");
                audio_player_stop();
                g_app_state = APP_STATE_STOPPED;
            }
        }

        // 7b. 电源管理 tick（1Hz 周期性任务）
        {
            static uint64_t last_power_tick = 0;
            uint64_t now = esp_timer_get_time();
            if ((now - last_power_tick) >= 1000000) {
                last_power_tick = now;
                power_mgmt_tick();

                // 电量极低时保存状态并深度休眠
                if (power_mgmt_should_shutdown()) {
                    ESP_LOGE(TAG, "Battery critical, saving state and shutting down");
                    save_current_position();
                    settings_flush();
                    audio_player_stop();
                    {
                        uint64_t wakeup_mask = 0;
                        wakeup_mask |= (1ULL << BTN_PLAY_PAUSE) | (1ULL << BTN_STOP);
                        wakeup_mask |= (1ULL << BTN_PREV) | (1ULL << BTN_NEXT);
                        wakeup_mask |= (1ULL << BTN_REWIND) | (1ULL << BTN_FAST_FORWARD);
                        esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
                    }
                    esp_deep_sleep_start();
                }
            }
        }

        // 7c. 自动休眠（5 分钟无操作进入 light sleep，按键 GPIO 唤醒）
        if (power_mgmt_should_sleep()) {
            ESP_LOGI(TAG, "Idle timeout, entering light sleep");

            save_current_position();
            audio_player_stop();
            g_app_state = APP_STATE_IDLE;

            {
                uint64_t wakeup_mask = 0;
                wakeup_mask |= (1ULL << BTN_PLAY_PAUSE) | (1ULL << BTN_STOP);
                wakeup_mask |= (1ULL << BTN_PREV) | (1ULL << BTN_NEXT);
                wakeup_mask |= (1ULL << BTN_REWIND) | (1ULL << BTN_FAST_FORWARD);
                esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
            }
            esp_light_sleep_start();

            ESP_LOGI(TAG, "Woke from light sleep");

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

        // 8. SD 卡状态监测（每 5 秒轮询挂载点）
        {
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_sd_check_us) >= SD_CHECK_INTERVAL_US) {
                g_last_sd_check_us = now;
                if (g_sd_card != NULL) {
                    uint32_t buf;
                    esp_err_t ret = sdmmc_read_sectors(g_sd_card, (uint8_t *)&buf, 0, 1);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "SD card removed!");
                        audio_player_stop();
                        display_show_no_card();
                        g_app_state = APP_STATE_IDLE;
                        g_sd_card = NULL;
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
