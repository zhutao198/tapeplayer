/**
 * @file power_mgmt.h
 * @brief 电源管理模块 (P2 — V1.1+)
 *
 * 电池 ADC 检测、自动休眠、定时关机。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BAT_STATE_NORMAL = 0,   // > 15%
    BAT_STATE_LOW = 1,      // 5% ~ 15%
    BAT_STATE_CRITICAL = 2, // < 5%
} bat_state_t;

/**
 * @brief 初始化电源管理
 */
void power_mgmt_init(void);

/**
 * @brief 定时 tick（1Hz，软件定时器中调用）
 */
void power_mgmt_tick(void);

/**
 * @brief 获取电量百分比 0~100
 * @note BAT_DET (IO1/ADC1_CH0) 经 LMV321 运放送入; 换算系数见 power_mgmt.cpp
 */
int power_mgmt_get_battery_percent(void);

/**
 * @brief 是否正在充电 (CHRG IO2, 低电平=充电中)
 */
bool power_mgmt_is_charging(void);

/**
 * @brief 软关机: 脉冲 POW_EN (IO40) 释放电源锁存, 整板断电
 * @note 仅在电池临界或用户长按等终态调用
 */
void power_mgmt_power_off(void);

/**
 * @brief 获取电池状态
 */
bat_state_t power_mgmt_get_state(void);

/**
 * @brief 是否应关机（电量 < 5%）
 */
bool power_mgmt_should_shutdown(void);

/**
 * @brief 记录用户活动（按键时调用，用于自动休眠计时）
 */
void power_mgmt_record_activity(void);

/**
 * @brief 是否应自动休眠（5 分钟无操作）
 */
bool power_mgmt_should_sleep(void);

/**
 * @brief 设置定时关机
 * @param minutes 0=关闭, 15/30/60/90
 */
void power_mgmt_set_auto_off(int minutes);

/**
 * @brief 定时关机是否到期
 */
bool power_mgmt_auto_off_expired(void);

#ifdef __cplusplus
}
#endif
