/**
 * @file display.cpp
 * @brief ST7789 TFT 显示实现 (2.0", 原生 esp_lcd + LVGL)
 *
 * 架构:
 *   - ESP-IDF 原生 esp_lcd 组件:
 *       esp_lcd_spi (SPI3_HOST IO 层) + esp_lcd_panel_st7789 (官方面板驱动)
 *   - esp_lcd_panel_draw_bitmap() 作为 LVGL 的 flush_cb 底层，原生打通 GUI。
 *   - 屏幕方向 (横屏 320x240 / 竖屏 240x320) 由 DISPLAY_ORIENTATION 决定，
 *     仅在初始化时设置 esp_lcd 的 swap_xy/mirror，应用层无需关心。
 *
 * 引脚见 config.h:
 *   SPI3_HOST (SCLK GPIO8 / MOSI GPIO18), DC GPIO16, RESET GPIO17,
 *   BLK GPIO15 (LEDC PWM), CS 接地, LCD_POW_EN GPIO39 (PMOS 低电平导通)。
 * SD 卡走独立的 SPI2_HOST，与显示总线互不冲突。
 */

#include "display.h"
#include "config.h"
#include "tape_control.h"
#include "power_mgmt.h"
#include "esp_timer.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"

/* 自定义中文字体（ui_font_*.c 生成）：显式声明供本编译单元使用 */
LV_FONT_DECLARE(lv_font_chinese_12);
LV_FONT_DECLARE(lv_font_chinese_14);
LV_FONT_DECLARE(lv_font_chinese_16);

static const char *TAG = "display";

static bool g_display_initialized = false;
static bool g_display_sleep       = false;

/* esp_lcd 句柄 */
static esp_lcd_panel_io_handle_t s_io_handle    = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;

/* LVGL 刷新缓冲 (优先 PSRAM) */
static lv_color_t *s_lv_buf = NULL;

/* LVGL UI 对象 */
static lv_obj_t *g_player = NULL;   // 播放界面容器
static lv_obj_t *g_msg    = NULL;   // 居中消息标签 (splash/提示)

static lv_obj_t *lbl_status  = NULL; // 状态栏: 播放态/曲目/模式/电池/音量
static lv_obj_t *lbl_title   = NULL; // "正在播放" 小标题
static lv_obj_t *lbl_track   = NULL; // 当前曲目名 (大号, 滚动)
static lv_obj_t *lbl_cur     = NULL; // 当前时间 (左)
static lv_obj_t *lbl_gear    = NULL; // 加速档位 (中部, 加速时显示)
static lv_obj_t *lbl_dur     = NULL; // 总时长 (右)
static lv_obj_t *bar_prog    = NULL; // 进度条
static lv_obj_t *lbl_percent = NULL; // 进度百分比
static lv_obj_t *lbl_hint    = NULL; // 底部按键提示
static lv_obj_t *reel_l      = NULL; // 磁带左卷轴装饰
static lv_obj_t *reel_r      = NULL; // 磁带右卷轴装饰

/* 图形电量 / 音量 / 快进退读秒 */
static lv_obj_t *batt_frame  = NULL; // 电量外框
static lv_obj_t *batt_fill   = NULL; // 电量填充
static lv_obj_t *batt_charge = NULL; // 充电标记 "充"
static lv_obj_t *vol_box     = NULL; // 音量容器 (右侧竖向: 扬声器图标 + 竖向音量条)
static lv_obj_t *vol_spk_box = NULL; // 扬声器箱体 (小矩形)
static lv_obj_t *vol_cone    = NULL; // 扬声器锥体 (三角线, 指向右)
static lv_obj_t *vol_lvl     = NULL; // 竖向音量条 (level 0..14)
static lv_obj_t *lbl_seek    = NULL; // 快进/快退 读秒（居中醒目）

/* TF 卡状态图标与插拔提示 */
static lv_obj_t *sd_icon_box = NULL;  // TF 卡图标: 卡片外形 (青色=在位, 灰=弹出)
static lv_obj_t *sd_icon_lbl = NULL;  // "TF" 文字
static lv_obj_t *lbl_sd_toast = NULL; // 插拔瞬时提示 (居中, 由 LVGL 定时器控制显隐)
static int64_t   g_sd_toast_until = 0;

static int64_t   g_vol_hide_until = 0; // 音量条自动隐藏截止时间 (停止调节 3s 后隐藏)

static int s_play_mode = 0;          // 0=SEQ 1=ALL 2=ONE

/* ============================================================
 * 背光 PWM (IO15, LEDC)
 * ============================================================ */
#define LCD_BLK_LEDC_CH     LEDC_CHANNEL_0
#define LCD_BLK_LEDC_TIMER  LEDC_TIMER_0
#define LCD_BLK_PWM_HZ      5000
#define LCD_BLK_PWM_BITS    10
static int s_last_brightness = 100;

static void lcd_backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)LCD_BLK_PWM_BITS,
        .timer_num = LCD_BLK_LEDC_TIMER,
        .freq_hz = LCD_BLK_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = DISPLAY_BLK_IO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BLK_LEDC_CH,
        .timer_sel = LCD_BLK_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
}

void display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_last_brightness = percent;
    uint32_t duty = (uint32_t)((percent * ((1 << LCD_BLK_PWM_BITS) - 1)) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BLK_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BLK_LEDC_CH);
}

/* ============================================================
 * LCD 软件电源开关 (IO39, PMOS 低电平导通)
 * ============================================================ */
void display_power(bool on)
{
    gpio_set_level(LCD_POW_EN_IO, on ? 0 : 1);
}

/* ============================================================
 * LVGL flush 回调：把像素图写到 ST7789 帧
 * ============================================================ */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t *color = (uint16_t *)px_map;
    esp_lcd_panel_draw_bitmap(s_panel_handle,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color);
    lv_display_flush_ready(disp);
}

/* LVGL 心跳 + 定时器处理任务 */
static void lvgl_task(void *arg)
{
    (void)arg;
    while (1) {
        lv_tick_inc(5);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ============================================================
 * esp_lcd 硬件初始化 (SPI3 + ST7789)
 * ============================================================ */
static esp_err_t lcd_hw_init(void)
{
    /* LCD 软电源: 拉低导通 */
    gpio_set_direction(LCD_POW_EN_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_POW_EN_IO, 0);

    /* SPI3 总线 (独立于 SD 的 SPI2) */
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_IO,
        .miso_io_num = -1,            // TFT 仅写, 不用 MISO
        .sclk_io_num = DISPLAY_SCLK_IO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 40 * 2,
    };
    esp_err_t ret = spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: 0x%x", ret);
        return ret;
    }

    /* esp_lcd_spi IO 层: DC=GPIO16, CS 接地(-1) */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num        = -1,
        .dc_gpio_num        = DISPLAY_DC_IO,
        .spi_mode           = 0,
        .pclk_hz            = 40 * 1000 * 1000,
        .trans_queue_depth  = 10,
        .on_color_trans_done = NULL,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST,
                                   &io_cfg, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: 0x%x", ret);
        return ret;
    }

    /* 原生 ST7789 面板驱动 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISPLAY_RESET_IO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(s_io_handle, &panel_cfg, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);

    /* 方向: 由 DISPLAY_ORIENTATION 决定, 应用层无需改写 */
#if DISPLAY_ORIENTATION == 1
    esp_lcd_panel_swap_xy(s_panel_handle, false);
    esp_lcd_panel_mirror(s_panel_handle, false, false);
#else
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, false, true);
#endif
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    return ESP_OK;
}

/* ============================================================
 * LVGL UI 构建与模式切换
 * ============================================================ */
static void ui_reel_create(lv_obj_t *parent, lv_obj_t **reel, lv_align_t align, int x)
{
    *reel = lv_obj_create(parent);
    lv_obj_set_size(*reel, 40, 40);
    lv_obj_align(*reel, align, x, 86);
    lv_obj_set_style_radius(*reel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(*reel, lv_color_hex(0x16203a), 0);
    lv_obj_set_style_border_color(*reel, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_border_width(*reel, 2, 0);
    lv_obj_set_style_pad_all(*reel, 0, 0);
    /* 以圆心为旋转中心 (transform_angle 单位为 0.1°) */
    lv_obj_set_style_transform_pivot_x(*reel, 20, 0);
    lv_obj_set_style_transform_pivot_y(*reel, 20, 0);
    lv_obj_clear_flag(*reel, LV_OBJ_FLAG_CLICKABLE);

    /* 中心孔 */
    lv_obj_t *hole = lv_obj_create(*reel);
    lv_obj_set_size(hole, 10, 10);
    lv_obj_center(hole);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hole, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_border_width(hole, 0, 0);
    lv_obj_clear_flag(hole, LV_OBJ_FLAG_CLICKABLE);

    /* 偏心标记点：旋转时可见 (模拟磁带卷轴绕线转动) */
    lv_obj_t *mark = lv_obj_create(*reel);
    lv_obj_set_size(mark, 7, 7);
    lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
}

/* 磁带卷轴旋转动画：根据播放状态决定方向与速度 */
static player_state_t s_reel_state = PLAYER_STATE_STOPPED;
static int            s_reel_gear  = 0;
static int32_t        s_reel_angle = 0;

static void reel_anim_cb(lv_timer_t *t)
{
    (void)t;
    int32_t delta = 0;
    switch (s_reel_state) {
    case PLAYER_STATE_PLAYING:      delta =  30; break; /* 正常：正向匀速 */
    case PLAYER_STATE_FAST_FORWARD: delta = 200 * (1 + s_reel_gear); break; /* 快进：更快正向 */
    case PLAYER_STATE_REWIND:       delta = -200 * (1 + s_reel_gear); break; /* 快退：反向 */
    default: delta = 0; break; /* 暂停 / 停止：静止 */
    }
    if (delta != 0) {
        s_reel_angle += delta;
        if (s_reel_angle >= 3600) s_reel_angle -= 3600;
        if (s_reel_angle < 0)     s_reel_angle += 3600;
        lv_obj_set_style_transform_angle(reel_l, s_reel_angle, 0);
        lv_obj_set_style_transform_angle(reel_r, s_reel_angle, 0);
    }
}

static void ui_create(void)
{
    const int W = DISPLAY_WIDTH, H = DISPLAY_HEIGHT, M = 8;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0e17), 0);

    g_player = lv_obj_create(scr);
    lv_obj_set_size(g_player, W, H);
    lv_obj_set_style_bg_color(g_player, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_border_width(g_player, 0, 0);
    lv_obj_set_style_pad_all(g_player, 0, 0);
    lv_obj_clear_flag(g_player, LV_OBJ_FLAG_SCROLLABLE);

    /* 磁带卷轴装饰 (左右两圆) */
    ui_reel_create(g_player, &reel_l, LV_ALIGN_TOP_LEFT,  M + 6);
    ui_reel_create(g_player, &reel_r, LV_ALIGN_TOP_RIGHT, -(M + 6));

    /* 状态栏: 左=状态/曲目/模式  右=图形电量+音量 */
    lbl_status = lv_label_create(g_player);
    lv_obj_set_pos(lbl_status, M, 6);
    lv_obj_set_width(lbl_status, W - 2 * M - 136);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_chinese_14, 0);

    /* 图形电量图标 (外框 + 填充 + 充电标记) */
    batt_frame = lv_obj_create(g_player);
    lv_obj_set_size(batt_frame, 30, 14);
    lv_obj_align(batt_frame, LV_ALIGN_TOP_RIGHT, -M, 8);
    lv_obj_set_style_bg_opa(batt_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_frame, lv_color_white(), 0);
    lv_obj_set_style_border_width(batt_frame, 2, 0);
    lv_obj_set_style_radius(batt_frame, 2, 0);
    lv_obj_set_style_pad_all(batt_frame, 0, 0);
    lv_obj_clear_flag(batt_frame, LV_OBJ_FLAG_CLICKABLE);
    batt_fill = lv_obj_create(batt_frame);
    lv_obj_set_size(batt_fill, 24, 10);
    lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_border_width(batt_fill, 0, 0);
    lv_obj_set_style_radius(batt_fill, 0, 0);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *batt_nub = lv_obj_create(g_player);   // 电池正极小柱
    lv_obj_set_size(batt_nub, 3, 6);
    lv_obj_align_to(batt_nub, batt_frame, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(batt_nub, lv_color_white(), 0);
    lv_obj_set_style_border_width(batt_nub, 0, 0);
    lv_obj_clear_flag(batt_nub, LV_OBJ_FLAG_CLICKABLE);
    /* 充电标记: 绿色闪电符号 (矢量线绘制, 不依赖字体字形) */
    batt_charge = lv_obj_create(g_player);
    lv_obj_set_size(batt_charge, 12, 16);
    lv_obj_align_to(batt_charge, batt_frame, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_set_style_bg_opa(batt_charge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(batt_charge, 0, 0);
    lv_obj_clear_flag(batt_charge, LV_OBJ_FLAG_CLICKABLE);
    static lv_point_precise_t bolt_pts[] = {
        {7, 0}, {2, 7}, {5, 7}, {3, 14}, {9, 6}, {6, 6}, {7, 0}
    };
    lv_obj_t *bolt = lv_line_create(batt_charge);
    lv_line_set_points(bolt, bolt_pts, 7);
    lv_obj_align(bolt, LV_ALIGN_TOP_LEFT, 1, 1);
    lv_obj_set_style_line_width(bolt, 2, 0);
    lv_obj_set_style_line_color(bolt, lv_color_hex(0x22c55e), 0); /* 绿色闪电 */
    lv_obj_set_style_line_rounded(bolt, true, 0);
    lv_obj_clear_flag(bolt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(batt_charge, LV_OBJ_FLAG_HIDDEN);

    /* 图形音量: 屏幕右侧竖向条 (扬声器图标 + 竖向音量条) */
    vol_box = lv_obj_create(g_player);
    lv_obj_set_size(vol_box, 18, 90);
    lv_obj_align(vol_box, LV_ALIGN_RIGHT_MID, -6, -18);  // 右侧竖向, 避开状态栏(上)与进度条(下)
    lv_obj_set_style_bg_opa(vol_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_box, 0, 0);
    lv_obj_clear_flag(vol_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(vol_box, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏, 调节音量时显示

    /* 扬声器图标 (箱体 + 锥体, 置于竖向条顶部作为标识) */
    vol_spk_box = lv_obj_create(vol_box);
    lv_obj_set_size(vol_spk_box, 6, 8);
    lv_obj_align(vol_spk_box, LV_ALIGN_TOP_LEFT, 3, 5);
    lv_obj_set_style_bg_color(vol_spk_box, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_border_width(vol_spk_box, 0, 0);
    lv_obj_set_style_radius(vol_spk_box, 1, 0);
    lv_obj_clear_flag(vol_spk_box, LV_OBJ_FLAG_CLICKABLE);

    static lv_point_precise_t cone_pts[] = {{0, 0}, {0, 8}, {7, 4}, {0, 0}};
    vol_cone = lv_line_create(vol_box);
    lv_line_set_points(vol_cone, cone_pts, 4);
    lv_obj_align(vol_cone, LV_ALIGN_TOP_LEFT, 9, 5);
    lv_obj_set_style_line_width(vol_cone, 2, 0);
    lv_obj_set_style_line_color(vol_cone, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_line_rounded(vol_cone, true, 0);
    lv_obj_clear_flag(vol_cone, LV_OBJ_FLAG_CLICKABLE);

    /* 竖向音量条 (填充自底向上 = 音量 level 0..VOLUME_LEVEL_MAX) */
    vol_lvl = lv_bar_create(vol_box);
    lv_obj_set_size(vol_lvl, 6, 64);
    lv_obj_align(vol_lvl, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_orientation(vol_lvl, LV_BAR_ORIENTATION_VERTICAL);
    lv_bar_set_range(vol_lvl, 0, VOLUME_LEVEL_MAX);
    lv_bar_set_value(vol_lvl, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol_lvl, lv_color_hex(0x16203a), 0);
    lv_obj_set_style_bg_color(vol_lvl, lv_color_hex(0x2dd4bf), LV_PART_INDICATOR);
    lv_obj_set_style_radius(vol_lvl, 3, 0);
    lv_obj_clear_flag(vol_lvl, LV_OBJ_FLAG_CLICKABLE);

    /* TF 卡图标 (状态栏: 插入=青色卡片, 弹出=灰) —— 位于充电标记左侧 */
    sd_icon_box = lv_obj_create(g_player);
    lv_obj_set_size(sd_icon_box, 24, 14);
    lv_obj_align_to(sd_icon_box, batt_charge, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_set_style_bg_color(sd_icon_box, lv_color_hex(0x33405e), 0);  // 默认弹出(灰)
    lv_obj_set_style_border_width(sd_icon_box, 0, 0);
    lv_obj_set_style_radius(sd_icon_box, 2, 0);
    lv_obj_set_style_pad_all(sd_icon_box, 0, 0);
    lv_obj_clear_flag(sd_icon_box, LV_OBJ_FLAG_CLICKABLE);
    /* 右上角缺角 (深色三角模拟 SD 卡斜切特征) */
    static lv_point_precise_t sd_notch[] = {{18, 0}, {24, 6}, {24, 0}, {18, 0}};
    lv_obj_t *sd_line = lv_line_create(sd_icon_box);
    lv_line_set_points(sd_line, sd_notch, 4);
    lv_obj_set_style_line_width(sd_line, 2, 0);
    lv_obj_set_style_line_color(sd_line, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_line_rounded(sd_line, false, 0);
    lv_obj_clear_flag(sd_line, LV_OBJ_FLAG_CLICKABLE);
    sd_icon_lbl = lv_label_create(sd_icon_box);
    lv_label_set_text(sd_icon_lbl, "");   // 默认弹出: 空
    lv_obj_center(sd_icon_lbl);
    lv_obj_set_style_text_color(sd_icon_lbl, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_text_font(sd_icon_lbl, &lv_font_chinese_12, 0);
    lv_obj_clear_flag(sd_icon_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* 插拔瞬时提示 (居中, 状态栏下方; 由 LVGL 定时器控制显隐) */
    lbl_sd_toast = lv_label_create(g_player);
    lv_obj_align(lbl_sd_toast, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_color(lbl_sd_toast, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_text_font(lbl_sd_toast, &lv_font_chinese_14, 0);
    lv_obj_add_flag(lbl_sd_toast, LV_OBJ_FLAG_HIDDEN);

    /* 正在播放 小标题 */
    lbl_title = lv_label_create(g_player);
    lv_obj_set_pos(lbl_title, M, 34);
    lv_label_set_text(lbl_title, "正在播放");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_chinese_12, 0);

    /* 文件名 (大号, 循环滚动) */
    lbl_track = lv_label_create(g_player);
    lv_obj_set_pos(lbl_track, M, 52);
    lv_obj_set_width(lbl_track, W - 2 * M);
    lv_label_set_long_mode(lbl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_track, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_track, &lv_font_chinese_16, 0);

    /* 时间行: 当前(左) / 档位(中) / 总时长(右) */
    lbl_cur = lv_label_create(g_player);
    lv_obj_set_pos(lbl_cur, M, 122);
    lv_obj_set_style_text_color(lbl_cur, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_cur, &lv_font_chinese_14, 0);

    lbl_gear = lv_label_create(g_player);
    lv_obj_set_pos(lbl_gear, W / 2 - 22, 122);
    lv_obj_set_style_text_color(lbl_gear, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_text_font(lbl_gear, &lv_font_chinese_14, 0);

    lbl_dur = lv_label_create(g_player);
    lv_obj_set_pos(lbl_dur, W - M, 122);
    lv_obj_set_style_text_align(lbl_dur, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_dur, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_dur, &lv_font_chinese_14, 0);

    /* 进度条 + 百分比 */
    bar_prog = lv_bar_create(g_player);
    lv_obj_set_size(bar_prog, W - 2 * M, 14);
    lv_obj_set_pos(bar_prog, M, 150);
    lv_bar_set_range(bar_prog, 0, 1000);
    lv_bar_set_value(bar_prog, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_prog, lv_color_hex(0x16203a), 0);
    lv_obj_set_style_bg_color(bar_prog, lv_color_hex(0x2dd4bf), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_prog, 4, 0);

    lbl_percent = lv_label_create(g_player);
    lv_obj_set_pos(lbl_percent, W - M, 168);
    lv_obj_set_style_text_align(lbl_percent, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_percent, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_percent, &lv_font_chinese_12, 0);

    /* 快进/快退 读秒提示（居中醒目，仅快进退时显示） */
    lbl_seek = lv_label_create(g_player);
    lv_obj_set_pos(lbl_seek, M, 180);
    lv_obj_set_width(lbl_seek, W - 2 * M);
    lv_obj_set_style_text_align(lbl_seek, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_seek, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_text_font(lbl_seek, &lv_font_chinese_16, 0);
    lv_obj_add_flag(lbl_seek, LV_OBJ_FLAG_HIDDEN);

    /* 底部按键提示 */
    lbl_hint = lv_label_create(g_player);
    lv_obj_set_pos(lbl_hint, M, H - 20);
    lv_obj_set_style_text_color(lbl_hint, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_hint, &lv_font_chinese_12, 0);

    /* 居中消息 (splash/提示/浏览) */
    g_msg = lv_label_create(scr);
    lv_label_set_long_mode(g_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_msg, W - 24);
    lv_obj_set_style_text_align(g_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_msg, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_msg, &lv_font_chinese_16, 0);
    lv_obj_align(g_msg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
}

static void ui_show_player(void)
{
    lv_obj_clear_flag(g_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
}

static void ui_show_msg(const char *txt)
{
    lv_label_set_text(g_msg, txt);
    lv_obj_clear_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_player, LV_OBJ_FLAG_HIDDEN);
}

void display_set_play_mode(int mode)
{
    s_play_mode = mode;
}

/* ============================================================
 * 脏区检查 / 屏保 (沿用原逻辑)
 * ============================================================ */
static uint32_t g_display_fp = 0;
static uint64_t g_display_last_update_us = 0;
#define SCREEN_SAVER_TIMEOUT_US  (30 * 1000000ULL)

static uint32_t calc_fingerprint(player_state_t state,
                                 int track_idx, int total,
                                 int current_sec, int total_sec,
                                 float speed, int gear, int volume)
{
    uint32_t h = (uint32_t)state;
    h = h * 31 + (uint32_t)track_idx;
    h = h * 31 + (uint32_t)total;
    h = h * 31 + (uint32_t)current_sec;
    h = h * 31 + (uint32_t)total_sec;
    h = h * 31 + (uint32_t)(int)(speed * 10);
    h = h * 31 + (uint32_t)gear;
    h = h * 31 + (uint32_t)volume;
    return h;
}

static const char *state_word(player_state_t state)
{
    switch (state) {
    case PLAYER_STATE_PLAYING:       return "播放中";
    case PLAYER_STATE_PAUSED:        return "已暂停";
    case PLAYER_STATE_STOPPED:       return "已停止";
    case PLAYER_STATE_FAST_FORWARD:  return "快进中";
    case PLAYER_STATE_REWIND:        return "快退中";
    default:                         return "未知";
    }
}

static const char *gear_str(int gear)
{
    static char s_gear_buf[8];
    float speed = tape_control_get_gear_speed(gear);
    if (speed <= 0.0f) return "";
    snprintf(s_gear_buf, sizeof(s_gear_buf), "%.1fx", speed);
    return s_gear_buf;
}

static void format_time(int seconds, char *buf, size_t size)
{
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0) snprintf(buf, size, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, size, "%02d:%02d", m, s);
}

/* ============================================================
 * 公共 API
 * ============================================================ */
static void sd_toast_timer_cb(lv_timer_t *t);  // 前向声明 (display_init 中注册)
static void vol_hide_timer_cb(lv_timer_t *t);   // 音量条自动隐藏定时器
void display_init(void)
{
    lcd_backlight_init();
    display_set_brightness(100);

    if (lcd_hw_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD hw init failed, display disabled");
        return;
    }

    lv_init();

    s_lv_buf = (lv_color_t *)heap_caps_malloc(DISPLAY_WIDTH * 40 * sizeof(lv_color_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lv_buf) {
        s_lv_buf = (lv_color_t *)heap_caps_malloc(DISPLAY_WIDTH * 40 * sizeof(lv_color_t),
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_lv_buf) {
        ESP_LOGE(TAG, "LVGL buffer alloc failed, display disabled");
        return;
    }

    lv_display_t *disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, s_lv_buf, NULL,
                           DISPLAY_WIDTH * 40 * sizeof(lv_color_t),
                           LV_DISP_RENDER_MODE_PARTIAL);

    ui_create();
    lv_timer_create(reel_anim_cb, 50, NULL);   // 磁带卷轴旋转动画 (50ms/帧)
    lv_timer_create(sd_toast_timer_cb, 100, NULL);  // 插拔提示显隐控制 (100ms)
    lv_timer_create(vol_hide_timer_cb, 200, NULL);  // 音量条停止调节 3s 后自动隐藏
    ui_show_msg("正在初始化...");

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);

    g_display_initialized = true;
    ESP_LOGI(TAG, "ST7789 + LVGL display initialized (%dx%d)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void display_show_splash(void)
{
    if (!g_display_initialized) return;
    ui_show_msg("有声书播放器\nESP32-S3\n正在加载 SD 卡");
}

void display_show_no_files(void)
{
    if (!g_display_initialized) return;
    ui_show_msg("未找到音频文件\n请将 .mp3/.flac/.wav\n拷贝到 SD 卡");
}

void display_show_no_card(void)
{
    if (!g_display_initialized) return;
    ui_show_msg("未检测到 SD 卡\n请插入 SD 卡");
}

static void sd_icon_update(bool present)
{
    if (!g_display_initialized) return;
    lv_color_t c = present ? lv_color_hex(0x2dd4bf) : lv_color_hex(0x33405e);
    lv_obj_set_style_bg_color(sd_icon_box, c, 0);
    lv_label_set_text(sd_icon_lbl, present ? "TF" : "");
}

/* 仅初始化图标状态, 不弹提示 (用于开机, 避免误报插拔) */
void display_set_sd_present_init(bool present)
{
    sd_icon_update(present);
}

/* 设置 TF 卡在位状态: 更新图标 + 弹插拔瞬时提示 */
void display_set_sd_present(bool present)
{
    sd_icon_update(present);
    lv_label_set_text(lbl_sd_toast, present ? "已插入 TF 卡" : "已弹出 TF 卡");
    g_sd_toast_until = esp_timer_get_time() + 1500 * 1000;  // 显示 1.5s
}

/* 插拔瞬时提示显隐控制 (独立于 display_update 的脏区判定, 保证提示及时出现/消失) */
static void sd_toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_display_initialized || !lbl_sd_toast) return;
    if (esp_timer_get_time() < g_sd_toast_until) {
        lv_obj_clear_flag(lbl_sd_toast, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_sd_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 音量条自动隐藏: 停止调节 3 秒后隐藏 (由 vol_hide_timer_cb 每 200ms 检查) */
static void vol_hide_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_display_initialized || !vol_box) return;
    if (esp_timer_get_time() >= g_vol_hide_until) {
        lv_obj_add_flag(vol_box, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 触发音量条显示: 立即刷新音量条与扬声器图标, 并重置 3 秒隐藏计时 */
void display_show_volume(int volume)
{
    if (!g_display_initialized || !vol_box) return;
    /* 立即刷新 (不依赖周期刷新, 保证出现即最新) */
    lv_color_t vol_col = (volume <= 0) ? lv_color_hex(0x33405e) : lv_color_hex(0x2dd4bf);
    lv_obj_set_style_bg_color(vol_spk_box, vol_col, 0);
    lv_obj_set_style_line_color(vol_cone, vol_col, 0);
    int vol_clamped = volume < 0 ? 0 : (volume > VOLUME_LEVEL_MAX ? VOLUME_LEVEL_MAX : volume);
    lv_bar_set_value(vol_lvl, vol_clamped, LV_ANIM_OFF);
    /* 显示 + 重置 3 秒隐藏倒计时 */
    lv_obj_clear_flag(vol_box, LV_OBJ_FLAG_HIDDEN);
    g_vol_hide_until = esp_timer_get_time() + 3000 * 1000;
}

void display_update(player_state_t state,
                    const char *track_name,
                    int track_idx, int total,
                    int current_sec, int total_sec,
                    float speed, int gear, int volume)
{
    if (!g_display_initialized) return;

    /* 驱动磁带卷轴动画 (方向/速度由状态与档位决定) */
    s_reel_state = state;
    s_reel_gear  = gear;

    uint64_t now = esp_timer_get_time();

    uint32_t fp = calc_fingerprint(state, track_idx, total,
                                   current_sec, total_sec, speed, gear, volume);
    fp = fp * 31 + (uint32_t)s_play_mode;   // 播放模式变化也触发刷新
    if (fp == g_display_fp) {
        if (!g_display_sleep &&
            (now - g_display_last_update_us) >= SCREEN_SAVER_TIMEOUT_US) {
            display_set_brightness(0);
            g_display_sleep = true;
        }
        return;
    }
    g_display_fp = fp;
    g_display_last_update_us = now;

    if (g_display_sleep) {
        display_set_brightness(s_last_brightness);
        g_display_sleep = false;
    }

    ui_show_player();

    /* 时间字符串先算好（时间行与读秒共用） */
    char cur[16], tot[16];
    format_time(current_sec, cur, sizeof(cur));
    format_time(total_sec, tot, sizeof(tot));

    /* 状态栏: 状态 · 曲目 x/y · [模式]（电量/音量已改为图形） */
    const char *mode_s = (s_play_mode == 1) ? "列表循环" : (s_play_mode == 2) ? "单曲循环" : "顺序播放";
    char line0[64];
    snprintf(line0, sizeof(line0), "%s %03d/%03d %s",
             state_word(state), track_idx, total, mode_s);
    lv_label_set_text(lbl_status, line0);

    /* 图形电量: 外框填充宽度 + 低电量变红 + 充电标记 */
    int bp = power_mgmt_get_battery_percent();
    int bw = (bp * 24) / 100;
    if (bw > 24) bw = 24;
    if (bw < 0) bw = 0;
    lv_obj_set_width(batt_fill, bw);
    lv_obj_set_style_bg_color(batt_fill, bp < 20 ? lv_color_hex(0xef4444) : lv_color_hex(0x2dd4bf), 0);
    if (power_mgmt_is_charging()) lv_obj_clear_flag(batt_charge, LV_OBJ_FLAG_HIDDEN);
    else                          lv_obj_add_flag(batt_charge, LV_OBJ_FLAG_HIDDEN);

    /* 图形音量: 扬声器随静音变灰 + 右侧竖向音量条填充=音量 level */
    lv_color_t vol_col = (volume <= 0) ? lv_color_hex(0x33405e) : lv_color_hex(0x2dd4bf);
    lv_obj_set_style_bg_color(vol_spk_box, vol_col, 0);
    lv_obj_set_style_line_color(vol_cone, vol_col, 0);
    int vol_clamped = volume < 0 ? 0 : (volume > VOLUME_LEVEL_MAX ? VOLUME_LEVEL_MAX : volume);
    lv_bar_set_value(vol_lvl, vol_clamped, LV_ANIM_OFF);

    /* 文件名 (超长省略号截断, 静态不滚动) */
    lv_label_set_text(lbl_track, track_name ? track_name : "");

    /* 时间(左) / 档位(中) / 总时长(右) */
    lv_label_set_text(lbl_cur, cur);
    lv_label_set_text(lbl_dur, tot);
    const char *gs = gear_str(gear);
    lv_label_set_text(lbl_gear, (gs && gs[0]) ? gs : "");

    /* 进度条 + 百分比 */
    int v = 0;
    if (total_sec > 0) {
        v = (int)((int64_t)current_sec * 1000 / total_sec);
        if (v < 0) v = 0;
        if (v > 1000) v = 1000;
    }
    lv_bar_set_value(bar_prog, v, LV_ANIM_OFF);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", v / 10);
    lv_label_set_text(lbl_percent, pct);

    /* 快进/快退 读秒（居中醒目，显示跳转方向与当前位置） */
    if (state == PLAYER_STATE_FAST_FORWARD || state == PLAYER_STATE_REWIND) {
        const char *dir = (state == PLAYER_STATE_FAST_FORWARD) ? "快进" : "快退";
        const char *arrow = (state == PLAYER_STATE_FAST_FORWARD) ? "->" : "<-";
        const char *gs2 = gear_str(gear);
        char seekbuf[32];
        snprintf(seekbuf, sizeof(seekbuf), "%s %s %s %s",
                 dir, (gs2 && gs2[0]) ? gs2 : "", arrow, cur);
        lv_label_set_text(lbl_seek, seekbuf);
        lv_obj_clear_flag(lbl_seek, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_seek, LV_OBJ_FLAG_HIDDEN);
    }

    /* 底部按键提示 (按物理按键左→右顺序: 走带四键 ｜ 选曲两键) */
    lv_label_set_text(lbl_hint, "快退 播放 快进 停止 ｜ 上一首 下一首");
}

void display_show_browse(int selected, int total, char lines[][24], int count)
{
    if (!g_display_initialized || count <= 0) return;

    static char buf[256];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "浏览  %d/%d\n",
                    selected + 1, total);
    int shown = count;
    if (shown > BROWSE_VISIBLE_LINES) shown = BROWSE_VISIBLE_LINES;
    for (int i = 0; i < shown; i++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s\n", lines[i]);
    }
    len += snprintf(buf + len, sizeof(buf) - len,
                    "\n上翻页/下翻页 翻页  上一首/下一首 移动  确认 播放  退出 停止");
    ui_show_msg(buf);
}

void display_show_menu(const char *title, char lines[][24], int count, int sel, const char *hint)
{
    (void)sel;
    if (!g_display_initialized || count <= 0) return;

    static char buf[256];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "%s\n",
                    (title && title[0]) ? title : "菜单");

    int shown = count;
    if (shown > BROWSE_VISIBLE_LINES) shown = BROWSE_VISIBLE_LINES;
    for (int i = 0; i < shown; i++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s\n", lines[i]);
    }
    if (hint) {
        len += snprintf(buf + len, sizeof(buf) - len, "\n%s", hint);
    }

    /* 菜单为活跃界面, 唤醒背光 */
    if (g_display_sleep) {
        display_set_brightness(s_last_brightness);
        g_display_sleep = false;
    }
    ui_show_msg(buf);
}

void display_show_info(const char *title, const char *text)
{
    if (!g_display_initialized) return;

    static char buf[256];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "%s\n\n",
                    (title && title[0]) ? title : "信息");
    if (text) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s", text);
    }
    len += snprintf(buf + len, sizeof(buf) - len, "\n\n（按任意键返回）");

    if (g_display_sleep) {
        display_set_brightness(s_last_brightness);
        g_display_sleep = false;
    }
    ui_show_msg(buf);
}
