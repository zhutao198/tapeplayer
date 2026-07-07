/**
 * @file power_mgmt.cpp
 * @brief 电源管理 stub (P2 — V1.1+)
 *
 * V1.0 仅提供 stub，V1.1 完善 ADC 检测和休眠逻辑。
 */

#include "power_mgmt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "config.h"

static const char *TAG = "power_mgmt";
static uint64_t g_last_activity_us = 0;
static int      g_auto_off_min = 0;
static uint64_t g_auto_off_start_us = 0;
static int      g_tick_count = 0;

#define SLEEP_TIMEOUT_US  (5 * 60 * 1000000ULL)  // 5 分钟无操作自动休眠
#define TICK_INTERVAL_US  (1 * 1000000)           // 1 秒

void power_mgmt_init(void)
{
    g_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Power management initialized (sleep timeout: 5min)");
}

void power_mgmt_tick(void)
{
    g_tick_count++;

    // 每 10 秒打印一次电池信息
    if (g_tick_count % 10 == 0) {
        int bat = power_mgmt_get_battery_percent();
        bat_state_t st = power_mgmt_get_state();
        if (st == BAT_STATE_LOW) {
            ESP_LOGW(TAG, "Battery low: %d%%", bat);
        } else if (st == BAT_STATE_CRITICAL) {
            ESP_LOGE(TAG, "Battery critical: %d%%", bat);
        }
    }
}

int power_mgmt_get_battery_percent(void)
{
    // V1.1+: ADC1 通道读取电池分压（GPIO1/ADC1_CH0 预留），查表换算百分比
    // V1.0: 返回 100（假定 USB 供电或满电）
    // int raw;
    // if (adc_oneshot_read(g_adc_handle, ADC_CHANNEL_0, &raw) == ESP_OK) {
    //     float voltage = raw * 3.3f / 4095.0f * 2.0f;  // 10K+10K 分压
    //     pct = (int)((voltage - 3.3f) / (4.2f - 3.3f) * 100.0f);
    //     if (pct < 0) pct = 0;
    //     if (pct > 100) pct = 100;
    // }
    return 100;
}

bat_state_t power_mgmt_get_state(void)
{
    int pct = power_mgmt_get_battery_percent();
    if (pct > 15) return BAT_STATE_NORMAL;
    if (pct > 5)  return BAT_STATE_LOW;
    return BAT_STATE_CRITICAL;
}

bool power_mgmt_should_shutdown(void)
{
    return power_mgmt_get_state() == BAT_STATE_CRITICAL;
}

void power_mgmt_record_activity(void)
{
    g_last_activity_us = esp_timer_get_time();
}

bool power_mgmt_should_sleep(void)
{
    if (g_auto_off_min > 0) return false;  // 定时关机开启时不自动休眠
    uint64_t idle_us = esp_timer_get_time() - g_last_activity_us;
    return idle_us >= SLEEP_TIMEOUT_US;
}

void power_mgmt_set_auto_off(int minutes)
{
    g_auto_off_min = minutes;
    g_auto_off_start_us = (minutes > 0) ? esp_timer_get_time() : 0;
}

bool power_mgmt_auto_off_expired(void)
{
    if (g_auto_off_min <= 0) return false;
    uint64_t elapsed_s = (esp_timer_get_time() - g_auto_off_start_us) / 1000000;
    return elapsed_s >= (uint64_t)(g_auto_off_min * 60);
}
