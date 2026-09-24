/**
 * @file power_mgmt.h
 * @brief 电源管理模块 (P2 — V1.1+)
 *
 * 电池 ADC 检测、自动休眠、定时关机、关机倒计时。
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

typedef enum {
    SHUTDOWN_COUNTDOWN_NONE = 0,
    SHUTDOWN_COUNTDOWN_LOW_BATTERY = 1,
    SHUTDOWN_COUNTDOWN_AUTO_OFF = 2,
} shutdown_countdown_type_t;

void power_mgmt_init(void);
void power_mgmt_tick(void);
int  power_mgmt_get_battery_percent(void);
bool power_mgmt_is_charging(void);
void power_mgmt_power_off(void);
bat_state_t power_mgmt_get_state(void);
bool power_mgmt_should_shutdown(void);
void power_mgmt_record_activity(void);
bool power_mgmt_should_sleep(void);
void power_mgmt_set_auto_off(int minutes);
bool power_mgmt_auto_off_expired(void);

/* 关机倒计时 */
void power_mgmt_start_shutdown_countdown(shutdown_countdown_type_t type, int seconds);
void power_mgmt_cancel_shutdown_countdown(void);
int  power_mgmt_get_shutdown_countdown_remaining(void);
shutdown_countdown_type_t power_mgmt_get_shutdown_countdown_type(void);
bool power_mgmt_shutdown_countdown_expired(void);
void power_mgmt_reset_auto_off_timer(void);

#ifdef __cplusplus
}
#endif
