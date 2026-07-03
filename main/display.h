/**
 * @file display.h
 * @brief SSD1306 OLED 显示模块
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
 * @brief 初始化 OLED 显示屏
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
 * @param volume      音量 0-100
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

#ifdef __cplusplus
}
#endif
