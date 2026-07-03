/**
 * @file audio_player.h
 * @brief 音频播放引擎 (基于 ESP-ADF)
 *
 * 使用 ESP-ADF 的 audio_pipeline + audio_element 框架：
 *   SD Card Reader → Audio Decoder → I2S Writer
 *
 * 关键设计：
 * - 每次播放重新创建 pipeline（避免 terminate 后复用失败）
 * - seek/tick 使用毫秒级精度
 * - 跳帧仅在 ≥4x 高档位执行，1.5x/2.5x 仅变速不跳帧
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 播放状态回调 */
typedef void (*audio_status_cb_t)(int state, void *user_data);

/**
 * @brief 初始化音频子系统 (I2S 输出流)
 */
void audio_player_init(void);

/**
 * @brief 播放指定文件
 * @param filepath  完整文件路径
 * @return true=成功开始 / false=失败
 */
bool audio_player_play(const char *filepath);

/**
 * @brief 暂停播放
 */
void audio_player_pause(void);

/**
 * @brief 恢复播放
 */
void audio_player_resume(void);

/**
 * @brief 停止播放（销毁管道，下次 play 重建）
 */
void audio_player_stop(void);

/**
 * @brief 跳转到指定位置（秒）
 * @param seconds 目标秒数
 */
void audio_player_seek(int seconds);

/**
 * @brief 跳转到指定位置（毫秒，内部使用）
 * @param ms 目标毫秒数
 */
void audio_player_seek_ms(int ms);

/**
 * @brief 获取当前播放位置 (毫秒)
 */
int audio_player_get_position_ms(void);

/**
 * @brief 获取当前播放位置 (秒)
 */
int audio_player_get_position(void);

/**
 * @brief 获取总时长 (秒)，未知返回0
 */
int audio_player_get_duration(void);

/**
 * @brief 检查是否正在播放
 */
bool audio_player_is_playing(void);

/**
 * @brief 检查是否已暂停
 */
bool audio_player_is_paused(void);

/**
 * @brief 设置播放速度倍率 (1.0=正常, >1.0=加速, <0=反向)
 * @param speed 速度倍率
 */
void audio_player_set_speed(float speed);

/**
 * @brief 设置音量 (0-100)
 */
void audio_player_set_volume(int volume);

/**
 * @brief 获取当前音量
 */
int audio_player_get_volume(void);

/**
 * @brief 主循环中调用，处理跳帧和管道状态
 */
void audio_player_tick(void);

/**
 * @brief 设置状态回调
 */
void audio_player_set_callback(audio_status_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
