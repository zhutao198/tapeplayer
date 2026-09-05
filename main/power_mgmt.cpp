/**
 * @file power_mgmt.cpp
 * @brief 电源管理 (V1.1+, 基于 PADS 原理图 V1 网表修正)
 *
 * 关键 GPIO (以原理图为准):
 *   BAT_DET (IO1 / ADC1_CH0) — 电池电压, 经 LMV321 运放送入
 *   CHRG    (IO2)            — 充电状态, 低电平=充电中
 *   POW_EN  (IO40)           — 电源锁存控制, 脉冲释放=整板断电
 *   LCD_POW_EN (IO39)        — LCD 软电源开关 (display.cpp 管理)
 */

#include "power_mgmt.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"

static const char *TAG = "power_mgmt";
static uint64_t g_last_activity_us = 0;
static int      g_auto_off_min = 0;
static uint64_t g_auto_off_start_us = 0;
static int      g_tick_count = 0;

/* 电池电压换算系数。
 * 原理图电池分压 (经 LMV321 电压跟随器送入 IO1/ADC1_CH0):
 *   VBAT → R34(45.3K, 上) → 分压点 → R35(110K, 下) → GND
 *   分压点电压 = VBAT × R35/(R34+R35) = VBAT × 110/155.3 ≈ VBAT × 0.708
 *   还原系数 = (R34+R35)/R35 = 155.3/110 ≈ 1.412 (原 2.0 假设 1:1 分压, 错误)。
 * ⚠️ 若改电阻值, 仅修改下列两值, 增益自动重算。 */
#define BAT_DIV_R_TOP   45.3f   // R34 上分压电阻 (KΩ)
#define BAT_DIV_R_BOT   110.0f  // R35 下分压电阻/对地 (KΩ)
#define BAT_ADC_GAIN    ((BAT_DIV_R_TOP + BAT_DIV_R_BOT) / BAT_DIV_R_BOT)  // ≈ 1.412
#define BAT_V_MIN          3.0f    // 0% 对应电压
#define BAT_V_MAX          4.2f    // 100% 对应电压
#define BAT_ADC_MAX_RAW    4095
#define BAT_ADC_VREF       3.3f

/* ADC 单次采样句柄 (BAT_DET) */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

#define SLEEP_TIMEOUT_US  (5 * 60 * 1000000ULL)  // 5 分钟无操作自动休眠
#define TICK_INTERVAL_US  (1 * 1000000)           // 1 秒

void power_mgmt_init(void)
{
    g_last_activity_us = esp_timer_get_time();
    g_auto_off_min = settings_load_auto_off();
    if (g_auto_off_min > 0) {
        g_auto_off_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Auto-off restored: %d min", g_auto_off_min);
    }

    /* BAT_DET: ADC1_CH0 (IO1) */
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&adc_cfg, &s_adc_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_11,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_0, &chan_cfg);
        ESP_LOGI(TAG, "BAT_DET ADC1_CH0 (IO1) initialized");
    } else {
        ESP_LOGW(TAG, "BAT_DET ADC init failed, battery stuck at 100%%");
    }

    /* CHRG: IO2 输入, 低=充电中 */
    gpio_set_direction(CHRG_DET_IO, GPIO_MODE_INPUT);

    /* POW_EN 配置。
     * 如果用户已用跳线旁路硬件锁存电路，IO40 任何后续操作都可能让硬件 latch
     * 误触为"关机脉冲"造成意外断电，因此跳过重复初始化与重复拉高。
     * 如果未旁路（正常硬件），则按常规 init。 */
#if TAPEBOOK_POWER_LATCH_BYPASSED
    /* 跳过: 主程序在 app_main 最开始已把 IO40 拉高，此处不要再动 */
#else
    gpio_set_direction(POW_EN_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(POW_EN_IO, 1);
#endif

    ESP_LOGI(TAG, "Power management initialized (sleep timeout: 5min)");
}

void power_mgmt_tick(void)
{
    g_tick_count++;

    if (g_tick_count % 10 == 0) {
        int bat = power_mgmt_get_battery_percent();
        bat_state_t st = power_mgmt_get_state();
        if (st == BAT_STATE_LOW) {
            ESP_LOGW(TAG, "Battery low: %d%%", bat);
        } else if (st == BAT_STATE_CRITICAL) {
            ESP_LOGE(TAG, "Battery critical: %d%%", bat);
        }
        if (power_mgmt_is_charging()) {
            ESP_LOGI(TAG, "Charging... bat=%d%%", bat);
        }
    }
}

int power_mgmt_get_battery_percent(void)
{
    if (!s_adc_handle) return 100;   // ADC 未就绪, 假定满电

    /* P1-fix: ADC 读电池需 ~1-5ms 阻塞, 缓存 5 秒避免 display_update 每秒阻塞 */
    static int s_cached_pct = 100;
    static uint64_t s_last_read_us = 0;
    uint64_t now = esp_timer_get_time();
    if (now - s_last_read_us < 5000000) return s_cached_pct;
    s_last_read_us = now;

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, ADC_CHANNEL_0, &raw) != ESP_OK) {
        return 100;
    }
    float v_adc = (float)raw * BAT_ADC_VREF / BAT_ADC_MAX_RAW;   // 0~3.3V
    float v_bat = v_adc * BAT_ADC_GAIN;                          // 还原电池电压
    int pct = (int)((v_bat - BAT_V_MIN) / (BAT_V_MAX - BAT_V_MIN) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_cached_pct = pct;
    return pct;
}

bool power_mgmt_is_charging(void)
{
    /* CHRG 低电平=充电中 */
    return gpio_get_level(CHRG_DET_IO) == 0;
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
    /* R031-005: 关机门槛从 0% 提到 5%（电量过低锂电池有损坏风险）
     * 但如果正在充电（CHRG 低电平），即使读不到电池电压也不该触发关机
     * —— USB 供电不接电池时 BAT_DET ADC 读到 0V，会被误判为电量极低。 */
    if (power_mgmt_is_charging()) {
        return false;
    }
    return power_mgmt_get_state() == BAT_STATE_CRITICAL;
}

void power_mgmt_record_activity(void)
{
    g_last_activity_us = esp_timer_get_time();
}

bool power_mgmt_should_sleep(void)
{
    // 用户决定（R089 后）：屏蔽所有自动息屏/休眠，开机即屏幕常亮。
    // 此前 light sleep 会使屏幕熄灭且偶发无法恢复。恒返回 false → 永不自动休眠。
    return false;
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

void power_mgmt_power_off(void)
{
#if TAPEBOOK_POWER_LATCH_BYPASSED
    /* 用户已用跳线旁路硬件锁存电路，IO40 拉低会产生 latch 误触风险。
     * 此函数仅打日志，不再真正操作 IO40，也不再进入 deep-sleep。
     * 真正的"关机"逻辑由用户在外部断电（电池低时仅警告不强行断电）。 */
    ESP_LOGW(TAG, "power_mgmt_power_off disabled (TAPEBOOK_POWER_LATCH_BYPASSED); battery-low warning only");
#else
    ESP_LOGI(TAG, "Soft power off: pulsing POW_EN (IO40) to release latch");
    /* 拉低 POW_EN ~2s 释放电源锁存 (MX66100T), 整板断电 */
    gpio_set_level(POW_EN_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level(POW_EN_IO, 1);
    /* 若锁存未完全切断, 进入深度休眠兜底 */
    esp_deep_sleep_start();
#endif
}
