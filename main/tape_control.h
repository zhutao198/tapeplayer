/**
 * @file tape_control.h
 * @brief 磁带机式快进快退控制模块
 *
 * 模拟传统磁带机的快进/快退体验：
 * - 按住快进键，播放速度逐级加速 (1.5x → 2.0x → 3.0x → 8x)
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
 * @brief 根据档位 (1~NUM_SPEED_STEPS) 返回对应播放速度倍率
 * @param gear 档位 (1=第一档, 2=第二档, ...)
 * @return 速度倍率 (1.0=正常档)；非法档位返回 0.0
 *
 * R035-006：display.cpp 的 gear_str() 此前持有独立的 `float speeds[]`
 * 双源，与本模块 g_speed_steps[] 硬编码 TAPE_SPEED_* 常量重复，
 * 修改档位定义时易漂移。现统一从 g_speed_steps[] 派生。
 */
float tape_control_get_gear_speed(int gear);

/**
 * @brief 返回最高档位对应的播放速度倍率（g_speed_steps[NUM_SPEED_STEPS-1].speed）
 *        用于派生 audio_player 的"高档位跳帧"判定阈值（替代硬编码 4.0f，R034-011）
 * @return 最高档位速度倍率（如 4.0f）；若 NUM_SPEED_STEPS=0 则返回 1.0f
 */
float tape_control_get_max_gear_speed(void);

/**
 * @brief 是否处于"跳帧模式"（R034-011）
 *
 * 用于 audio_player 在 FF/RW 高档位时跳帧推进，避免与 tape_control.cpp
 * 的 g_speed_steps 数组档位数硬耦合。修改 NUM_SPEED_STEPS 时无需同步
 * 修改 audio_player。
 *
 * @return true = 当前处于 FF/RW 且档位 ≥ 最高档（跳帧快进模式）
 */
bool tape_control_is_scrub_mode(void);

/**
 * @brief 主循环中调用，用于根据按住时长自动切换档位
 */
void tape_control_tick(void);

#ifdef __cplusplus
}
#endif
