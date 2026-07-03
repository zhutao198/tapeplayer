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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
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
} app_state_t;

static app_state_t    g_app_state = APP_STATE_IDLE;
static int            g_current_track = 0;
static bool           g_key_locked = false;
static app_state_t    g_state_before_lock = APP_STATE_STOPPED;
static uint64_t       g_last_display_update = 0;
static int            g_vol_hold_counter = 0;  // 音量长按计数器（每 5 步=100ms 调 1 级）
static int            g_seek_on_play_position = 0;  // 断点恢复 seek 目标（秒）
static uint64_t       g_last_auto_save_us = 0;      // 上次自动保存时间
static play_mode_t    g_play_mode = PLAY_MODE_SEQUENCE;

static sdmmc_card_t   *g_sd_card = NULL;  // SD 卡句柄
static uint64_t       g_last_sd_check_us = 0;  // SD 卡状态检查计时

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
    save_current_position();  // 停止前保存
    audio_player_stop();
    if (g_app_state == APP_STATE_FAST_FORWARD || g_app_state == APP_STATE_REWIND) {
        tape_control_ff_release();   // 确保退出磁带模式
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

    // 保存并归零（已听完）
    char name[FILENAME_MAX_LEN] = "";
    playlist_get_name(g_current_track, name, sizeof(name));
    settings_save_position(g_current_track, 0, name);

    // 根据播放模式决定下一首
    switch (g_play_mode) {
    case PLAY_MODE_SEQUENCE:
        if (g_current_track < playlist_count() - 1) {
            g_current_track = playlist_next();
            playlist_set_index(g_current_track);
            g_seek_on_play_position = 0;  // 新曲目从头播放
            play_current_track();
        } else {
            g_app_state = APP_STATE_STOPPED;
            ESP_LOGI(TAG, "Playlist finished (sequence mode)");
        }
        break;

    case PLAY_MODE_REPEAT_ALL:
        g_current_track = playlist_next();  // 循环
        playlist_set_index(g_current_track);
        g_seek_on_play_position = 0;
        play_current_track();
        break;

    case PLAY_MODE_REPEAT_ONE:
        g_seek_on_play_position = 0;  // 从头
        play_current_track();
        break;
    }
}

/* ============================================================
 * 按键事件处理
 * ============================================================ */
static void handle_button_events(void)
{
    btn_event_info_t events[8];
    int n = button_manager_scan(events, sizeof(events) / sizeof(events[0]));

    for (int i = 0; i < n; i++) {
        btn_event_info_t *e = &events[i];

        /* --- 按键锁定模式下，仅响应解锁操作 --- */
        if (g_key_locked) {
            if (e->id == BTN_ID_PLAY_PAUSE && e->event == BTN_EVENT_EXTRA_LONG_PRESS) {
                g_key_locked = false;
                g_app_state = g_state_before_lock;  // 恢复锁定前的状态
                ESP_LOGI(TAG, "Key lock released, state restored to %d", g_app_state);
            }
            continue;
        }

        /* 记录用户活动（用于自动休眠计时） */
        if (e->event != BTN_EVENT_NONE) {
            power_mgmt_record_activity();
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
                voice_prompt_status();
            } else if (e->event == BTN_EVENT_LONG_PRESS) {
                ESP_LOGI(TAG, "Folder browse (stub - V1.1)");
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
    playlist_get_name(g_current_track, track_name, sizeof(track_name));

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
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

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

    // 8. 看门狗初始化（5 秒超时，超时后 panic）
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 5000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&twdt_config);
    esp_task_wdt_add(NULL);  // 订阅当前任务

    // 9. 加载持久化设置
    int vol = settings_load_volume();
    audio_player_set_volume(vol);
    g_play_mode = (play_mode_t)settings_load_play_mode();
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

        // 5. 每 30 秒自动保存断点
        if (g_app_state == APP_STATE_PLAYING) {
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_auto_save_us) >= AUTO_SAVE_INTERVAL_US) {
                save_current_position();
                g_last_auto_save_us = now;
            }
        }

        // 6. SD 卡状态监测（每 5 秒）
        // IDF v5.5 移除了 sdmmc_card_state_t / sdmmc_get_state，此功能暂时禁用
        // V1.1: 改用 sdmmc_io_check_events 重新实现
#if 0
        {
            uint64_t now = esp_timer_get_time();
            if ((now - g_last_sd_check_us) >= SD_CHECK_INTERVAL_US) {
                g_last_sd_check_us = now;
                if (g_sd_card != NULL) {
                    sdmmc_card_state_t sd_state = sdmmc_get_state(g_sd_card);
                    if (sd_state == SDMMC_CARD_REMOVED) {
                        ESP_LOGW(TAG, "SD card removed!");
                        audio_player_stop();
                        display_show_no_card();
                        g_app_state = APP_STATE_IDLE;
                        // 等待重新插入后重新挂载
                        // V1.1: 自动重新挂载并恢复
                    }
                }
            }
        }
#endif

        // 7. 看门狗复位
        esp_task_wdt_reset();

        // 8. 更新显示屏
        update_display();

        // 9. 休眠，控制循环频率
        vTaskDelay(pdMS_TO_TICKS(BTN_SCAN_INTERVAL));
    }
}
