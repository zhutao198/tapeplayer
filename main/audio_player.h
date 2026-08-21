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
 * - 跳帧仅在 ≥8x 最高档位执行，1.5x/2.0x/3.0x 仅变速不跳帧（变调靠 I2S 输出采样率倍增）
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
 * @param filepath  完整文件路径（非 NULL，须以 '/' 开头的合法路径）
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
 * @brief 设置音量 (level 0..14, 15 档逻辑音量, 线性 dB -96..+12)
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

/* ============================================================
 * A-B 区间复读 (R049b)
 * ============================================================ */
/** 在当前播放位置标记 A 点（若已标记 B 则清除 B 重新选择） */
void audio_player_mark_a(void);
/** 在当前播放位置标记 B 点（需先标记 A） */
void audio_player_mark_b(void);
/** 清除 A/B 标记并关闭复读 */
void audio_player_clear_ab(void);
/** 开启/关闭复读（标记仍存在，仅控制是否循环） */
void audio_player_set_ab_enabled(bool en);
/** 复读是否开启 */
bool audio_player_is_ab_enabled(void);
/** A 点位置(ms)，未标记返回 -1 */
int  audio_player_ab_a_ms(void);
/** B 点位置(ms)，未标记返回 -1 */
int  audio_player_ab_b_ms(void);
/** 直接设置 A 点位置(ms)（菜单微调用；负无效；若越过 B 则 B 顺延） */
void audio_player_set_ab_a_ms(int ms);
/** 直接设置 B 点位置(ms)（菜单微调用；自动保证 B>A+1000ms） */
void audio_player_set_ab_b_ms(int ms);

/* ============================================================
 * 按键提示音 (R049c) — 仅在非播放态（菜单/浏览/停止）有效
 * ============================================================ */
void audio_player_play_beep(void);

/* ============================================================
 * 蓝牙音箱 (A2DP Sink) 控制接口 — 需 CONFIG_USE_BT_SPEAKER
 * ============================================================ */
bool audio_player_start_bt(void);
void audio_player_stop_bt(void);
bool audio_player_is_bt_active(void);

#ifdef __cplusplus
}
#endif
