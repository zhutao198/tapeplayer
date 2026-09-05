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
#include "font_partition.h"
#include "audio_player.h"
#include "esp_timer.h"
#include "reel_img.h"
#include "cassette_bg.h"   /* P1-UI: 盒壳静态背景 */  // R109c: 预烘焙红轮毂位图 (带 6 辐条, 旋转只转 1 个 img 对象)

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"
#include "cjk_font.h"   // 点阵字库 (simsun 16x16, 绕开 LVGL 中文字体路径)

/* 自定义中文字体（ui_font_*.c 生成）：显式声明供本编译单元使用 */
LV_FONT_DECLARE(lv_font_montserrat_14);
/* R102-R106 结论: 在 ESP32-S3 该硬件上, LVGL 中文子集字体 (lv_font_chinese_*) 即使
   仅渲染 ASCII 字符, 实际表现为"二维码"乱码/崩溃 (R102 教训: 其 fallback 指向
   freetype TTF, 在 PSRAM + Cache 约束下不稳定)。
   因此: 播放界面所有纯 ASCII 文案 (状态/时间/进度/档位/提示/SD toast/消息)
   一律改用 lv_font_montserrat_14 (纯 ASCII, 无 freetype fallback, 渲染稳定)。
   真正的中文显示走方案 C: 点阵 (cjk_font.h) -> lv_canvas -> LVGL flush,
   由 display_show_menu / s_track_canvas 负责, 不经本 UI_FONT 路径。 */
LV_FONT_DECLARE(lv_font_chinese_12);
LV_FONT_DECLARE(lv_font_chinese_14);
LV_FONT_DECLARE(lv_font_chinese_16);
#define UI_FONT (&lv_font_montserrat_14)

static const char *TAG = "display";

/* R102: 菜单点阵绘制缓存 (绕开 LVGL 中文路径, 最小验证) */
static bool   s_menu_visible = false;
static char   s_menu_title[64];
static char   s_menu_lines[BROWSE_VISIBLE_LINES][24];
static int    s_menu_count = 0;
static int    s_menu_sel = 0;
static char   s_menu_hint[64];

/* R098f: display_init 提前到本文件靠前位置, 其依赖的下列函数定义在文件后段,
   此处补前向声明以满足编译 (均为本编译单元内函数)。 */
static esp_err_t        lcd_hw_init(void);
static void             lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px);
static void             ui_create(void);
/* reel 动画状态 (P1-fix2: 移到此处供 lvgl_task 引用) */
static player_state_t s_reel_state = PLAYER_STATE_STOPPED;
static int            s_reel_gear  = 0;
static int            s_reel_frame = 0;
static uint64_t       s_reel_last_us = 0;
static void             sd_toast_timer_cb(lv_timer_t *t);
static void             vol_hide_timer_cb(lv_timer_t *t);
/* P1-fix: display_update 实际执行体 (lvgl_task 持锁区内调用) */
static void display_update_nolock(player_state_t state, const char *track_name,
                                  int track_idx, int total,
                                  int current_sec, int total_sec,
                                  float speed, int gear, int volume);

/* R102-fix: 菜单真正的 LVGL 绘制(仅供 lvgl_task 持锁区内调用, 前向声明见 menu_apply_nolock) */
static void             menu_apply_nolock(void);
/* R103: 点阵 canvas (LVGL 原生位图路径), 在 display_init 中 ui_create 之后调用 */
static void             cjk_canvas_init(void);
static void             ui_show_msg(const char *msg);

static bool g_display_initialized = false;
static bool g_display_sleep       = false;

/* esp_lcd 句柄 */
static esp_lcd_panel_io_handle_t s_io_handle    = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;

/* LVGL 刷新缓冲 (优先 PSRAM) */
static lv_color_t *s_lv_buf1 = NULL;
static lv_color_t *s_lv_buf2 = NULL;
static lv_display_t *s_flush_disp = NULL;

/* R102: 点阵 shadow 帧缓冲 + 覆盖掩码 (PSRAM), 避免访问 LVGL draw_buf 内部 + 避免 SPI 重入崩溃 */
static uint16_t *s_cjk_fb   = NULL;   // RGB565, 全屏 [H][W]
static uint8_t  *s_cjk_mask = NULL;   // 1=该像素有点阵需覆盖

/* R103: 点阵 canvas —— LVGL 原生位图路径 (方案 C)
 * 前两个落点均已被证伪:
 *   (a) flush 外直接 esp_lcd_panel_draw_bitmap  -> BREAK 崩溃 (抢 SPI 总线)
 *   (b) flush 内覆盖 px_map (shadow 帧缓冲+掩码) -> TG1WDT 反复重启
 * 方案 C: 把点阵画进 lv_canvas 的 buffer (纯内存写), 由 LVGL 当作普通图像
 *   对象 flush 出去 —— 不碰 SPI / 不碰 draw_buf 内部 / 不额外调 draw_bitmap。
 * 字节序: canvas buffer 存 LVGL **原生** RGB565 (不做 SWAP16);
 *         flush_cb 会对整帧统一 SWAP16, 与 LVGL 其它内容保持一致。 */
static lv_obj_t *s_cjk_canvas     = NULL;
static uint16_t *s_cjk_canvas_buf = NULL;
#define CJK_CANVAS_W  (DISPLAY_WIDTH)
#define CJK_CANVAS_H  (DISPLAY_HEIGHT)

/* R105: 曲名专用小 canvas —— 只覆盖曲名那一条区域, 避免遮挡 player 屏的
   其它 LVGL 元素(状态栏/进度条/时间等)。全屏 canvas 仅用于菜单/browse 独占态。
   坐标与 lbl_track 保持一致 (ui_create 中 lbl_track 位于 M=8, y=56; R108 下移以容纳格式行)。 */
static lv_obj_t *s_track_canvas     = NULL;
static uint16_t *s_track_canvas_buf = NULL;
#define TRACK_CANVAS_X  (8)
#define TRACK_CANVAS_Y  (56)
#define TRACK_CANVAS_W  (DISPLAY_WIDTH - 2 * 8)   /* 304 */
#define TRACK_CANVAS_H  (18)

/* LVGL UI 对象 */
static lv_obj_t *g_player = NULL;   // 播放界面容器
static lv_obj_t *g_msg    = NULL;   // 居中消息标签 (splash/提示)

/* SD-OTA 升级界面 (R049c 真实化) */
static lv_obj_t *g_ota     = NULL;  // 升级界面容器
static lv_obj_t *ota_title = NULL;  // 标题

/* R051：A-B 复读状态屏（带迷你进度条） */
static lv_obj_t *g_ab_menu   = NULL;
static lv_obj_t *abm_title   = NULL;
static lv_obj_t *abm_bar     = NULL;
static lv_obj_t *abm_mark_a  = NULL;
static lv_obj_t *abm_mark_b  = NULL;
static lv_obj_t *abm_stat    = NULL;
static lv_obj_t *abm_lines[4]= {NULL, NULL, NULL, NULL};
static lv_obj_t *abm_hint    = NULL;
static lv_obj_t *ota_body  = NULL;  // 正文(摘要/状态, 多行)
static lv_obj_t *ota_bar   = NULL;  // 写入进度条
static lv_obj_t *ota_pct   = NULL;  // 进度百分比
static lv_obj_t *ota_hint  = NULL;  // 底部操作提示

static lv_obj_t *lbl_status  = NULL; // 状态栏: 播放态/曲目/模式/电池/音量
static lv_obj_t *lbl_title   = NULL; // "正在播放" 小标题
static lv_obj_t *lbl_track   = NULL; // 当前曲目名 (大号, 滚动)
static lv_obj_t *lbl_fmt     = NULL; // R108: 格式/品牌行 (FLAC|44KHZ|16bit|0918kbps SQ)
static lv_obj_t *lbl_cur     = NULL; // 当前时间 (左)
static lv_obj_t *lbl_gear    = NULL; // 加速档位 (中部, 加速时显示)
static lv_obj_t *lbl_dur     = NULL; // 总时长 (右)
static lv_obj_t *bar_prog    = NULL; // 进度条
static lv_obj_t *lbl_percent = NULL; // 进度百分比
static lv_obj_t *lbl_hint    = NULL; // 底部按键提示
/* P2: 底部6键指示条 */
typedef struct { lv_obj_t *btn; lv_obj_t *icon; lv_obj_t *lab; } key_btn_t;
static key_btn_t s_keys[6];
static const char *KEY_ICONS[6] = {"<<", ">", ">>", "[]", "<|", "|>"};
static const char *KEY_LABELS[6] = {"快退", "播放", "快进", "停止", "上首", "下首"};
static lv_obj_t *s_play_tri = NULL;
static lv_obj_t *s_pause_l = NULL;
static lv_obj_t *s_pause_r = NULL;
static lv_obj_t *lbl_ab      = NULL; // A-B 复读信息（底部灰栏）
static lv_obj_t *ab_mark_a   = NULL; // 进度条上的 A 点标记
static lv_obj_t *ab_mark_b   = NULL; // 进度条上的 B 点标记
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
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = (ledc_timer_bit_t)LCD_BLK_PWM_BITS;
    t.timer_num = LCD_BLK_LEDC_TIMER;
    t.freq_hz = LCD_BLK_PWM_HZ;
    t.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {};
    ch.gpio_num = DISPLAY_BLK_IO;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LCD_BLK_LEDC_CH;
    ch.timer_sel = LCD_BLK_LEDC_TIMER;
    ch.duty = 0;
    ch.hpoint = 0;
    ledc_channel_config(&ch);
}

void display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_last_brightness = percent;
    uint32_t duty = (uint32_t)((percent * ((1 << LCD_BLK_PWM_BITS) - 1)) / 100);
    ESP_LOGI(TAG, "DBG: set_brightness percent=%d duty=%lu max=%lu", percent, duty, (1u << LCD_BLK_PWM_BITS) - 1);
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
 * Main tick 注册回调（消除 main_task 与 lvgl_task 并发访问 LVGL 死锁）
 * - main 不再直接调 display_* 系列 LVGL API
 * - main 调用 display_request_main_tick() 置 dirty
 * - lvgl_task 在自己持锁区间内调注册的 main tick 回调
 * ============================================================ */
static volatile bool     s_main_tick_pending = false;
static display_main_tick_fn_t s_main_tick_cb = NULL;
/* R056-fix4: 音量条轮询方案(替代 lv_async_call,避免队列堆积→状态损坏→Guru Meditation)
   main_task 只设标志,lvgl_task 在持锁状态下消费 */
static volatile bool s_vol_pending = false;
static volatile int  s_vol_value   = 0;
static volatile uint32_t s_main_tick_call_count = 0;  /* 诊断：调用次数 */

/* R063-fix: 弹窗消息(show_no_card/show_no_files/show_splash)轮询方案,
   与音量条同源——main_task 只设标志,lvgl_task 在持锁状态下调 LVGL,
   避免 main 直接调 ui_show_msg(裸 LVGL API) 与 lvgl_task 死锁→TWDT(R062 死锁回退)。 */
static volatile bool s_msg_pending = false;

/* P1-fix: display_update flag-consumption cache (main->lvgl_task, 消除跨任务锁竞争) */
typedef struct {
    player_state_t state;
    char track_name[FILENAME_MAX_LEN];
    int track_idx, total, current_sec, total_sec;
    float speed;
    int gear, volume;
} disp_update_cache_t;
static disp_update_cache_t s_disp_cache;
static volatile bool s_disp_update_pending = false;

/* P1-fix: 硬件 esp_timer 驱动 LVGL tick (不再依赖 lvgl_task 循环的 lv_tick_inc(5)) */
static esp_timer_handle_t s_lv_tick_timer = NULL;
static void lv_tick_esp_cb(void *arg) { (void)arg; lv_tick_inc(1); }
static char          s_msg_text[96] = {0};
/* R100: SD 图标/插拔提示/清消息返回播放器——全部由 lvgl 任务消费,
   main_task 只设标志,彻底避免 main 调 LVGL 与 lvgl_task 死锁(拔卡死机根因)。 */
static volatile bool s_sd_icon_pending  = false;   /* SD 图标+插拔提示待更新 */
static volatile bool s_sd_icon_present   = false;   /* 期望的卡在位状态 */
static volatile bool s_clear_msg_pending = false;   /* 清全屏消息返回播放器 */
/* R102-fix: 菜单绘制标志位化。
   原因: display_show_menu 有两条调用路径——
     (a) display_update()  : main 任务, 但已持 lv_lock -> 安全
     (b) handle_button_events -> menu_open -> menu_render : main 任务, **不持锁**
   路径 (b) 直接调 lv_obj_add_flag/invalidate 会与 CPU1 的 lvgl_task 并发竞争,
   损坏 LVGL 内部结构 -> lv_refr/lv_obj_pos 遍历死循环 -> main 卡 30s -> task_wdt。
   这与 R063/R097/R100 的"main_task 完全不碰 LVGL"是同一类问题, 故沿用同一范式:
   main 只设标志, 真正的 LVGL 操作由 lvgl_task 在持锁区内执行。 */
static volatile bool s_menu_pending       = false;  /* 菜单内容待绘制 */
static volatile bool s_menu_close_pending = false;  /* 菜单关闭待处理(恢复 player) */

void display_register_main_tick(display_main_tick_fn_t fn)
{
    s_main_tick_cb = fn;
    ESP_LOGI(TAG, "DBG: display_register_main_tick fn=%p", (void *)fn);
}

/* R098f: 在 display_init 末尾启动 LVGL 渲染任务。
 * 之前版本 display_start_lvgl_task() 未被任何地方调用，导致 lvgl_task 任务从未创建，
 * 屏幕只停在上电残留帧（花屏），UI 不刷新。这里统一在 init 末尾拉起渲染任务。 */
void display_init(void)
{
    lcd_backlight_init();
    display_set_brightness(100);

    if (lcd_hw_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD hw init failed, display disabled");
        return;
    }

    lv_init();

    /* 挂载独立字库分区并初始化中文回退字体（菜单白字 + 任意中文文件名） */
    font_partition_init();

    /* 部分刷新缓冲: 每行块 <= SPI 事务上限, 避免整屏一次性 draw_bitmap 导致花屏
       ESP32-S3 SPI 单次事务上限约 32KB, 这里用 40 行(竖屏 240*40*2=19200 字节)留有余量 */
    const size_t buf_lines = 40;
    const size_t buf_px = DISPLAY_WIDTH * buf_lines;
    const size_t buf_sz = buf_px * sizeof(lv_color_t);
    s_lv_buf1 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lv_buf2 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lv_buf1 || !s_lv_buf2) {
        ESP_LOGW(TAG, "PSRAM dual-buffer partial, fallback to DRAM");
        if (!s_lv_buf1) s_lv_buf1 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_lv_buf2) s_lv_buf2 = (lv_color_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_lv_buf1 || !s_lv_buf2) {
        ESP_LOGE(TAG, "LVGL dual-buffer alloc failed, display disabled");
        return;
    }

    /* R102: 点阵 shadow 帧缓冲 + 掩码 (全屏, PSRAM) */
    s_cjk_fb = (uint16_t *)heap_caps_malloc((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cjk_fb) s_cjk_fb = (uint16_t *)heap_caps_malloc((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
                                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_cjk_mask = (uint8_t *)heap_caps_malloc((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cjk_mask) s_cjk_mask = (uint8_t *)heap_caps_malloc((size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT,
                                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_cjk_fb) memset(s_cjk_fb, 0, (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
    if (s_cjk_mask) memset(s_cjk_mask, 0, (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT);

    lv_display_t *disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, s_lv_buf1, NULL,   /* 回退单缓冲排查卡顿 */
                           buf_sz,
                           LV_DISP_RENDER_MODE_PARTIAL);

    ui_create();
    cjk_canvas_init();   /* R103: 在 ui_create 之后创建, 保证 canvas 位于最顶层可覆盖其它屏 */
    /* P1-fix2: reel 动画改由 lvgl_task 循环直接驱动 (硬件时间戳, 绕过 LVGL timer 调度抖动) */
    lv_timer_create(sd_toast_timer_cb, 100, NULL);  // 插拔提示显隐控制 (100ms)
    lv_timer_create(vol_hide_timer_cb, 200, NULL);  // 音量条停止调节 3s 后自动隐藏
    display_mem_report();                            // 启动即打印一次内存水位
    ESP_LOGI(TAG, "DBG: mem report done");

    /* freetype 可能未就绪（FT_Init_FreeType 失败），此时渲染中文会误入 freetype
     * 路径死循环 + task_wdt；故首屏消息也用 ASCII 兜底，与 splash 保持一致 */
    ESP_LOGI(TAG, "DBG: show msg ascii");
    ui_show_msg("Initializing...");
    ESP_LOGI(TAG, "DBG: ui_show_msg done");

    g_display_initialized = true;
    ESP_LOGI(TAG, "ST7789 + LVGL display initialized (%dx%d)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT);
    ESP_LOGI(TAG, ">>> SYSTEM DISPLAY READY <<<");

    display_register_main_tick(NULL);
    display_start_lvgl_task();   /* 关键：拉起渲染任务，否则屏幕不刷新 */
    /* P1-fix: 硬件 1ms 定时器驱动 LVGL tick, 消除任务调度导致的时钟漂移 */
    if (!s_lv_tick_timer) {
        esp_timer_create_args_t targs = {};
        targs.callback = lv_tick_esp_cb;
        targs.name = "lv_tick";
        if (esp_timer_create(&targs, &s_lv_tick_timer) == ESP_OK) {
            esp_timer_start_periodic(s_lv_tick_timer, 1000);
            ESP_LOGI(TAG, "lv_tick esp_timer started @1ms");
        }
    }
}

void display_request_main_tick(void)
{
    s_main_tick_pending = true;
}

/* ============================================================
 * LVGL flush 回调：把像素图写到 ST7789 帧
 * ============================================================ */
/* R102: 点阵 overlay (写进 shadow 帧缓冲, 由 flush 按脏区覆盖刷出), 前向声明 */
static void cjk_flush_overlay(void);
static int s_flush_cnt = 0;
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t *color = (uint16_t *)px_map;
    s_flush_disp = disp;
    s_flush_cnt++;

    /* 方向 (MADCTL) 与 BGR 由 display_init 一次性设置 (swap_xy/mirror/rgb_ele_order)
     * + 原生 ST7789 驱动自动下发, 此处不再每帧重复写, 避免覆盖与撕裂。
     *
     * 字节序补偿 (little-endian RAMCTRL):
     *   本模组硬件 GRAM 实测按 little-endian 取数, 而 LVGL 送出的 RGB565 是
     *   big-endian (高字节在前)。若不做交换, 每像素高低字节颠倒 -> "白中带彩"。
     *   诊断 (display_diag_run) 已验证: 软件 SWAP16 每像素后送显, 颜色纯正。
     *   注意: lv_conf.h 的 LV_COLOR_16_SWAP 在本工程中未真正生效 (sdkconfig 无此项,
     *   main/lv_conf.h 未被 LVGL 组件包含), 故必须在此处显式软件交换, 保证 100% 生效。 */
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    const int n = w * h;
    /* R102-revert: 临时移除点阵 shadow 覆盖, 验证反复重启是否由点阵帧缓冲机制引起
       (崩溃类型 rst:0x8 TG1WDT 指向 flush 内 SPI 传输死锁/越界)。先恢复纯净 flush。 */
    for (int i = 0; i < n; i++) {
        uint16_t v = color[i];
        color[i] = (uint16_t)((v << 8) | (v >> 8));   /* SWAP16: 补偿 little-endian GRAM */
    }

    /* P0-2: esp_lcd_panel_io_spi 不会按 max_transfer_sz 自动分片, 需手动切。
       ESP32-S3 SPI DMA 单次事务安全上限约 32752 字节, 每块按此上限横向切。 */
    const int max_bytes = 32752;    const int bytes_per_row = w * (int)sizeof(lv_color_t);
    int rows_per_chunk = max_bytes / bytes_per_row;   // 每块行数
    if (rows_per_chunk < 1) {
        /* R050: 防御 rows_per_chunk=0 死循环 (w 过大时整行超 DMA 上限)。
           改为按 1 行处理, 并从该行重新分块, 避免 y 不前进的死循环。 */
        ESP_LOGW(TAG, "DBG: flush#%d rows_per_chunk=0 (w=%d), clamp to 1 row", s_flush_cnt, w);
        rows_per_chunk = 1;
    }
    int y = area->y1;
    while (y <= area->y2) {
        int y_end = (y + rows_per_chunk - 1);
        if (y_end > area->y2) y_end = area->y2;
        esp_lcd_panel_draw_bitmap(s_panel_handle,
                                  area->x1, y,
                                  area->x2 + 1, y_end + 1,
                                  &color[(y - area->y1) * w]);
        y = y_end + 1;
    }
    /* 方向 A：恢复真实写屏（之前 #if 0 诊断开关已移除，见 DEBUG-0820 / R049）*/
    /* R102: 点阵已写进 LVGL 全屏帧缓冲(b->data), 随本帧 flush 正常刷出, 零 SPI 重入 */
    lv_display_flush_ready(disp);   /* 阻塞 flush: SPI draw_bitmap 已完成 */
}

/* ============================================================
 * 点阵中文文本绘制层 (R102: 绕开 LVGL 中文字体)
 *   - 绝不在 LVGL flush 之外直接调 esp_lcd_panel_draw_bitmap (会抢 SPI 总线导致
 *     BREAK/Cache-error 崩溃). 改为: 调用方用 cjk_request_text() 把待绘文本入队,
 *     由 lvgl_flush_cb 在本帧写屏完成后统一绘制 (此时 LVGL 持有 SPI 总线, 安全).
 *   - 屏幕 GRAM 为 little-endian, 与 flush_cb 一致需 SWAP16
 * ============================================================ */
#define CJK_Q_MAX 8
#define CJK_Q_STR 64
typedef struct { int x, y; uint16_t fg, bg; char s[CJK_Q_STR]; } cjk_req_t;
static cjk_req_t s_cjk_q[CJK_Q_MAX];
static int       s_cjk_qnum = 0;

/* 入队: 由 display_update / display_show_menu 调用 (均在 lvgl_task 持锁上下文) */
static void cjk_request_text(int x, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    if (!utf8 || !utf8[0]) return;
    if (s_cjk_qnum >= CJK_Q_MAX) return;   /* 队列满, 丢弃最旧避免溢出 */
    cjk_req_t *r = &s_cjk_q[s_cjk_qnum++];
    r->x = x; r->y = y; r->fg = fg; r->bg = bg;
    strncpy(r->s, utf8, CJK_Q_STR - 1);
    r->s[CJK_Q_STR - 1] = '\0';
}

/* 把字串直接写进 LVGL 全屏帧缓冲 (不走 esp_lcd, 避免重入/抢 SPI 崩溃).
 * 由 lvgl_flush_cb 在本帧 SWAP16 之前调用, LVGL 的 flush 会把含点阵的脏区正常刷出. */
/* 把字串写进点阵 shadow 帧缓冲 + 设掩码 (供 flush_cb 按脏区覆盖进 px_map).
 * 不访问 LVGL draw_buf 内部, 不调 SPI -> 无重入崩溃. */
static void cjk_write_lvgl_buf(int x, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    if (!s_cjk_fb || !s_cjk_mask || !utf8) return;
    const int GW = cjk_font_w;   // 16
    const int GH = cjk_font_h;   // 16
    int pen = x;
    const char *p = utf8;
    while (*p) {
        uint32_t u = 0;
        int n = 0;
        /* UTF-8 解码 */
        if ((*p & 0x80) == 0)      { u = (uint8_t)*p;       n = 1; }
        else if ((*p & 0xE0) == 0xC0) { u = ((uint32_t)(*p & 0x1F) << 6)  | (*(p+1) & 0x3F); n = 2; }
        else if ((*p & 0xF0) == 0xE0) { u = ((uint32_t)(*p & 0x0F) << 12) | ((*(p+1) & 0x3F) << 6) | (*(p+2) & 0x3F); n = 3; }
        else { p++; continue; } /* 非法 lead, 跳过 */
        p += n;

        int idx = cjk_unicode_to_glyph_idx(u);
        uint16_t buf[16 * 16];
        if (idx < 0) {
            /* 缺字: 画空心方框占位 */
            for (int i = 0; i < GH; i++)
                for (int j = 0; j < GW; j++)
                    buf[i * GW + j] = ((i == 0 || i == GH-1 || j == 0 || j == GW-1) ? fg : bg);
        } else {
            uint8_t g[16 * 16 / 8];
            const uint8_t *src = cjk_font_raw + (size_t)idx * cjk_font_glyph_bytes;
            for (int i = 0; i < GH; i++) {
                uint8_t b0 = src[i * 2], b1 = src[i * 2 + 1];
                for (int j = 0; j < 8; j++)
                    g[i * 16 + j]      = (b0 & (0x80 >> j)) ? 1 : 0;
                for (int j = 0; j < 8; j++)
                    g[i * 16 + 8 + j]  = (b1 & (0x80 >> j)) ? 1 : 0;
            }
            for (int i = 0; i < GH; i++)
                for (int j = 0; j < GW; j++)
                    buf[i * GW + j] = g[i * 16 + j] ? fg : bg;
        }
        /* SWAP16: 与 flush_cb 一致 */
        for (int i = 0; i < GW * GH; i++) {
            uint16_t v = buf[i];
            buf[i] = (uint16_t)((v << 8) | (v >> 8));
        }
        /* 写入 shadow 帧缓冲 + 设掩码 */
        for (int i = 0; i < GH; i++) {
            int py = y + i;
            if (py < 0 || py >= DISPLAY_HEIGHT) continue;
            for (int j = 0; j < GW; j++) {
                int px = pen + j;
                if (px < 0 || px >= DISPLAY_WIDTH) continue;
                s_cjk_fb[py * DISPLAY_WIDTH + px]   = buf[i * GW + j];
                s_cjk_mask[py * DISPLAY_WIDTH + px] = 1;
            }
        }
        pen += GW;
        if (pen > DISPLAY_WIDTH) break;
    }
}

/* 清整个点阵掩码 (界面切换时调用, 避免旧点阵残留) */
static void cjk_clear_all(void)
{
    if (s_cjk_mask) memset(s_cjk_mask, 0, (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT);
}

/* ============================================================
 * R103: 点阵 canvas —— LVGL 原生位图路径 (方案 C)
 * ============================================================ */
static void cjk_canvas_init(void)
{
    /* R105: 先创建曲名小 canvas。
       LVGL 中后创建的对象位于上层, 因此全屏 canvas 必须在其后创建,
       这样菜单/browse 独占态的全屏 canvas 才能正确盖住曲名。 */
    size_t tbytes = (size_t)TRACK_CANVAS_W * TRACK_CANVAS_H * 2;
    s_track_canvas_buf = (uint16_t *)heap_caps_malloc(tbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_track_canvas_buf)
        s_track_canvas_buf = (uint16_t *)heap_caps_malloc(tbytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_track_canvas_buf) {
        memset(s_track_canvas_buf, 0, tbytes);
        s_track_canvas = lv_canvas_create(lv_screen_active());
        lv_canvas_set_buffer(s_track_canvas, s_track_canvas_buf,
                             TRACK_CANVAS_W, TRACK_CANVAS_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_track_canvas, TRACK_CANVAS_X, TRACK_CANVAS_Y);
        lv_obj_clear_flag(s_track_canvas, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "track canvas ready %dx%d@(%d,%d) (%u bytes)",
                 TRACK_CANVAS_W, TRACK_CANVAS_H, TRACK_CANVAS_X, TRACK_CANVAS_Y,
                 (unsigned)tbytes);
    } else {
        ESP_LOGE(TAG, "track canvas buffer alloc failed (%u bytes)", (unsigned)tbytes);
    }

    size_t bytes = (size_t)CJK_CANVAS_W * CJK_CANVAS_H * 2;
    s_cjk_canvas_buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cjk_canvas_buf)
        s_cjk_canvas_buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_cjk_canvas_buf) {
        ESP_LOGE(TAG, "cjk canvas buffer alloc failed (%u bytes)", (unsigned)bytes);
        return;
    }
    memset(s_cjk_canvas_buf, 0, bytes);
    s_cjk_canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(s_cjk_canvas, s_cjk_canvas_buf,
                         CJK_CANVAS_W, CJK_CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_cjk_canvas, 0, 0);
    lv_obj_add_flag(s_cjk_canvas, LV_OBJ_FLAG_HIDDEN);   /* 默认隐藏, 菜单/browse 态才显示 */
    ESP_LOGI(TAG, "cjk canvas ready %dx%d (%u bytes)",
             CJK_CANVAS_W, CJK_CANVAS_H, (unsigned)bytes);
}

static void cjk_canvas_clear(void)
{
    if (!s_cjk_canvas_buf) return;
    /* 填背景色而非纯黑: 与 LVGL player 屏背景 (0x0a0e17) 一致, 避免出现色块 */
    uint16_t bg = (uint16_t)lv_color_to_u16(lv_color_hex(0x0a0e17));
    size_t n = (size_t)CJK_CANVAS_W * CJK_CANVAS_H;
    for (size_t i = 0; i < n; i++) s_cjk_canvas_buf[i] = bg;
}

/* 通用: 把一行点阵文本画进任意 RGB565 帧缓冲。
   颜色为 LVGL 原生 RGB565 (flush_cb 会对整帧统一 SWAP16)。缺字时画空心方框。 */
static void cjk_blit_text(uint16_t *fb, int fb_w, int fb_h,
                          int x, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    if (!fb || !utf8) return;
    const int GW = cjk_font_w;
    const int GH = cjk_font_h;
    int pen = x;
    const char *p = utf8;
    while (*p) {
        uint32_t u = 0; int n = 0;
        if ((*p & 0x80) == 0)         { u = (uint8_t)*p; n = 1; }
        else if ((*p & 0xE0) == 0xC0) { u = ((uint32_t)(*p & 0x1F) << 6)  | (*(p+1) & 0x3F); n = 2; }
        else if ((*p & 0xF0) == 0xE0) { u = ((uint32_t)(*p & 0x0F) << 12) | ((*(p+1) & 0x3F) << 6) | (*(p+2) & 0x3F); n = 3; }
        else { p++; continue; }        /* 非法 lead, 跳过 */
        p += n;

        int idx = cjk_unicode_to_glyph_idx(u);
        for (int i = 0; i < GH; i++) {
            int py = y + i;
            if (py < 0 || py >= fb_h) continue;
            uint8_t b0, b1;
            if (idx >= 0) {
                const uint8_t *src = cjk_font_raw + (size_t)idx * cjk_font_glyph_bytes;
                b0 = src[i * 2];
                b1 = src[i * 2 + 1];
            } else {
                /* 缺字: 空心方框 (首末行全亮, 中间仅左右两列) */
                bool edge = (i == 0 || i == GH - 1);
                b0 = edge ? 0xFF : 0x80;
                b1 = edge ? 0xFF : 0x01;
            }
            for (int j = 0; j < 8; j++) {
                int px = pen + j;
                if (px >= 0 && px < fb_w) fb[py * fb_w + px] = (b0 & (0x80 >> j)) ? fg : bg;
            }
            for (int j = 0; j < 8; j++) {
                int px = pen + 8 + j;
                if (px >= 0 && px < fb_w) fb[py * fb_w + px] = (b1 & (0x80 >> j)) ? fg : bg;
            }
        }
        pen += GW;
        if (pen > fb_w) break;
    }
}

/* 全屏 canvas (菜单/browse 独占态) */
static void cjk_canvas_text(int x, int y, const char *utf8, uint16_t fg, uint16_t bg)
{
    cjk_blit_text(s_cjk_canvas_buf, CJK_CANVAS_W, CJK_CANVAS_H, x, y, utf8, fg, bg);
}

/* R105: 曲名小 canvas */
static void cjk_track_clear(void)
{
    if (!s_track_canvas_buf) return;
    /* 填背景色(同 cjk_canvas_clear), 与 player 屏背景一致, 避免色块 */
    uint16_t bg = (uint16_t)lv_color_to_u16(lv_color_hex(0x0a0e17));
    size_t n = (size_t)TRACK_CANVAS_W * TRACK_CANVAS_H;
    for (size_t i = 0; i < n; i++) s_track_canvas_buf[i] = bg;
}

static void cjk_track_text(const char *utf8)
{
    if (!s_track_canvas_buf) return;
    const uint16_t fg = (uint16_t)lv_color_to_u16(lv_color_white());
    const uint16_t bg = (uint16_t)lv_color_to_u16(lv_color_hex(0x0a0e17));
    cjk_blit_text(s_track_canvas_buf, TRACK_CANVAS_W, TRACK_CANVAS_H, 0, 0, utf8, fg, bg);
}

/* flush_cb 在本帧写屏前调用: 把点阵队列写进 shadow 帧缓冲 + 掩码 */
static void cjk_flush_overlay(void)
{
    for (int i = 0; i < s_cjk_qnum; i++) {
        cjk_write_lvgl_buf(s_cjk_q[i].x, s_cjk_q[i].y, s_cjk_q[i].s,
                           s_cjk_q[i].fg, s_cjk_q[i].bg);
    }
    s_cjk_qnum = 0;
}

/* R063-fix: 不持锁版消息渲染,供 lvgl_task 在已持锁区间内调用(前向声明,定义见 ui_show_msg 处) */
static void ui_show_msg_nolock(const char *txt);
/* R100: 前向声明,供 lvgl_task 消费标志时调用(定义在后面) */
static void ui_show_player(void);
static void sd_icon_update(bool present);

/* LVGL 心跳 + 定时器处理任务 */
static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "DBG: lvgl_task ENTER (this proves task started)");
    /* R051-state: 打印栈水位 (high water mark)，单位 = word (4字节)。
       16384字节栈 → 起始 hwm ≈ 4096 word。实际使用 = 4096 - hwm。 */
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "DBG: lvgl_task hwm=%u words (stack=%u used=%u words)",
             (unsigned)hwm, 4096u, (unsigned)(4096u - hwm));
    /* 注意：本任务不再订阅/喂 task_wdt。原因：
     *  - main_task 已取消订阅以避免长操作被误判超时
     *  - IDF 5.5 在 main_task 取消订阅后，lvgl_task 即使 add 成功也会在 reset 时报
     *    "task not found"（wdt 内部状态被破坏），刷屏噪声影响调试
     *  - lvgl_task 死了不会有看门狗重启，main_task 不被监控时整个系统靠业务逻辑自我保护 */
    while (1) {
        /* P1-fix: lv_tick_inc 改由硬件 esp_timer 驱动, 此处不再调用 */
        /* 显式 lv_lock：避免与 main 中 display_update 的 LVGL 访问交叉持锁 */
        lv_lock();
        /* R056-fix4: 消费音量条标志(持锁状态下,无 race condition,无队列堆积) */
        if (s_vol_pending) {
            s_vol_pending = false;
            int vol = s_vol_value;
            if (vol_box) {
                lv_color_t vol_col = (vol <= 0) ? lv_color_hex(0x33405e) : lv_color_hex(0x2dd4bf);
                lv_obj_set_style_bg_color(vol_spk_box, vol_col, 0);
                lv_obj_set_style_line_color(vol_cone, vol_col, 0);
                int vol_clamped = vol < 0 ? 0 : (vol > VOLUME_LEVEL_MAX ? VOLUME_LEVEL_MAX : vol);
                lv_bar_set_value(vol_lvl, vol_clamped, LV_ANIM_OFF);
                lv_obj_clear_flag(vol_box, LV_OBJ_FLAG_HIDDEN);
            }
        }
        /* 若 main 标记了 dirty，执行其注册的业务 tick（消除 main 直接调 LVGL 死锁） */
        if (s_main_tick_pending && s_main_tick_cb) {
            s_main_tick_pending = false;
            s_main_tick_call_count++;
            s_main_tick_cb();
        }
        /* R063-fix: 消费弹窗消息标志(已在 lv_lock 内,直接调 LVGL 安全) */
        if (s_msg_pending) {
            s_msg_pending = false;
            ui_show_msg_nolock(s_msg_text);
        }
        /* R100: 消费 SD 图标/插拔提示更新(避免 main 调 LVGL 死锁) */
        if (s_sd_icon_pending) {
            s_sd_icon_pending = false;
            bool present = s_sd_icon_present;
            sd_icon_update(present);
            lv_label_set_text(lbl_sd_toast, present ? "TF card inserted" : "TF card ejected");
        }
        /* R100: 消费"清消息返回播放器"标志 */
        if (s_clear_msg_pending) {
            s_clear_msg_pending = false;
            /* R105: browse 现走点阵 canvas(独占态), 退出时必须清 s_menu_visible
               并隐藏全屏 canvas, 否则 canvas 会一直盖住 player 屏。 */
            s_menu_visible = false;
            if (s_cjk_canvas) lv_obj_add_flag(s_cjk_canvas, LV_OBJ_FLAG_HIDDEN);
            ui_show_player();
        }
        /* R102-fix: 消费菜单绘制/关闭标志(已在 lv_lock 内, 直接调 LVGL 安全)。
           main 侧只置标志, 杜绝 main 与 lvgl_task 并发碰 LVGL 导致的死循环。 */
        if (s_menu_pending) {
            s_menu_pending = false;
            menu_apply_nolock();
        }
        if (s_menu_close_pending) {
            s_menu_close_pending = false;
            /* R103: 隐藏点阵 canvas */
            if (s_cjk_canvas) lv_obj_add_flag(s_cjk_canvas, LV_OBJ_FLAG_HIDDEN);
            /* R103-fix: 必须显式恢复 player 屏。
               menu_apply_nolock() 曾把 g_player 设为 HIDDEN, 若此处只隐藏 canvas,
               屏幕上就没有任何可见对象 -> 退出菜单后一片黑屏。
               且不能依赖 display_update 里的 ui_show_player(): 它有指纹节流,
               退出菜单时状态未变(同一首歌/同一位置/仍 STOPPED) -> 指纹相同直接 return,
               player 会一直黑着, 直到按 PLAY 改变状态才恢复。 */
            ui_show_player();
            lv_obj_invalidate(lv_screen_active());
        }
        /* P1-fix: 消费 display_update 标志 (main_task 只缓存, 此处持锁执行) */
        if (s_disp_update_pending) {
            s_disp_update_pending = false;
            display_update_nolock(s_disp_cache.state, s_disp_cache.track_name,
                                  s_disp_cache.track_idx, s_disp_cache.total,
                                  s_disp_cache.current_sec, s_disp_cache.total_sec,
                                  s_disp_cache.speed, s_disp_cache.gear, s_disp_cache.volume);
        }
        /* P1-fix2: 卷轴动画 — 硬件时间戳驱动, 在 lv_timer_handler 之前切帧 */
        {
            int rdir = 0;
            uint32_t interval_us = 33000;   /* P1-fix3: 33ms/帧=30fps, 更流畅 (1.58s/转) */
            switch (s_reel_state) {
            case PLAYER_STATE_PLAYING:
                rdir = 1; interval_us = 33000; break;
            case PLAYER_STATE_FAST_FORWARD:
                rdir = 1;
                interval_us = (s_reel_gear >= 2) ? 16000 : (s_reel_gear >= 1) ? 22000 : 33000;
                break;
            case PLAYER_STATE_REWIND:
                rdir = -1;
                interval_us = (s_reel_gear >= 2) ? 16000 : (s_reel_gear >= 1) ? 22000 : 33000;
                break;
            default: break;
            }
            if (rdir != 0 && reel_l && reel_r) {
                uint64_t now_us = esp_timer_get_time();
                if (now_us - s_reel_last_us >= interval_us) {
                    s_reel_last_us = now_us;
                    s_reel_frame = (s_reel_frame + rdir + REEL_FRAME_COUNT) % REEL_FRAME_COUNT;
                    const lv_img_dsc_t *f = &reel_frame_dsc[s_reel_frame];
                    lv_img_set_src(reel_l, f);
                    lv_img_set_src(reel_r, f);
                }
            }
        }
        lv_timer_handler();
        lv_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
        /* R098g: 调试期临时 flush 计数诊断日志已移除(每 ~1s 刷屏, 干扰正常日志)。
           如需复测刷新, 临时取消下一行注释即可。 */
        // if ((++diag_loop % 200) == 0) ESP_LOGW(TAG, "DIAG lvgl loop=%u flush_cnt=%u", diag_loop, s_flush_cnt);
    }
}

/* ============================================================
 * esp_lcd 硬件初始化 (SPI3 + ST7789)
 * ============================================================ */
/* P0: SPI 颜色传输完成回调 (ISR 上下文) — 通知 LVGL flush 完成,
   配合双缓冲实现渲染与 SPI DMA 并行。buf 40 行 < SPI 单事务上限,
   每次 flush 恰好一次 draw_bitmap, 回调每帧触发一次。 */
static bool lcd_on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    if (s_flush_disp) lv_display_flush_ready(s_flush_disp);
    return false;
}

static esp_err_t lcd_hw_init(void)
{
    /* LCD 软电源: 拉低导通 */
    gpio_set_direction(LCD_POW_EN_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_POW_EN_IO, 0);
    ESP_LOGI(TAG, "DBG: lcd_pow_en set 0, readback=%d", gpio_get_level(LCD_POW_EN_IO));

    /* 上电后先做一次硬件复位: RST 拉低->延时->拉高->延时, 必须在 SPI 初始化前完成 */
    gpio_set_direction(DISPLAY_RESET_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(DISPLAY_RESET_IO, 1);          // 先确保高电平(退出复位)
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DISPLAY_RESET_IO, 0);          // 拉低复位
    vTaskDelay(pdMS_TO_TICKS(20));                // ST7789 要求 RST 低 >=10ms
    gpio_set_level(DISPLAY_RESET_IO, 1);          // 释放复位
    vTaskDelay(pdMS_TO_TICKS(120));               // 复位后等待 >=120ms 才能发命令
    ESP_LOGI(TAG, "DBG: lcd hw reset done");

    /* SPI3 总线 (独立于 SD 的 SPI2) */
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_MOSI_IO;
    buscfg.miso_io_num = -1;            // TFT 仅写, 不用 MISO
    buscfg.sclk_io_num = DISPLAY_SCLK_IO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 32752;   // ESP32-S3 SPI DMA 单次事务安全上限 (避开 32767 int16 边界, 防偶发截断)
    esp_err_t ret = spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: 0x%x", ret);
        return ret;
    }

    /* esp_lcd_spi IO 层: DC=GPIO16, CS 接地(-1) */
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num        = -1;
    io_cfg.dc_gpio_num        = DISPLAY_DC_IO;
    io_cfg.spi_mode           = 0;
    io_cfg.pclk_hz            = 20 * 1000 * 1000;   // P1-fix: 10->20MHz 刷新提速2x (如花屏降回15M)
    io_cfg.trans_queue_depth  = 10;
    io_cfg.on_color_trans_done = NULL;   /* 回退阻塞 flush, 排查动画卡顿 */
    io_cfg.lcd_cmd_bits       = 8;
    io_cfg.lcd_param_bits     = 8;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST,
                                   &io_cfg, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: 0x%x", ret);
        return ret;
    }

    /* 原生 ST7789 面板驱动 */
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = DISPLAY_RESET_IO;
    /* RGB/BGR 控制位 = MADCTL (0x36) bit3, ESP-IDF 驱动 rgb_ele_order 正是写此位
     * (RGB->0x00, BGR->0x08)。
     * 诊断实测 (v2.4.7-DIAG): 设 BGR 时 GREEN 显示蓝 / BLUE 显示绿 (绿蓝互换),
     * 故本模组 (2.4" ST7789) 实际需 RGB 顺序。
     * 另硬件 RAMCTRL 为 little-endian, 需配合 lv_conf.h 中 LV_COLOR_16_SWAP=1
     * 做软件字节交换补偿 (诊断 swap 帧 WHITE/BLACK 纯正即证)。 */
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_st7789(s_io_handle, &panel_cfg, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: 0x%x", ret);
        return ret;
    }

    // 注：RST 已在函数开头手动复位并等待 >=120ms，此处不再调用 esp_lcd_panel_reset，
    // 避免二次复位后时序不满足 ST7789 要求的 120ms 上电稳定时间导致 init 失效。
    ret = esp_lcd_panel_init(s_panel_handle);
    ESP_LOGI(TAG, "DBG: panel_init ret=0x%x", ret);

    /* 关键: 不手动覆盖 RAMCTRL (0xB0) 字节序。
     * ESP-IDF 驱动默认 RAMCTRL = 0x00,0xf0 = big-endian (RGB565 高字节先)。
     * 之前手动设 0xf4 (little-endian) 会把每像素高低字节反转, 导致颜色全乱 (白中带彩)。
     * 实测本模组面板 GRAM 实际按 little-endian 取数, 但不再用 RAMCTRL 寄存器改端序
     * (易与驱动默认打架), 而是在 lv_conf.h 设 LV_COLOR_16_SWAP=1 由 LVGL 在软件层
     * 交换每像素高低字节补偿 (诊断 swap 帧 WHITE/BLACK 纯正即证)。保持驱动默认 RAMCTRL。 */

    /* 屏幕方向: 由 config.h 的 DISPLAY_SWAP_XY / DISPLAY_MIRROR_X / DISPLAY_MIRROR_Y
     * 推导 (基于 DISPLAY_ORIENTATION 宏):
     *   - swap_xy=true (MV=1): 必填 (LVGL 320 列必须映射到物理 GRAM 行方向)
     *   - mirror_x=false + mirror_y=false: 文字/图标朝向自然, 填满全屏
     * MADCTL (含 BGR 位) 由 ESP-IDF 驱动在 init 内自动合成下发, 无需手动写。 */
    esp_lcd_panel_swap_xy(s_panel_handle, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(s_panel_handle, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    vTaskDelay(pdMS_TO_TICKS(50));   // 等显示开启稳定
    esp_lcd_panel_disp_on_off(s_panel_handle, true);   // 二次确认开显示
    ESP_LOGI(TAG, "DBG: disp_on_off done, BLK gpio15 level=%d, POW_EN gpio39 level=%d",
             gpio_get_level(DISPLAY_BLK_IO), gpio_get_level(LCD_POW_EN_IO));

    return ESP_OK;
}

/* 启动 LVGL 渲染任务（须在所有初始化 LVGL 写操作完成、进入主循环前调用，
 * 避免 main_task 与 lvgl_task 并发访问 LVGL 对象树导致死锁） */
void display_start_lvgl_task(void)
{
    // R071：合并 before/after lvgl task create 打印为单条；保留失败路径 ESP_LOGE
    TaskHandle_t h = NULL;
    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 16384, NULL, 8, &h,  /* P1-fix: prio 5->8 */
                                                    1, MALLOC_CAP_INTERNAL);
    if (rc != pdPASS || !h) {
        ESP_LOGE(TAG, "lvgl task CREATE FAILED! rc=%d", (int)rc);
    } else {
        ESP_LOGI(TAG, "lvgl task created handle=%p", (void *)h);
    }
}

/* ============================================================
 * LVGL UI 构建与模式切换
 * ============================================================ */
/* R109c: 磁带红色轮毂 = 预烘焙位图 (reel_img.h, 40x40 RGB565, 带 6 辐条)。
   用 lv_img 显示, 旋转时只转 1 个对象 (流畅), 辐条细节全保留。 */
static void ui_reel_create(lv_obj_t *parent, lv_obj_t **reel, lv_align_t align, int x)
{
    *reel = lv_img_create(parent);
    lv_img_set_src(*reel, &reel_frame_dsc[0]);  /* 预渲染帧 0 (透明背景) */
    lv_obj_align(*reel, align, x, 80);   /* P1: 48px 轮毂垂直居中 */
    lv_obj_clear_flag(*reel, LV_OBJ_FLAG_CLICKABLE);
}

/* P1: 磁带卷轴旋转 — step 恒 ±1, 速度由定时器周期动态控制 (消除大步进丢帧)。
   48 帧 (7.5°/帧), 正常 50ms/帧=2.4s/转; FF/REW 按档位 33/22/16ms, 逐帧不跳。
   动画驱动见 lvgl_task 循环中的 P1-fix2 块。 */

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

    /* P1-UI: 盒壳静态背景图 (渐变壳+花生跑道+磁带窗线圈), 296x96 */
    lv_obj_t *tape_bg = lv_img_create(g_player);
    lv_img_set_src(tape_bg, &cassette_bg_dsc);
    lv_obj_set_pos(tape_bg, 12, 42);
    lv_obj_clear_flag(tape_bg, LV_OBJ_FLAG_CLICKABLE);

    /* P1-UI: 64px 卷轴, 对准盒壳背景图中的轮毂中心 (左中心 x=57, 右中心 x=263, y=90) */
    reel_l = lv_img_create(g_player);
    lv_img_set_src(reel_l, &reel_frame_dsc[0]);
    lv_obj_set_pos(reel_l, 25, 58);   /* 57-32, 90-32 */
    lv_obj_clear_flag(reel_l, LV_OBJ_FLAG_CLICKABLE);

    reel_r = lv_img_create(g_player);
    lv_img_set_src(reel_r, &reel_frame_dsc[0]);
    lv_obj_set_pos(reel_r, 231, 58);  /* 263-32, 90-32 */
    lv_obj_clear_flag(reel_r, LV_OBJ_FLAG_CLICKABLE);

    /* 状态栏: 左=状态/曲目/模式  右=图形电量+音量 */
    lbl_status = lv_label_create(g_player);
    lv_obj_set_pos(lbl_status, M, 6);
    lv_obj_set_width(lbl_status, W - 2 * M - 90);  /* P1-UI: 加宽防换行 */
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_status, UI_FONT, 0);

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
    lv_obj_set_style_text_font(sd_icon_lbl, UI_FONT, 0);
    lv_obj_clear_flag(sd_icon_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* 插拔瞬时提示 (居中, 状态栏下方; 由 LVGL 定时器控制显隐) */
    lbl_sd_toast = lv_label_create(g_player);
    lv_obj_align(lbl_sd_toast, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_color(lbl_sd_toast, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_text_font(lbl_sd_toast, UI_FONT, 0);
    lv_obj_add_flag(lbl_sd_toast, LV_OBJ_FLAG_HIDDEN);

    /* 正在播放 小标题 */
    lbl_title = lv_label_create(g_player);
    lv_obj_set_pos(lbl_title, M, 34);
    lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);  /* P1-UI: 设计稿无此副标题 */
    lv_label_set_text(lbl_title, "Now Playing");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_text_font(lbl_title, UI_FONT, 0);

    /* 文件名 (大号, 循环滚动) */
    lbl_track = lv_label_create(g_player);
    lv_obj_set_pos(lbl_track, M, 24);  /* P1-UI: 上移 */
    lv_obj_set_width(lbl_track, W - 2 * M);
    lv_label_set_long_mode(lbl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_track, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_track, UI_FONT, 0);

    /* R108: 格式/品牌行 (文件名下方): FLAC|44KHZ|16bit|0918kbps + SQ */
    lbl_fmt = lv_label_create(g_player);
    lv_obj_set_pos(lbl_fmt, M, 144);  /* P1-UI: 移到磁带区下方 */
    lv_obj_set_width(lbl_fmt, W - 2 * M);
    lv_label_set_long_mode(lbl_fmt, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_fmt, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_fmt, UI_FONT, 0);

    /* 时间行: 当前(左) / 档位(中) / 总时长(右) */
    lbl_cur = lv_label_create(g_player);
    lv_obj_set_pos(lbl_cur, M, 164);  /* P1-UI */
    lv_obj_set_style_text_color(lbl_cur, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_cur, UI_FONT, 0);

    lbl_gear = lv_label_create(g_player);
    lv_obj_set_pos(lbl_gear, W / 2 - 22, 164);  /* P1-UI */
    lv_obj_set_style_text_color(lbl_gear, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_text_font(lbl_gear, UI_FONT, 0);

    lbl_dur = lv_label_create(g_player);
    lv_obj_set_pos(lbl_dur, W - M, 164);  /* P1-UI */
    lv_obj_set_style_text_align(lbl_dur, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_dur, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_dur, UI_FONT, 0);

    /* 进度条 + 百分比 */
    bar_prog = lv_bar_create(g_player);
    lv_obj_set_size(bar_prog, W - 2 * M, 6);   /* P1-UI: 细进度条 */
    lv_obj_set_pos(bar_prog, M, 182);
    lv_bar_set_range(bar_prog, 0, 1000);
    lv_bar_set_value(bar_prog, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_prog, lv_color_hex(0x16203a), 0);
    lv_obj_set_style_bg_color(bar_prog, lv_color_hex(0x22c55e), LV_PART_INDICATOR); /* R108: 设计稿绿色填充 */
    lv_obj_set_style_radius(bar_prog, 4, 0);

    lbl_percent = lv_label_create(g_player);
    lv_obj_set_pos(lbl_percent, W - M, 168);
    lv_obj_add_flag(lbl_percent, LV_OBJ_FLAG_HIDDEN);  /* P1-UI: 设计稿无百分比 */
    lv_obj_set_style_text_align(lbl_percent, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_percent, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_percent, UI_FONT, 0);

    /* 快进/快退 读秒提示（居中醒目，仅快进退时显示） */
    lbl_seek = lv_label_create(g_player);
    lv_obj_set_pos(lbl_seek, M, 196);  /* P1-UI */
    lv_obj_set_width(lbl_seek, W - 2 * M);
    lv_obj_set_style_text_align(lbl_seek, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_seek, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_text_font(lbl_seek, UI_FONT, 0);
    lv_obj_add_flag(lbl_seek, LV_OBJ_FLAG_HIDDEN);

    /* P2: 底部6键指示条 — 几何图形图标 */
    {
        const int KB_Y = 204;
        const int KB_H = 28;
        const int BTN_W = 42;
        const int GAP = 8;
        const int total_w = 6 * BTN_W + 5 * GAP;
        const int start_x = (W - total_w) / 2;
        for (int i = 0; i < 6; i++) {
            key_btn_t *k = &s_keys[i];
            k->btn = lv_obj_create(g_player);
            lv_obj_set_size(k->btn, BTN_W, KB_H);
            lv_obj_set_pos(k->btn, start_x + i * (BTN_W + GAP), KB_Y);
            lv_obj_set_style_bg_color(k->btn, lv_color_hex(0x161c2e), 0);
            lv_obj_set_style_border_color(k->btn, lv_color_hex(0x232c44), 0);
            lv_obj_set_style_border_width(k->btn, 1, 0);
            lv_obj_set_style_radius(k->btn, 4, 0);
            lv_obj_set_style_pad_all(k->btn, 0, 0);
            lv_obj_clear_flag(k->btn, LV_OBJ_FLAG_CLICKABLE);
            k->icon = lv_obj_create(k->btn);
            lv_obj_set_size(k->icon, 24, 18);
            lv_obj_center(k->icon);
            lv_obj_set_style_bg_opa(k->icon, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(k->icon, 0, 0);
            lv_obj_set_style_pad_all(k->icon, 0, 0);
            lv_obj_clear_flag(k->icon, LV_OBJ_FLAG_CLICKABLE);
        }
        const lv_color_t IC = lv_color_hex(0x9aa3b2);
        /* 0: 快退 */
        {
            lv_obj_t *p = s_keys[0].icon;
            static lv_point_precise_t t1[] = {{11,2},{11,16},{2,9},{11,2}};
            lv_obj_t *l1 = lv_line_create(p); lv_line_set_points(l1,t1,4);
            lv_obj_set_style_line_width(l1,2,0); lv_obj_set_style_line_color(l1,IC,0); lv_obj_set_style_line_rounded(l1,true,0);
            static lv_point_precise_t t2[] = {{22,2},{22,16},{13,9},{22,2}};
            lv_obj_t *l2 = lv_line_create(p); lv_line_set_points(l2,t2,4);
            lv_obj_set_style_line_width(l2,2,0); lv_obj_set_style_line_color(l2,IC,0); lv_obj_set_style_line_rounded(l2,true,0);
        }
        /* 1: 播放/暂停 */
        {
            lv_obj_t *p = s_keys[1].icon;
            static lv_point_precise_t t[] = {{6,2},{6,16},{18,9},{6,2}};
            lv_obj_t *l = lv_line_create(p); lv_line_set_points(l,t,4);
            lv_obj_set_style_line_width(l,2,0); lv_obj_set_style_line_color(l,IC,0); lv_obj_set_style_line_rounded(l,true,0);
            s_play_tri = l;
            lv_obj_t *pa = lv_obj_create(p); lv_obj_set_size(pa,3,14); lv_obj_set_pos(pa,6,2);
            lv_obj_set_style_bg_color(pa,IC,0); lv_obj_set_style_border_width(pa,0,0); lv_obj_set_style_radius(pa,1,0);
            lv_obj_clear_flag(pa,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_flag(pa,LV_OBJ_FLAG_HIDDEN);
            s_pause_l = pa;
            lv_obj_t *pb = lv_obj_create(p); lv_obj_set_size(pb,3,14); lv_obj_set_pos(pb,15,2);
            lv_obj_set_style_bg_color(pb,IC,0); lv_obj_set_style_border_width(pb,0,0); lv_obj_set_style_radius(pb,1,0);
            lv_obj_clear_flag(pb,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_flag(pb,LV_OBJ_FLAG_HIDDEN);
            s_pause_r = pb;
        }
        /* 2: 快进 */
        {
            lv_obj_t *p = s_keys[2].icon;
            static lv_point_precise_t t1[] = {{2,2},{2,16},{11,9},{2,2}};
            lv_obj_t *l1 = lv_line_create(p); lv_line_set_points(l1,t1,4);
            lv_obj_set_style_line_width(l1,2,0); lv_obj_set_style_line_color(l1,IC,0); lv_obj_set_style_line_rounded(l1,true,0);
            static lv_point_precise_t t2[] = {{13,2},{13,16},{22,9},{13,2}};
            lv_obj_t *l2 = lv_line_create(p); lv_line_set_points(l2,t2,4);
            lv_obj_set_style_line_width(l2,2,0); lv_obj_set_style_line_color(l2,IC,0); lv_obj_set_style_line_rounded(l2,true,0);
        }
        /* 3: 停止 */
        {
            lv_obj_t *p = s_keys[3].icon;
            lv_obj_t *sq = lv_obj_create(p); lv_obj_set_size(sq,12,12); lv_obj_align(sq,LV_ALIGN_CENTER,0,0);
            lv_obj_set_style_bg_color(sq,IC,0); lv_obj_set_style_border_width(sq,0,0); lv_obj_set_style_radius(sq,1,0);
            lv_obj_clear_flag(sq,LV_OBJ_FLAG_CLICKABLE);
        }
        /* 4: 上首 */
        {
            lv_obj_t *p = s_keys[4].icon;
            lv_obj_t *vl = lv_obj_create(p); lv_obj_set_size(vl,3,14); lv_obj_set_pos(vl,2,2);
            lv_obj_set_style_bg_color(vl,IC,0); lv_obj_set_style_border_width(vl,0,0); lv_obj_set_style_radius(vl,1,0);
            lv_obj_clear_flag(vl,LV_OBJ_FLAG_CLICKABLE);
            static lv_point_precise_t t[] = {{20,2},{20,16},{10,9},{20,2}};
            lv_obj_t *l = lv_line_create(p); lv_line_set_points(l,t,4);
            lv_obj_set_style_line_width(l,2,0); lv_obj_set_style_line_color(l,IC,0); lv_obj_set_style_line_rounded(l,true,0);
        }
        /* 5: 下首 */
        {
            lv_obj_t *p = s_keys[5].icon;
            static lv_point_precise_t t[] = {{4,2},{4,16},{14,9},{4,2}};
            lv_obj_t *l = lv_line_create(p); lv_line_set_points(l,t,4);
            lv_obj_set_style_line_width(l,2,0); lv_obj_set_style_line_color(l,IC,0); lv_obj_set_style_line_rounded(l,true,0);
            lv_obj_t *vr = lv_obj_create(p); lv_obj_set_size(vr,3,14); lv_obj_set_pos(vr,19,2);
            lv_obj_set_style_bg_color(vr,IC,0); lv_obj_set_style_border_width(vr,0,0); lv_obj_set_style_radius(vr,1,0);
            lv_obj_clear_flag(vr,LV_OBJ_FLAG_CLICKABLE);
        }
    }
    /* 底部按键提示 (保留, 菜单态用) */
    lbl_hint = lv_label_create(g_player);
    lv_obj_set_pos(lbl_hint, M, H - 20);
    lv_obj_set_style_text_color(lbl_hint, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_hint, UI_FONT, 0);
    lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);

    /* A-B 复读：进度条上的 A/B 点标记（细竖线）+ 底部灰栏信息 */
    ab_mark_a = lv_obj_create(g_player);
    lv_obj_set_size(ab_mark_a, 2, 10);
    lv_obj_set_pos(ab_mark_a, M, 180);  /* P1-UI */
    lv_obj_set_style_bg_color(ab_mark_a, lv_color_hex(0xffffff), 0); /* 白色，区别于青色进度条与橙色 B 点 */
    lv_obj_set_style_border_width(ab_mark_a, 0, 0);
    lv_obj_set_style_pad_all(ab_mark_a, 0, 0);
    lv_obj_set_style_shadow_width(ab_mark_a, 0, 0);
    lv_obj_set_style_radius(ab_mark_a, 0, 0);
    lv_obj_add_flag(ab_mark_a, LV_OBJ_FLAG_HIDDEN);

    ab_mark_b = lv_obj_create(g_player);
    lv_obj_set_size(ab_mark_b, 2, 10);
    lv_obj_set_pos(ab_mark_b, M, 180);  /* P1-UI */
    lv_obj_set_style_bg_color(ab_mark_b, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_border_width(ab_mark_b, 0, 0);
    lv_obj_set_style_pad_all(ab_mark_b, 0, 0);
    lv_obj_set_style_shadow_width(ab_mark_b, 0, 0);
    lv_obj_set_style_radius(ab_mark_b, 0, 0);
    lv_obj_add_flag(ab_mark_b, LV_OBJ_FLAG_HIDDEN);

    lbl_ab = lv_label_create(g_player);
    lv_obj_set_pos(lbl_ab, M, H - 20 - 18);
    lv_obj_set_width(lbl_ab, W - 2 * M);
    lv_obj_set_style_text_align(lbl_ab, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_ab, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(lbl_ab, UI_FONT, 0);
    lv_obj_add_flag(lbl_ab, LV_OBJ_FLAG_HIDDEN);

    /* 居中消息 (splash/提示/浏览) */
    g_msg = lv_label_create(scr);
    lv_label_set_long_mode(g_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_msg, W - 24);
    lv_obj_set_style_text_align(g_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_msg, lv_color_white(), 0);
    /* R052: splash ASCII 兜底时用蒙文字体（不依赖 freetype 也不会因 lv_font_chinese_16
     * 子集不含 ASCII 字符而 fallback 到失败的 freetype 路径导致渲染死循环）。
     * 背景改为透明：黑底框已按需求移除；背景本就是深色屏, 抗锯齿灰阶边沿与深色混合无明显彩边。*/
    lv_obj_set_style_text_font(g_msg, UI_FONT, 0);
    lv_obj_set_style_bg_opa(g_msg, LV_OPA_TRANSP, 0);
    lv_obj_align(g_msg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);

    /* ---- SD-OTA 升级界面 ---- */
    g_ota = lv_obj_create(scr);
    lv_obj_set_size(g_ota, W, H);
    lv_obj_set_style_bg_color(g_ota, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_border_width(g_ota, 0, 0);
    lv_obj_set_style_pad_all(g_ota, 0, 0);
    lv_obj_clear_flag(g_ota, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_ota, LV_OBJ_FLAG_HIDDEN);

    ota_title = lv_label_create(g_ota);
    lv_obj_set_pos(ota_title, M, 12);
    lv_obj_set_style_text_color(ota_title, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_text_font(ota_title, UI_FONT, 0);

    ota_body = lv_label_create(g_ota);
    lv_obj_set_pos(ota_body, M, 54);
    lv_obj_set_width(ota_body, W - 2 * M);
    lv_label_set_long_mode(ota_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ota_body, lv_color_white(), 0);
    lv_obj_set_style_text_font(ota_body, UI_FONT, 0);

    ota_bar = lv_bar_create(g_ota);
    lv_obj_set_size(ota_bar, W - 2 * M, 16);
    lv_obj_set_pos(ota_bar, M, 132);
    lv_bar_set_range(ota_bar, 0, 100);
    lv_bar_set_value(ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x16203a), 0);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x2dd4bf), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ota_bar, 4, 0);
    lv_obj_add_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);

    ota_pct = lv_label_create(g_ota);
    lv_obj_set_pos(ota_pct, W - M, 150);
    lv_obj_set_style_text_align(ota_pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(ota_pct, lv_color_white(), 0);
    lv_obj_set_style_text_font(ota_pct, UI_FONT, 0);
    lv_obj_add_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);

    ota_hint = lv_label_create(g_ota);
    lv_obj_set_pos(ota_hint, M, H - 20);
    lv_obj_set_style_text_color(ota_hint, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(ota_hint, UI_FONT, 0);

    /* ---- R051：A-B 复读状态屏（迷你进度条 + 状态 + 动作列表） ---- */
    g_ab_menu = lv_obj_create(scr);
    lv_obj_set_size(g_ab_menu, W, H);
    lv_obj_set_style_bg_color(g_ab_menu, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_border_width(g_ab_menu, 0, 0);
    lv_obj_set_style_pad_all(g_ab_menu, 0, 0);
    lv_obj_clear_flag(g_ab_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_ab_menu, LV_OBJ_FLAG_HIDDEN);

    abm_title = lv_label_create(g_ab_menu);
    lv_obj_set_pos(abm_title, M, 12);
    lv_obj_set_style_text_color(abm_title, lv_color_hex(0x2dd4bf), 0);
    lv_obj_set_style_text_font(abm_title, UI_FONT, 0);

    abm_bar = lv_bar_create(g_ab_menu);
    lv_obj_set_size(abm_bar, W - 2 * M, 12);
    lv_obj_set_pos(abm_bar, M, 40);
    lv_bar_set_range(abm_bar, 0, 1000);
    lv_bar_set_value(abm_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(abm_bar, lv_color_hex(0x16203a), 0);
    lv_obj_set_style_bg_color(abm_bar, lv_color_hex(0x2dd4bf), LV_PART_INDICATOR);
    lv_obj_set_style_radius(abm_bar, 4, 0);

    abm_mark_a = lv_obj_create(g_ab_menu);
    lv_obj_set_size(abm_mark_a, 2, 16);
    lv_obj_set_pos(abm_mark_a, M, 38);
    lv_obj_set_style_bg_color(abm_mark_a, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(abm_mark_a, 0, 0);
    lv_obj_set_style_pad_all(abm_mark_a, 0, 0);
    lv_obj_set_style_shadow_width(abm_mark_a, 0, 0);
    lv_obj_set_style_radius(abm_mark_a, 0, 0);
    lv_obj_add_flag(abm_mark_a, LV_OBJ_FLAG_HIDDEN);

    abm_mark_b = lv_obj_create(g_ab_menu);
    lv_obj_set_size(abm_mark_b, 2, 16);
    lv_obj_set_pos(abm_mark_b, M, 38);
    lv_obj_set_style_bg_color(abm_mark_b, lv_color_hex(0xf5a623), 0);
    lv_obj_set_style_border_width(abm_mark_b, 0, 0);
    lv_obj_set_style_pad_all(abm_mark_b, 0, 0);
    lv_obj_set_style_shadow_width(abm_mark_b, 0, 0);
    lv_obj_set_style_radius(abm_mark_b, 0, 0);
    lv_obj_add_flag(abm_mark_b, LV_OBJ_FLAG_HIDDEN);

    abm_stat = lv_label_create(g_ab_menu);
    lv_obj_set_pos(abm_stat, M, 60);
    lv_obj_set_width(abm_stat, W - 2 * M);
    lv_obj_set_style_text_color(abm_stat, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(abm_stat, UI_FONT, 0);

    for (int i = 0; i < 4; i++) {
        abm_lines[i] = lv_label_create(g_ab_menu);
        lv_obj_set_pos(abm_lines[i], M, 92 + i * 22);
        lv_obj_set_width(abm_lines[i], W - 2 * M);
        lv_obj_set_style_text_color(abm_lines[i], lv_color_hex(0x8a93a6), 0);
        lv_obj_set_style_text_font(abm_lines[i], UI_FONT, 0);
    }

    abm_hint = lv_label_create(g_ab_menu);
    lv_obj_set_pos(abm_hint, M, H - 20);
    lv_obj_set_style_text_color(abm_hint, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_text_font(abm_hint, UI_FONT, 0);
}

static void ui_show_player(void)
{
    static uint32_t n = 0;
    if (n < 3) {
        ESP_LOGI(TAG, "DBG: ui_show_player #%u g_player=%p g_msg=%p", (unsigned)n, (void*)g_player, (void*)g_msg);
    }
    n++;
    lv_obj_clear_flag(g_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ota, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ab_menu, LV_OBJ_FLAG_HIDDEN);
}

static void ui_show_msg(const char *txt)
{
    lv_lock();   /* 保护 LVGL 对象树: main_task 与 lvgl_task 并发写必须加锁 */
    lv_label_set_text(g_msg, txt);
    lv_obj_clear_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ota, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ab_menu, LV_OBJ_FLAG_HIDDEN);
    lv_unlock();
}

/* R063-fix: 不持锁版本,仅给 lvgl_task 在已持锁区间内调用,避免双重加锁 */
static void ui_show_msg_nolock(const char *txt)
{
    lv_label_set_text(g_msg, txt);
    lv_obj_clear_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ota, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ab_menu, LV_OBJ_FLAG_HIDDEN);
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
#define SCREEN_SAVER_TIMEOUT_US  (UINT64_MAX)  /* R055-fix1: 屏蔽屏保(用户决定),后续重新设计 */

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
    case PLAYER_STATE_PLAYING:       return "PLAYING";
    case PLAYER_STATE_PAUSED:        return "PAUSED";
    case PLAYER_STATE_STOPPED:       return "STOPPED";
    case PLAYER_STATE_FAST_FORWARD:  return "FF";
    case PLAYER_STATE_REWIND:        return "REW";
    default:                         return "???";
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
 * 颜色诊断 (DISPLAY_DIAG): 纯色 + 大小端测试
 * 直接绕过 LVGL, 用 esp_lcd_panel_draw_bitmap 写裸 RGB565,
 * 排除 LVGL 颜色转换/COLOR_16_SWAP 干扰, 定位 RGB/BGR 与字节序。
 * 测试序列 (每块停 ~1.2s, 串口有日志):
 *   0 红  1 绿  2 蓝  3 白  4 黑
 *   每个纯色都做 "不交换 / 交换高低字节" 两种, 共 10 帧。
 * 目的: 肉眼看哪种组合显示正确 → 决定 rgb_ele_order + 是否 SWAP。
 * ============================================================ */
#ifdef DISPLAY_DIAG
static uint16_t diag_make(uint16_t rgb565, bool swap)
{
    if (swap) {
        uint8_t hi = rgb565 >> 8;
        uint8_t lo = rgb565 & 0xff;
        return (uint16_t)((lo << 8) | hi);   // 字节交换
    }
    return rgb565;
}

static void diag_fill_block(uint16_t rgb565, bool swap)
{
    const int W = DISPLAY_WIDTH, H = DISPLAY_HEIGHT;
    const size_t px = (size_t)W * H;
    /* 优先 PSRAM, 失败回退 DRAM */
    uint16_t *fb = (uint16_t *)heap_caps_malloc(px * 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) fb = (uint16_t *)heap_caps_malloc(px * 2,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!fb) return;
    uint16_t v = diag_make(rgb565, swap);
    for (size_t i = 0; i < px; i++) fb[i] = v;
    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, W, H, fb);
    heap_caps_free(fb);
}

void display_diag_run(void)
{
    if (!g_display_initialized) return;
    ESP_LOGW(TAG, "=== DISPLAY_DIAG START (纯色+大小端) ===");
    struct { uint16_t c; const char *name; } tests[] = {
        { 0xF800, "RED"   },  // RGB565 红
        { 0x07E0, "GREEN" },  // 绿
        { 0x001F, "BLUE"  },  // 蓝
        { 0xFFFF, "WHITE" },  // 白
        { 0x0000, "BLACK" },  // 黑
    };
    for (int i = 0; i < (int)(sizeof(tests) / sizeof(tests[0])); i++) {
        ESP_LOGW(TAG, "[DIAG] %s (RGB565=0x%04x) no-swap", tests[i].name, tests[i].c);
        diag_fill_block(tests[i].c, false);
        vTaskDelay(pdMS_TO_TICKS(1200));
        ESP_LOGW(TAG, "[DIAG] %s swap", tests[i].name);
        diag_fill_block(tests[i].c, true);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    ESP_LOGW(TAG, "=== DISPLAY_DIAG DONE ===");
}
#endif /* DISPLAY_DIAG */

/* ============================================================
 * 公共 API
 * ============================================================ */
static void sd_toast_timer_cb(lv_timer_t *t);  // 前向声明 (display_init 中注册)
static void vol_hide_timer_cb(lv_timer_t *t);   // 音量条自动隐藏定时器

/* ---- 内存诊断：PSRAM/DRAM 堆水位 + 当前屏 LVGL 对象数 ---- */
void display_mem_report(void)
{
    const uint32_t spi  = MALLOC_CAP_SPIRAM  | MALLOC_CAP_8BIT;
    const uint32_t dram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t spi_total  = heap_caps_get_total_size(spi);
    size_t spi_free   = heap_caps_get_free_size(spi);
    size_t dram_total = heap_caps_get_total_size(dram);
    size_t dram_free  = heap_caps_get_free_size(dram);
    lv_obj_t *scr = lv_screen_active();
    int objs = scr ? (int)lv_obj_get_child_cnt(scr) : -1;
    ESP_LOGW("mem", "PSRAM total=%u free=%u largest=%u used=%u (%.1f%%)",
             spi_total, spi_free, heap_caps_get_largest_free_block(spi),
             spi_total - spi_free,
             100.0f * (float)(spi_total - spi_free) / (float)(spi_total ? spi_total : 1));
    ESP_LOGW("mem", "DRAM  total=%u free=%u largest=%u used=%u (%.1f%%)",
             dram_total, dram_free, heap_caps_get_largest_free_block(dram),
             dram_total - dram_free,
             100.0f * (float)(dram_total - dram_free) / (float)(dram_total ? dram_total : 1));
    ESP_LOGW("mem", "LVGL objects(active screen children)=%d", objs);
}

void display_show_splash(void)
{
    if (!g_display_initialized) return;
    /* freetype 未就绪（cjk.ttf 未烧录到 font 分区）时, 渲染中文 label 会走入
     * 未初始化的 freetype 路径导致 LVGL 死循环 + task_wdt, 故用 ASCII 兜底 */
    const char *msg = font_partition_ready()
        ? "TapeBook Player\nESP32-S3\nLoading SD Card..."
        : "TapeBook Player\nESP32-S3\nloading SD...";
    /* R063-fix: 仅设标志,避免 main 直接调 LVGL 与 lvgl_task 死锁 */
    strncpy(s_msg_text, msg, sizeof(s_msg_text) - 1);
    s_msg_text[sizeof(s_msg_text) - 1] = '\0';
    s_msg_pending = true;
}

void display_show_no_files(void)
{
    if (!g_display_initialized) return;
    /* R063-fix: 仅设标志,避免 main 直接调 LVGL 与 lvgl_task 死锁 */
    strncpy(s_msg_text, "No audio files found.\nCopy .mp3/.flac/.wav\nto the SD card.",
            sizeof(s_msg_text) - 1);
    s_msg_text[sizeof(s_msg_text) - 1] = '\0';
    s_msg_pending = true;
}

void display_show_no_card(void)
{
    if (!g_display_initialized) return;
    /* R063-fix: 仅设标志,避免 main 直接调 LVGL 与 lvgl_task 死锁 (P0) */
    strncpy(s_msg_text, "SD card not detected.\nPlease insert an SD card.",
            sizeof(s_msg_text) - 1);
    s_msg_text[sizeof(s_msg_text) - 1] = '\0';
    s_msg_pending = true;
}

/* R100: 清除全屏消息(如"SD card not detected")并返回播放器界面，插卡成功/正常播放时调用 */
void display_clear_msg(void)
{
    if (!g_display_initialized) return;
    s_clear_msg_pending = true;   /* 由 lvgl 任务消费调 ui_show_player */
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
    /* R100: 仅设标志,由 lvgl 任务消费(避免 main 调 LVGL 死锁) */
    s_sd_icon_present = present;
    s_sd_icon_pending  = true;
}

/* 设置 TF 卡在位状态: 更新图标 + 弹插拔瞬时提示 */
void display_set_sd_present(bool present)
{
    /* R100: 仅设标志,由 lvgl 任务消费(避免 main 调 LVGL 死锁——拔卡死机根因) */
    s_sd_icon_present = present;
    s_sd_icon_pending  = true;
    g_sd_toast_until   = esp_timer_get_time() + 1500 * 1000;  // 显示 1.5s
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

/* R056-fix4: 不用 lv_async_call(队列堆积会累积状态损坏 → 屏幕黑 + 几秒后 Guru Meditation)。
   改用全局标志 + lvgl_task 内部轮询。main_task 只设标志,lvgl_task 在持锁状态下消费。
   这样:
   - 消除异步队列(无堆积)
   - lvgl_task 自己消费(无 race condition)
   - 状态始终一致 */

/* 触发音量条显示: 立即刷新音量条与扬声器图标, 并重置 3 秒隐藏计时 */
void display_show_volume(int volume)
{
    if (!g_display_initialized || !vol_box) return;
    /* R056-fix4: 仅设标志,lvgl_task 会在持锁状态下消费并执行 LVGL 操作。
       不调 lv_async_call(避免队列堆积),也不直接调 LVGL(避免与 lvgl_task 死锁)。 */
    s_vol_value = volume;
    s_vol_pending = true;
    /* 3 秒隐藏倒计时:仅设变量,不涉及 LVGL,无需异步 */
    g_vol_hide_until = esp_timer_get_time() + 3000 * 1000;
}

static void display_update_nolock(player_state_t state,
                    const char *track_name,
                    int track_idx, int total,
                    int current_sec, int total_sec,
                    float speed, int gear, int volume)
{
    if (!g_display_initialized) return;
    // R071: 删除冗余 DBG 打印（before/after lv_lock call#N + now/last/diff）——
    // 主循环 200ms 节流每条都打→ 20行/秒纯噪声，把真崩因日志淹没。
    // LVGL 死锁问题已在 R063-fix 用异步 pending 解决，无需每次锁前后再验。
    // call_count 仅用于 ui_show_player #N 调试（前3次打印），保留不增噪声。
    static uint32_t call_count = 0;
    if (call_count <= 3) {
        ESP_LOGI(TAG, "DBG: display_update call #%u state=%d track='%s' idx=%d/%d pos=%d/%d spd=%.2f gear=%d vol=%d",
                 (unsigned)call_count, (int)state, track_name, track_idx, total,
                 current_sec, total_sec, speed, gear, volume);
    }
    call_count++;
    /* P1-fix: 已在 lvgl_task 持锁区调用, 无需再加锁 */

    /* 驱动磁带卷轴动画 (方向/速度由状态与档位决定) */
    s_reel_state = state;
    s_reel_gear  = gear;

    /* A-B 复读状态（决定底部灰栏 + 进度条标记刷新） */
    int ab_a   = audio_player_ab_a_ms();
    int ab_b   = audio_player_ab_b_ms();
    bool ab_on = audio_player_is_ab_enabled();

    uint64_t now = esp_timer_get_time();

    uint32_t fp = calc_fingerprint(state, track_idx, total,
                                   current_sec, total_sec, speed, gear, volume);
    fp = fp * 31 + (uint32_t)s_play_mode;   // 播放模式变化也触发刷新
    /* A-B 状态变化也要触发刷新（标记 A/B 或开关复读） */
    fp = fp * 31 + (uint32_t)(ab_a >= 0 ? (uint32_t)ab_a : 0xFFFF0001u);
    fp = fp * 31 + (uint32_t)(ab_b >= 0 ? (uint32_t)ab_b : 0xFFFF0002u);
    fp = fp * 31 + (uint32_t)(ab_on ? 1u : 0u);
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

    ui_show_player();  /* 默认渲染 player 屏 */

    /* R102: MENU 态不跑 ui_show_player(否则会恢复 player 盖掉点阵), 改为持续重绘菜单点阵。
       R102-fix: 此处已持 lv_lock(函数开头 lv_lock), 直接调 nolock 版本;
       不要调 display_show_menu(那是无锁入口, 只会置标志导致延迟一帧)。 */
    if (s_menu_visible) {
        menu_apply_nolock();
    } else if (s_cjk_canvas && !lv_obj_has_flag(s_cjk_canvas, LV_OBJ_FLAG_HIDDEN)) {
        /* 非菜单态: 确保点阵 canvas 已隐藏, 避免遮挡 player 屏 */
        lv_obj_add_flag(s_cjk_canvas, LV_OBJ_FLAG_HIDDEN);
    }


    /* 时间字符串先算好（时间行与读秒共用） */
    char cur[16], tot[16];
    format_time(current_sec, cur, sizeof(cur));
    format_time(total_sec, tot, sizeof(tot));

    /* 状态栏: 状态 · 曲目 x/y · [模式] · NOR（电量/音量已改为图形）
       R108: 模式词映射设计稿风格缩写 (SEQ=顺序/LST=列表循环/SGL=单曲), 尾加 NOR 标 */
    const char *mode_s = (s_play_mode == 1) ? "LST" : (s_play_mode == 2) ? "SGL" : "SEQ";
    char line0[64];
    snprintf(line0, sizeof(line0), "%s %03d/%03d %s NOR",
             state_word(state), track_idx, total, mode_s);
    lv_label_set_text(lbl_status, line0);

    /* P2: 底部按键条高亮 */
    {
        int active_idx = -1;
        switch (state) {
        case PLAYER_STATE_PLAYING:     active_idx = 1; break;
        case PLAYER_STATE_PAUSED:      active_idx = 1; break;
        case PLAYER_STATE_FAST_FORWARD:active_idx = 2; break;
        case PLAYER_STATE_REWIND:      active_idx = 0; break;
        case PLAYER_STATE_STOPPED:     active_idx = 3; break;
        default: break;
        }
        for (int i = 0; i < 6; i++) {
            key_btn_t *k = &s_keys[i];
            if (!k->btn) continue;
            bool act = (i == active_idx);
            lv_color_t ic = act ? lv_color_hex(0x4ade80) : lv_color_hex(0x9aa3b2);
            lv_obj_set_style_bg_color(k->btn, act ? lv_color_hex(0x0d3b1e) : lv_color_hex(0x161c2e), 0);
            lv_obj_set_style_border_color(k->btn, act ? lv_color_hex(0x22c55e) : lv_color_hex(0x232c44), 0);
            /* 更新图标容器内所有 line 和 obj 的颜色 */
            if (k->icon) {
                uint32_t cnt = lv_obj_get_child_cnt(k->icon);
                for (uint32_t j = 0; j < cnt; j++) {
                    lv_obj_t *child = lv_obj_get_child(k->icon, j);
                    if (lv_obj_check_type(child, &lv_line_class)) {
                        lv_obj_set_style_line_color(child, ic, 0);
                    } else {
                        lv_obj_set_style_bg_color(child, ic, 0);
                    }
                }
            }
        }
        if (s_play_tri && s_pause_l && s_pause_r) {
            bool paused = (state == PLAYER_STATE_PAUSED);
            if (paused) {
                lv_obj_add_flag(s_play_tri, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_pause_l, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_pause_r, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_play_tri, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_pause_l, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_pause_r, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* R108: 格式/品牌行 (静态占位, 真实采样率/码率/编码接入留待后续) */
    lv_label_set_text(lbl_fmt, "MP3|44KHZ|16bit|0320kbps SQ");

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

    /* 文件名: R105 改走曲名 canvas 点阵。
       R102 原用 cjk_request_text() 入队 + flush_cb 消费, 但该队列机制因导致
       TG1WDT 反复重启已被禁用 -> 队列无人消费 -> **曲名完全不显示**(回归 bug)。
       此处改用专用小 canvas, 修复回归。 */
    lv_obj_add_flag(lbl_track, LV_OBJ_FLAG_HIDDEN);   /* LVGL label 让位给点阵 */
    if (s_track_canvas && !s_menu_visible) {
        /* P1-fix: 曲名不变时跳过重渲染 (每秒 display_update 但曲名通常不变) */
        static char s_last_track[FILENAME_MAX_LEN] = "";
        const char *tn = (track_name && track_name[0]) ? track_name : " ";
        if (strcmp(s_last_track, tn) != 0) {
            strncpy(s_last_track, tn, FILENAME_MAX_LEN - 1);
            s_last_track[FILENAME_MAX_LEN - 1] = '\0';
            cjk_track_clear();
            cjk_track_text(tn);
        }
        lv_obj_clear_flag(s_track_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_track_canvas);
    } else if (s_track_canvas) {
        lv_obj_add_flag(s_track_canvas, LV_OBJ_FLAG_HIDDEN);  /* 菜单/browse 独占态隐藏 */
    }
    /* P1-fix: 移除全屏 invalidate — 旧 flush_cb 点阵队列已废弃, 现走 canvas,
       canvas 在上方已单独 invalidate, 各 label/bar 内容变化时自行 invalidate.
       全屏 invalidate 会导致 320x240 整屏刷新 (~120ms@10MHz SPI), 阻塞卷轴动画. */

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
    lv_label_set_text(lbl_percent, "");  /* P1-UI: 设计稿无百分比 */

    /* A-B 复读：底部灰栏显示 A/B 点 + 进度条标记 */
    if (ab_a < 0) {
        /* 未标记 A：完全隐藏 */
        lv_obj_add_flag(lbl_ab, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ab_mark_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ab_mark_b, LV_OBJ_FLAG_HIDDEN);
    } else {
        char abuf[10], bbuf[10];
        format_time(ab_a / 1000, abuf, sizeof(abuf));
        if (ab_b >= 0) {
            format_time(ab_b / 1000, bbuf, sizeof(bbuf));
        } else {
            snprintf(bbuf, sizeof(bbuf), "--:--");
        }
        char abtxt[48];
        if (ab_b >= 0) {
            snprintf(abtxt, sizeof(abtxt), "A:%s -> B:%s  %s",
                     abuf, bbuf, ab_on ? "LOOP ON" : "PAUSED");
        } else {
            snprintf(abtxt, sizeof(abtxt), "A:%s  B:PENDING", abuf);
        }
        lv_label_set_text(lbl_ab, abtxt);
        lv_obj_set_style_text_color(lbl_ab,
            ab_on ? lv_color_hex(0x2dd4bf) : lv_color_hex(0x8a93a6), 0);
        lv_obj_clear_flag(lbl_ab, LV_OBJ_FLAG_HIDDEN);

        /* 进度条上的 A/B 点标记（需要已知总时长才能定位） */
        if (total_sec > 0) {
            int64_t dur_ms = (int64_t)total_sec * 1000;
            int ap = (int)((int64_t)ab_a * 1000 / dur_ms);
            ap = ap < 0 ? 0 : (ap > 1000 ? 1000 : ap);
            int bp = (ab_b >= 0) ? (int)((int64_t)ab_b * 1000 / dur_ms) : ap;
            bp = bp < 0 ? 0 : (bp > 1000 ? 1000 : bp);
            int bar_w = DISPLAY_WIDTH - 2 * 8;
            lv_obj_set_x(ab_mark_a, 8 + (ap * bar_w) / 1000);
            lv_obj_set_x(ab_mark_b, 8 + (bp * bar_w) / 1000);
            lv_obj_clear_flag(ab_mark_a, LV_OBJ_FLAG_HIDDEN);
            if (ab_b >= 0) lv_obj_clear_flag(ab_mark_b, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(ab_mark_b, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ab_mark_a, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ab_mark_b, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 快进/快退 读秒（居中醒目，显示跳转方向与当前位置） */
    if (state == PLAYER_STATE_FAST_FORWARD || state == PLAYER_STATE_REWIND) {
        const char *dir = (state == PLAYER_STATE_FAST_FORWARD) ? "FF" : "RW";
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
    lv_label_set_text(lbl_hint, "REW PLAY FF STOP  |  PREV NEXT");

    /* 诊断：检查 g_player 可见性 + 子元素数 + lbl_status 实际状态 */
    if (call_count <= 3) {
        bool player_vis = !(lv_obj_has_flag(g_player, LV_OBJ_FLAG_HIDDEN));
        bool msg_vis    = !(lv_obj_has_flag(g_msg,    LV_OBJ_FLAG_HIDDEN));
        bool status_vis = !(lv_obj_has_flag(lbl_status, LV_OBJ_FLAG_HIDDEN));
        bool track_vis  = !(lv_obj_has_flag(lbl_track,  LV_OBJ_FLAG_HIDDEN));
        int player_children = (int)lv_obj_get_child_cnt(g_player);
        lv_area_t a;
        lv_obj_get_coords(lbl_status, &a);
        ESP_LOGW(TAG, "DBG: post-update #%u player_vis=%d msg_vis=%d status_vis=%d track_vis=%d player_kids=%d status_coords=(%d,%d,%d,%d) text='%.40s'",
                 (unsigned)call_count, player_vis, msg_vis, status_vis, track_vis, player_children,
                 a.x1, a.y1, a.x2, a.y2, lv_label_get_text(lbl_status));
    }

}

/* P1-fix: 公共入口 — main_task 只缓存数据 + 设标志, 不持 lv_lock,
   实际 LVGL 绘制由 lvgl_task 在持锁区消费 s_disp_update_pending 时执行。
   这消除了 main_task 长时间持锁导致 lvgl_task (及卷轴定时器) 被阻塞的根因。 */
void display_update(player_state_t state, const char *track_name,
                    int track_idx, int total,
                    int current_sec, int total_sec,
                    float speed, int gear, int volume)
{
    if (!g_display_initialized) return;
    s_disp_cache.state = state;
    if (track_name) {
        strncpy(s_disp_cache.track_name, track_name, FILENAME_MAX_LEN - 1);
        s_disp_cache.track_name[FILENAME_MAX_LEN - 1] = '\0';
    } else {
        s_disp_cache.track_name[0] = '\0';
    }
    s_disp_cache.track_idx = track_idx;
    s_disp_cache.total = total;
    s_disp_cache.current_sec = current_sec;
    s_disp_cache.total_sec = total_sec;
    s_disp_cache.speed = speed;
    s_disp_cache.gear = gear;
    s_disp_cache.volume = volume;
    s_disp_update_pending = true;
}

void display_show_browse(int selected, int total, char lines[][24], int count)
{
    if (!g_display_initialized || count <= 0) return;

    /* R105: 原实现把所有行拼成一个大串交给 ui_show_msg()(LVGL 字体路径),
       -> 中文文件名被渲染成"二维码"乱码。
       改为复用菜单的点阵 canvas 路径: "标题/行/提示" 结构与菜单完全一致,
       直接转调 display_show_menu(安全入口: 只缓存数据+置标志, 由 lvgl_task 持锁绘制)。 */
    char title[32];
    snprintf(title, sizeof(title), "Browse %d/%d", selected + 1, total);
    display_show_menu(title, lines, count, -1,
                      "UP/DN scroll   PREV/NEXT move   PLAY confirm   STOP exit");
}

/* R102-fix: 菜单绘制 —— main 侧只缓存数据 + 置标志, 绝不触碰 LVGL。
   (原实现直接调 lv_obj_add_flag/invalidate; 在 handle_button_events→menu_open→
    menu_render 路径下 main 并未持 lv_lock, 与 CPU1 的 lvgl_task 并发竞争,
    损坏 LVGL 内部结构 → lv_refr/lv_obj_pos 遍历死循环 → main 卡 30s → task_wdt。
    这与 R063/R097/R100 "main_task 完全不碰 LVGL" 是同一类问题, 沿用同一范式。) */
void display_show_menu(const char *title, char lines[][24], int count, int sel, const char *hint)
{
    (void)sel;
    if (!g_display_initialized || count <= 0) return;

    /* 仅缓存菜单内容到全局(纯内存写, 不触碰 LVGL) */
    s_menu_count = count;
    s_menu_sel   = sel;
    snprintf(s_menu_title, sizeof(s_menu_title), "%s",
             (title && title[0]) ? title : "Menu");
    int shown = count;
    if (shown > BROWSE_VISIBLE_LINES) shown = BROWSE_VISIBLE_LINES;
    for (int i = 0; i < shown; i++)
        snprintf(s_menu_lines[i], sizeof(s_menu_lines[i]), "%s", lines[i]);
    if (hint) snprintf(s_menu_hint, sizeof(s_menu_hint), "%s", hint);
    else      s_menu_hint[0] = '\0';
    s_menu_visible = true;

    /* 唤醒背光 (非 LVGL 操作, 可在此执行) */
    if (g_display_sleep) {
        display_set_brightness(s_last_brightness);
        g_display_sleep = false;
    }

    s_menu_pending = true;   /* 由 lvgl_task 持锁区消费 menu_apply_nolock() */
}

/* R102-fix: 菜单真正的 LVGL 绘制 —— 必须在 lv_lock() 内调用。
   调用点: (a) lvgl_task 消费 s_menu_pending  (b) display_update 持锁区。 */
static void menu_apply_nolock(void)
{
    if (!g_display_initialized || !s_menu_visible) return;

    /* 隐藏 LVGL 各屏, 让点阵菜单独占显示 */
    lv_obj_add_flag(g_msg,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ota,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ab_menu,LV_OBJ_FLAG_HIDDEN);

    /* R103: 用 canvas 画点阵菜单 (LVGL 原生位图路径, 不碰 SPI) */
    if (!s_cjk_canvas) return;

    const uint16_t fg = (uint16_t)lv_color_to_u16(lv_color_white());
    const uint16_t bg = (uint16_t)lv_color_to_u16(lv_color_hex(0x0a0e17));
    int shown = s_menu_count;
    if (shown > BROWSE_VISIBLE_LINES) shown = BROWSE_VISIBLE_LINES;

    cjk_canvas_clear();
    int y = 24;
    cjk_canvas_text(8, y, s_menu_title, fg, bg);    /* 标题 */
    y += 22;
    for (int i = 0; i < shown; i++) {               /* 菜单行 */
        cjk_canvas_text(8, y, s_menu_lines[i], fg, bg);
        y += 20;
    }
    if (s_menu_hint[0]) {                           /* 底部提示 */
        cjk_canvas_text(8, DISPLAY_HEIGHT - 20, s_menu_hint,
                        (uint16_t)lv_color_to_u16(lv_color_hex(0x8a93a6)), bg);
    }
    lv_obj_clear_flag(s_cjk_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_cjk_canvas);
}

/* R102: 菜单关闭时由宿主(app_menu_exit)通知 display 清缓存, 恢复 player 渲染 */
void display_menu_closed(void)
{
    s_menu_visible = false;
    cjk_clear_all();                 /* 清点阵掩码(纯 memset), 避免旧菜单点阵残留 */
    /* R102-fix: 原在此直接调 lv_obj_invalidate; 本函数由 app_menu_exit(main 任务, 无锁)
       调用, 同样违反"main 不碰 LVGL"。改为置标志, 由 lvgl_task 持锁区执行。 */
    s_menu_close_pending = true;
}

/* R051：A-B 复读状态屏 —— 迷你进度条(白A/橙B) + 实时状态 + 动作列表 */
void display_show_ab_menu(const char *title, char lines[][24], int count, int sel,
                          bool edit, int scrub,
                          int ab_a_ms, int ab_b_ms, bool ab_on,
                          int total_ms, int cur_ms, const char *hint)
{
    if (!g_display_initialized) return;
    lv_lock();   /* 保护整个 A-B 界面 LVGL 写: 与 lvgl_task 并发必须加锁 */

    /* 隐藏其它界面，独占显示 A-B 状态屏 */
    lv_obj_add_flag(g_msg,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ota,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_ab_menu, LV_OBJ_FLAG_HIDDEN);

    if (g_display_sleep) {          // 菜单活跃，唤醒背光
        display_set_brightness(s_last_brightness);
        g_display_sleep = false;
    }

    lv_label_set_text(abm_title, (title && title[0]) ? title : "A-B Repeat");

    /* 迷你进度条：填充 = 当前播放位置 */
    int cur_v = 0;
    if (total_ms > 0 && cur_ms >= 0) {
        cur_v = (int)((int64_t)cur_ms * 1000 / total_ms);
        if (cur_v < 0) cur_v = 0;
        if (cur_v > 1000) cur_v = 1000;
    }
    lv_bar_set_value(abm_bar, cur_v, LV_ANIM_OFF);

    /* A/B 标记定位（需已知总时长） */
    const int MARGIN = 8;
    int bar_w = DISPLAY_WIDTH - 2 * MARGIN;
    int ap = -1, bp = -1;
    if (total_ms > 0) {
        if (ab_a_ms >= 0) { ap = (int)((int64_t)ab_a_ms * 1000 / total_ms); ap = ap < 0 ? 0 : (ap > 1000 ? 1000 : ap); }
        if (ab_b_ms >= 0) { bp = (int)((int64_t)ab_b_ms * 1000 / total_ms); bp = bp < 0 ? 0 : (bp > 1000 ? 1000 : bp); }
    }
    if (ap >= 0) { lv_obj_set_x(abm_mark_a, MARGIN + (ap * bar_w) / 1000); lv_obj_clear_flag(abm_mark_a, LV_OBJ_FLAG_HIDDEN); }
    else         { lv_obj_add_flag(abm_mark_a, LV_OBJ_FLAG_HIDDEN); }
    if (bp >= 0) { lv_obj_set_x(abm_mark_b, MARGIN + (bp * bar_w) / 1000); lv_obj_clear_flag(abm_mark_b, LV_OBJ_FLAG_HIDDEN); }
    else         { lv_obj_add_flag(abm_mark_b, LV_OBJ_FLAG_HIDDEN); }

    /* 状态行：A:.. B:.. 复读:.. */
    char a_buf[10], b_buf[10];
    if (ab_a_ms >= 0) format_time(ab_a_ms / 1000, a_buf, sizeof(a_buf));
    else              snprintf(a_buf, sizeof(a_buf), "--:--");
    if (ab_b_ms >= 0) format_time(ab_b_ms / 1000, b_buf, sizeof(b_buf));
    else              snprintf(b_buf, sizeof(b_buf), "PENDING");
    char stat[48];
    snprintf(stat, sizeof(stat), "A:%s  B:%s  Repeat:%s", a_buf, b_buf, ab_on ? "ON" : "OFF");
    lv_label_set_text(abm_stat, stat);
    lv_obj_set_style_text_color(abm_stat, ab_on ? lv_color_hex(0x2dd4bf) : lv_color_hex(0x8a93a6), 0);

    /* 动作行（选中行高亮白字） */
    for (int i = 0; i < 4; i++) {
        if (i < count) lv_label_set_text(abm_lines[i], lines[i]);
        else           lv_label_set_text(abm_lines[i], "");
        bool sel_i = (i == sel);
        lv_obj_set_style_text_color(abm_lines[i],
            sel_i ? lv_color_hex(0xffffff) : lv_color_hex(0x8a93a6), 0);
    }

    lv_label_set_text(abm_hint, hint ? hint : "");
    lv_unlock();
}
void display_show_info(const char *title, const char *text)
{
    if (!g_display_initialized) return;

    static char buf[256];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "%s\n\n",
                    (title && title[0]) ? title : "Info");
    if (text) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s", text);
    }
    len += snprintf(buf + len, sizeof(buf) - len, "\n\n(Press any key to return)");

    if (g_display_sleep) {
        display_set_brightness(s_last_brightness);
        g_display_sleep = false;
    }
    ui_show_msg(buf);
}

/* ============================================================
 * 蓝牙音箱状态屏 (R050-BT)
 * ============================================================ */
void display_show_bt_status(const char *device_name, bool connected, int volume)
{
    if (!g_display_initialized) return;

    player_state_t st = connected ? PLAYER_STATE_PLAYING : PLAYER_STATE_STOPPED;
    const char *name = connected
        ? (device_name && device_name[0] ? device_name : "Connected")
        : "BT Speaker - Waiting...";

    /* 复用播放界面：设备名作为曲目名，无进度（BT 无文件/seek） */
    display_update(st, name, 0, 0, 0, 0, 0.0f, 0, volume);
    /* 覆盖底部提示为蓝牙操作说明 */
    lv_lock();
    lv_label_set_text(lbl_hint, "PLAY/PAUSE passthrough AVRCP   STOP exit");
    lv_unlock();
}

/* ============================================================
 * SD-OTA 升级界面 (R049c 真实化)
 * ============================================================ */
static void ui_show_ota(void)
{
    lv_obj_clear_flag(g_ota, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_msg, LV_OBJ_FLAG_HIDDEN);
    if (g_display_sleep) {
        display_set_brightness(s_last_brightness);
        g_display_sleep = false;
    }
}

void display_show_ota_confirm(const char *cur_ver, const char *new_ver,
                              long size_kb, const char *img_name, bool battery_ok)
{
    if (!g_display_initialized) return;

    static char buf[160];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "Current:  %s\n",
                    cur_ver ? cur_ver : "?");
    len += snprintf(buf + len, sizeof(buf) - len, "New:      %s\n",
                    (new_ver && new_ver[0]) ? new_ver : "Unknown");
    len += snprintf(buf + len, sizeof(buf) - len, "Image:    %s\n",
                    img_name ? img_name : "");
    len += snprintf(buf + len, sizeof(buf) - len, "Size:     %ld KB\n", size_kb);
    len += snprintf(buf + len, sizeof(buf) - len, "Battery:  %s",
                    battery_ok ? "OK" : "LOW - please charge");

    lv_label_set_text(ota_title, "Firmware Update");
    lv_label_set_text(ota_body, buf);
    lv_obj_add_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ota_hint, battery_ok ? "PLAY confirm upgrade   STOP cancel"
                                           : "Battery LOW, cannot upgrade (STOP back)");
    lv_lock();
    ui_show_ota();
    lv_unlock();
}

void display_show_ota_progress(int percent)
{
    if (!g_display_initialized) return;

    lv_label_set_text(ota_title, "Firmware Update");
    lv_label_set_text(ota_body, "Writing firmware...\nDO NOT power off / remove TF");
    lv_obj_clear_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(ota_bar, percent, LV_ANIM_OFF);
    char p[8];
    snprintf(p, sizeof(p), "%d%%", percent);
    lv_label_set_text(ota_pct, p);
    lv_label_set_text(ota_hint, "");
    lv_lock();
    ui_show_ota();
    lv_unlock();
}

void display_show_ota_done(void)
{
    if (!g_display_initialized) return;
    lv_label_set_text(ota_title, "Update OK");
    lv_label_set_text(ota_body, "Firmware written.\nRestart to apply.");
    lv_obj_add_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ota_hint, "Press any key to restart");
    lv_lock();
    ui_show_ota();
    lv_unlock();
}

void display_show_ota_error(const char *msg)
{
    if (!g_display_initialized) return;
    lv_label_set_text(ota_title, "Update FAILED");
    lv_label_set_text(ota_body, msg ? msg : "Unknown error");
    lv_obj_add_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ota_hint, "STOP back   PLAY retry");
    lv_lock();
    ui_show_ota();
    lv_unlock();
}
