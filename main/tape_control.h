/**
 * @file tape_control.h
 * @brief 磁带机式快进快退控制模块
 *
 * 模拟传统磁带机的快进/快退体验：
 * - 按住快进键，播放速度逐级加速 (1.5x → 2.5x → 4x → 8x)
 * - 速度切换有时间阶梯：0.8s/2s/4s/7s 进入各档位
 * - 松开快进键，立即恢复正常速度
 * - 快退键同理，反向加速播放
 * - 加速时可以听到"变调快放"效果（通过I2S采样率控制）
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TAPE_MODE_NORMAL = 0,       // 正常播放
    TAPE_MODE_FAST_FORWARD,     // 快进
    TAPE_MODE_REWIND,           // 快退
} tape_mode_t;

/**
 * @brief 初始化磁带控制模块
 */
void tape_control_init(void);

/**
 * @brief 按下快进键
 */
void tape_control_ff_press(void);

/**
 * @brief 释放快进键，恢复速度
 */
void tape_control_ff_release(void);

/**
 * @brief 按下快退键
 */
void tape_control_rewind_press(void);

/**
 * @brief 释放快退键，恢复速度
 */
void tape_control_rewind_release(void);

/**
 * @brief 获取当前播放速度倍率
 * @return 当前速度 (1.0 = 正常)
 */
float tape_control_get_speed(void);

/**
 * @brief 获取当前磁带模式
 */
tape_mode_t tape_control_get_mode(void);

/**
 * @brief 获取当前加速档位 (0=正常, 1-4=各加速档)
 */
int tape_control_get_gear(void);

/**
 * @brief 主循环中调用，用于根据按住时长自动切换档位
 */
void tape_control_tick(void);

#ifdef __cplusplus
}
#endif
