/**
 * @file display.h
 * @brief ST7789 TFT 显示模块 (原生 esp_lcd + LVGL v9)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 播放状态 */
typedef enum {
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_FAST_FORWARD,
    PLAYER_STATE_REWIND,
} player_state_t;

/**
 * @brief 初始化 TFT 显示屏 (esp_lcd ST7789 + LVGL)
 */
void display_init(void);

/**
 * @brief 更新屏幕显示
 * @param state       播放状态
 * @param track_name  曲目名称
 * @param track_idx   当前曲目索引 (1-based)
 * @param total       总曲目数
 * @param current_sec 当前播放秒数
 * @param total_sec   总时长秒数 (0=未知)
 * @param speed       播放速度倍率
 * @param gear        加速档位 (0=正常)
 * @param volume      音量 level 0..14 (15 档逻辑音量)
 */
void display_update(player_state_t state,
                    const char *track_name,
                    int track_idx, int total,
                    int current_sec, int total_sec,
                    float speed, int gear, int volume);

/**
 * @brief 显示启动画面
 */
void display_show_splash(void);

/**
 * @brief 显示"无音频文件"提示
 */
void display_show_no_files(void);

/**
 * @brief 显示"无 SD 卡"提示
 */
void display_show_no_card(void);

/**
 * @brief 设置 TF 卡在位状态 (状态栏 SD 图标 + 插拔瞬时提示)
 * @param present true=已插入, false=已弹出
 */
void display_set_sd_present(bool present);

/**
 * @brief 仅初始化 TF 卡图标状态 (无插拔提示, 用于开机)
 * @param present true=已插入, false=已弹出
 */
void display_set_sd_present_init(bool present);

/**
 * @brief 设置屏幕背光亮度 (0-100)
 * @note TFT_BLK (IO15) 由 LEDC PWM 控制
 */
void display_set_brightness(int percent);

/**
 * @brief 控制 LCD 软件电源 (PMOS 软开关, IO39)
 * @param on true=上电显示, false=断电省电
 */
void display_power(bool on);

/**
 * @brief 设置当前播放模式 (供主界面状态栏显示)
 * @param mode 0=顺序(SEQ) 1=全部循环(ALL) 2=单曲循环(ONE)
 */
void display_set_play_mode(int mode);

/**
 * @brief 触发音量条显示 (调节音量时调用)
 * @note 调用后音量条立即显示最新音量, 并在停止调节 3 秒后自动隐藏
 * @param volume 当前音量 level 0..VOLUME_LEVEL_MAX (15 档逻辑音量)
 */
void display_show_volume(int volume);

#define BROWSE_VISIBLE_LINES 6

/**
 * @brief 文件夹浏览列表
 * @param selected   当前选中索引 (0-based)
 * @param total      总文件数
 * @param lines      预格式化的显示行数组（每行含标记+文件名，如 "> song.mp3"）
 * @param count      行数
 */
void display_show_browse(int selected, int total, char lines[][24], int count);

/**
 * @brief 统一设置菜单显示 (R049)
 * @param title  当前层标题
 * @param lines  预格式化显示行数组（含标记+文本，如 "> 播放模式: 顺序播放"）
 * @param count  总行数
 * @param sel    当前选中索引 (未使用, 预留)
 * @param hint   底部操作提示
 */
void display_show_menu(const char *title, char lines[][24], int count, int sel, const char *hint);

/**
 * @brief 通用信息屏 (R049c 桩功能提示，如 OTA/USB/关于)
 * @param title 标题（居中首行）
 * @param text  多行正文（'\n' 分隔，居中）
 */
void display_show_info(const char *title, const char *text);

/* ---- SD-OTA 升级界面（R049c 真实化）---- */
void display_show_ota_confirm(const char *cur_ver, const char *new_ver,
                              long size_kb, const char *img_name, bool battery_ok);
void display_show_ota_progress(int percent);
void display_show_ota_done(void);
void display_show_ota_error(const char *msg);

#ifdef __cplusplus
}
#endif
