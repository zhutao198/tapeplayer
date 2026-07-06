/**
 * @file settings.h
 * @brief NVS 持久化设置模块 — 断点续播、音量、播放模式存储
 */

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
 * @param position_s 播放位置（秒），0 表示从头
 * @param file_name  文件名（用于校验文件是否仍存在）
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

/**
 * @brief 提交所有未保存的设置（批量 flush，降低 flash 磨损）
 */
void settings_flush(void);

#ifdef __cplusplus
}
#endif
