/**
 * @file power_mgmt.cpp
 * @brief 电源管理 stub (P2 — V1.1+)
 *
 * V1.0 仅提供 stub，V1.1 完善 ADC 检测和休眠逻辑。
 */

#include "power_mgmt.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "power_mgmt";
static uint64_t g_last_activity_us = 0;
static int      g_auto_off_min = 0;
static uint64_t g_auto_off_start_us = 0;

void power_mgmt_init(void) {
    ESP_LOGI(TAG, "Power management init (stub for V1.1)");
    g_last_activity_us = esp_timer_get_time();
}

void power_mgmt_tick(void) {
    /* V1.1+: ADC 采样电池电压 */
}

int power_mgmt_get_battery_percent(void) {
    return 100;  /* stub: 假定满电 */
}

bat_state_t power_mgmt_get_state(void) {
    return BAT_STATE_NORMAL;
}

bool power_mgmt_should_shutdown(void) {
    return false;
}

void power_mgmt_record_activity(void) {
    g_last_activity_us = esp_timer_get_time();
}

bool power_mgmt_should_sleep(void) {
    /* V1.0 不自动休眠 */
    return false;
}

void power_mgmt_set_auto_off(int minutes) {
    g_auto_off_min = minutes;
    g_auto_off_start_us = (minutes > 0) ? esp_timer_get_time() : 0;
}

bool power_mgmt_auto_off_expired(void) {
    if (g_auto_off_min <= 0) return false;
    uint64_t elapsed_s = (esp_timer_get_time() - g_auto_off_start_us) / 1000000;
    return elapsed_s >= (uint64_t)(g_auto_off_min * 60);
}
