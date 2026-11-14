/*
 * TapeBook LCD SPI 频率扫描诊断固件 (2026-11-14)
 * ============================================================
 * 背景: 多块新板黑屏——信号在模组端全对(电源/背光/CS/DC/RES/SCL/SDA)、
 *       故障随板走(交叉换模组无效)、一块永久黑、几块间歇(曾出现又恢复)、
 *       USB/电池一样。怀疑 SPI 信号余量不足(时钟/驱动能力贴边)。
 *
 * 专用工程: 与主工程平级, 主工程构建不涉及本目录; 本固件也不引 ADF/LVGL。
 * 烧录方式: 只写 0x10000(app 分区), 不动 bootloader/分区表/otadata。
 *           诊断完把主固件 app 写回 0x10000 即恢复。
 *
 * 方法: 两轮扫描——
 *       第一轮: 默认 GPIO 驱动能力, 频率 80/60/40/20/10/5/1 MHz 降序
 *       第二轮: 降驱动能力(GPIO_DRIVE_CAP_1, 边沿变缓治振铃), 同样降序
 *       每档: LCD上电->硬复位->ST7789 init->纯色填充(红->绿->蓝)->保持1.5s
 *       串口每步同步打印频率/颜色, 人眼对照屏幕。
 *
 * 判读:
 *   - 某档亮起          => 信号余量问题实锤, 该档即主固件修复值(pclk_hz)
 *   - 某档亮但颜色错乱  => 数据损坏, 同样降频/降驱动
 *   - 所有频率全不亮    => 非时钟问题(GPIO8/16/18 pad 损坏或面板通路), 走硬件返修
 *
 * 参数与主固件 display.cpp lcd_hw_init 逐项对齐(总线/面板/RGB顺序/invert/时序),
 * 保证诊断结果可直接映射回主固件。
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "lcd_sweep";

/* ---- 与主固件 config.h / display.cpp 对齐 ---- */
#define PIN_LCD_POW   GPIO_NUM_39   /* PMOS 低导通 */
#define PIN_RES       GPIO_NUM_17
#define PIN_BLK       GPIO_NUM_15
#define PIN_MOSI      GPIO_NUM_18
#define PIN_SCLK      GPIO_NUM_8
#define PIN_DC        GPIO_NUM_16
#define SPI_HOST      SPI3_HOST

#define LCD_W         320
#define LCD_H         240
#define LCD_BLK_LEDC_CH    LEDC_CHANNEL_0
#define LCD_BLK_LEDC_TIMER LEDC_TIMER_0
#define LCD_BLK_PWM_HZ     5000
#define LCD_BLK_PWM_BITS   10

/* 频率阶梯(MHz): 覆盖主固件的 20MHz, 上下都探 */
static const int s_freqs_mhz[] = { 80, 60, 40, 20, 10, 5, 1 };
#define NFREQ (int)(sizeof(s_freqs_mhz) / sizeof(s_freqs_mhz[0]))

/* RGB565 纯色(注意 invert_color=true 开启, 实际显示为补色: 红->青, 绿->品红, 蓝->黄) */
static const uint16_t s_colors[] = { 0xF800, 0x07E0, 0x001F };
static const char    *s_color_names[] = { "RED", "GREEN", "BLUE" };

static void backlight_init_100(void)
{
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = (ledc_timer_bit_t)LCD_BLK_PWM_BITS;
    t.timer_num       = LCD_BLK_LEDC_TIMER;
    t.freq_hz         = LCD_BLK_PWM_HZ;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {};
    ch.gpio_num   = PIN_BLK;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel    = LCD_BLK_LEDC_CH;
    ch.timer_sel  = LCD_BLK_LEDC_TIMER;
    ch.duty       = (1 << LCD_BLK_PWM_BITS) - 1;   /* 100% 常亮, 排除背光变量 */
    ch.hpoint     = 0;
    ledc_channel_config(&ch);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BLK_LEDC_CH, ch.duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BLK_LEDC_CH);
    ESP_LOGI(TAG, "backlight 100%% (duty=%d)", ch.duty);
}

/* 与主固件一致的硬复位序列: 高->10ms->低20ms->高->120ms */
static void hw_reset_panel(void)
{
    gpio_set_level(PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* 分块填充纯色(40 行/块, 与主固件清屏同款分片, 避开 SPI 单事务上限) */
static void fill_solid(esp_lcd_panel_handle_t panel, uint16_t rgb565)
{
    static uint16_t buf[LCD_W * 40];
    for (int i = 0; i < LCD_W * 40; i++) buf[i] = rgb565;
    for (int y = 0; y < LCD_H; y += 40) {
        int h = (y + 40 <= LCD_H) ? 40 : (LCD_H - y);
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + h, buf);
    }
}

/* 在指定频率/驱动能力下: 建 io -> 复位 -> 建 panel -> init -> 填充 */
static bool try_freq(int mhz, gpio_drive_cap_t drive, uint16_t rgb565, const char *cname)
{
    char io_name[24];
    snprintf(io_name, sizeof(io_name), "io_%dMHz_cap%d", mhz, (int)drive);

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num        = -1;                 /* CS 接地, 3 线制 */
    io_cfg.dc_gpio_num        = PIN_DC;
    io_cfg.spi_mode           = 0;
    io_cfg.pclk_hz            = mhz * 1000 * 1000;
    io_cfg.trans_queue_depth  = 10;
    io_cfg.lcd_cmd_bits       = 8;
    io_cfg.lcd_param_bits     = 8;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t r = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST, &io_cfg, &io);
    if (r != ESP_OK || !io) {
        ESP_LOGE(TAG, "[%s] new_panel_io FAILED 0x%x", io_name, r);
        return false;
    }

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num  = PIN_RES;
    pcfg.rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB;   /* 主固件实测本模组需 RGB */
    pcfg.bits_per_pixel  = 16;

    esp_lcd_panel_handle_t panel = NULL;
    r = esp_lcd_new_panel_st7789(io, &pcfg, &panel);
    if (r != ESP_OK || !panel) {
        ESP_LOGE(TAG, "[%s] new_panel FAILED 0x%x", io_name, r);
        esp_lcd_panel_io_del(io);
        return false;
    }

    hw_reset_panel();                       /* 手动复位(不调 panel_reset, 同主固件) */
    r = esp_lcd_panel_init(panel);
    esp_lcd_panel_swap_xy(panel, true);      /* 主固件: 320x240 横屏 */
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);

    fill_solid(panel, rgb565);

    ESP_LOGW(TAG, "[%s] >>> freq=%dMHz cap=%d color=%s(显示为补色) init_ret=0x%x  hold 1.5s <<<",
             io_name, mhz, (int)drive, cname, r);
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_lcd_panel_del(panel);
    esp_lcd_panel_io_del(io);
    return true;
}

static void run_pass(int pass_no, gpio_drive_cap_t drive)
{
    ESP_LOGW(TAG, "===== PASS %d: drive_cap=%d, freqs 80->1 MHz =====", pass_no, (int)drive);
    for (int i = 0; i < NFREQ; i++) {
        uint16_t c = s_colors[i % 3];
        try_freq(s_freqs_mhz[i], drive, c, s_color_names[i % 3]);
    }
}

void app_main(void)
{
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, " LCD SPI sweep diag (R167-DIAG) start");
    ESP_LOGW(TAG, " 盯屏: 从哪一档开始亮/颜色是否正确");
    ESP_LOGW(TAG, "========================================");

    /* LCD 软电源: 拉低导通 */
    gpio_set_direction(PIN_LCD_POW, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_POW, 0);
    ESP_LOGI(TAG, "LCD_POW_EN=0 readback=%d", gpio_get_level(PIN_LCD_POW));

    gpio_set_direction(PIN_RES, GPIO_MODE_OUTPUT);
    backlight_init_100();

    /* SPI3 总线(与主固件同参) */
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num     = PIN_MOSI;
    buscfg.miso_io_num     = -1;
    buscfg.sclk_io_num     = PIN_SCLK;
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = 32752;
    esp_err_t r = spi_bus_initialize(SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize FAILED 0x%x", r);
        return;
    }

    /* PASS 1: 默认驱动能力 */
    run_pass(1, GPIO_DRIVE_CAP_2);

    /* PASS 2: 降驱动能力(治振铃) —— 对 SCLK/MOSI/DC 三个信号脚 */
    gpio_set_drive_capability(PIN_SCLK, GPIO_DRIVE_CAP_1);
    gpio_set_drive_capability(PIN_MOSI, GPIO_DRIVE_CAP_1);
    gpio_set_drive_capability(PIN_DC,   GPIO_DRIVE_CAP_1);
    ESP_LOGW(TAG, "drive cap lowered to 1 for SCLK/MOSI/DC");
    run_pass(2, GPIO_DRIVE_CAP_1);

    /* 收尾: 1MHz 白屏常亮 + 完成标志, 循环慢闪便于确认扫描已结束 */
    ESP_LOGW(TAG, "===== SWEEP DONE: 记录哪一档亮 => 修复值; 全不亮 => 硬件问题 =====");
    for (;;) {
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = -1; io_cfg.dc_gpio_num = PIN_DC; io_cfg.spi_mode = 0;
        io_cfg.pclk_hz = 1 * 1000 * 1000; io_cfg.trans_queue_depth = 10;
        io_cfg.lcd_cmd_bits = 8; io_cfg.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t io = NULL;
        if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST, &io_cfg, &io) == ESP_OK) {
            esp_lcd_panel_dev_config_t pcfg = {};
            pcfg.reset_gpio_num = PIN_RES; pcfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB; pcfg.bits_per_pixel = 16;
            esp_lcd_panel_handle_t panel = NULL;
            if (esp_lcd_new_panel_st7789(io, &pcfg, &panel) == ESP_OK) {
                hw_reset_panel();
                esp_lcd_panel_init(panel);
                esp_lcd_panel_swap_xy(panel, true);
                esp_lcd_panel_mirror(panel, true, false);
                esp_lcd_panel_invert_color(panel, true);
                esp_lcd_panel_disp_on_off(panel, true);
                for (;;) {
                    fill_solid(panel, 0xFFFF);   /* 白(invert 后为黑) */
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    fill_solid(panel, 0x0000);   /* 黑(invert 后为白) */
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
