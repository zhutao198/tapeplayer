/**
 * @file button_manager.cpp
 * @brief 按键管理实现 - 状态机去抖 + 短按/双击/长按/超长按/持续按住检测
 *
 * 状态机扩展：
 * - IDLE → DEBOUNCE → PRESSED → (松开) → DBL_WAIT → (二次按下) → DOUBLE_CLICK
 *                                               → (超时) → SHORT_PRESS
 * - PRESSED → (≥500ms) → LONG_PRESS → HOLD → (≥3s) → EXTRA_LONG_PRESS → HOLD
 * - HOLD → (松开) → RELEASE
 */

#include "button_manager.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "button";

/* --- 按键状态机 --- */
typedef enum {
    BTN_STATE_IDLE,
    BTN_STATE_DEBOUNCE,
    BTN_STATE_PRESSED,
    BTN_STATE_DBL_WAIT,         // 等待第二次点击（双击判定窗口）
    BTN_STATE_DBL_DEBOUNCE,     // 第二次点击的去抖
    BTN_STATE_DBL_PRESSED,      // 第二次点击按下中
    BTN_STATE_LONG_PRESS,
    BTN_STATE_HOLD,
} btn_state_t;

typedef struct {
    gpio_num_t      gpio;
    btn_id_t        id;
    bool            dbl_click_en;      // 是否启用双击检测（FF/RW 禁用，避免磁带模式误判）
    btn_state_t     state;
    uint64_t        press_start_us;     // 当前状态下的按下起始时刻（IDLE→DEBOUNCE 或 DBL_WAIT→DBL_DEBOUNCE 时更新）
    uint64_t        last_scan_us;       // 上次扫描时刻
    uint64_t        first_release_us;   // 第一次松开时刻（双击判定用）
    bool            long_press_fired;   // 长按事件是否已触发
    bool            extra_long_fired;   // 超长按事件是否已触发
} btn_ctx_t;

/* 配置结构（纯配置，不含 runtime 状态） */
typedef struct {
    gpio_num_t      gpio;
    btn_id_t        id;
    bool            dbl_click_en;
} btn_config_t;

/* 按键配置：FF/RW 禁用双击检测，保证磁带模式即时响应 */
static const btn_config_t g_btn_config[BTN_ID_MAX] = {
    { .gpio = BTN_PLAY_PAUSE,    .id = BTN_ID_PLAY_PAUSE,   .dbl_click_en = true  },
    { .gpio = BTN_STOP,          .id = BTN_ID_STOP,         .dbl_click_en = true  },
    { .gpio = BTN_PREV,          .id = BTN_ID_PREV,         .dbl_click_en = false },
    { .gpio = BTN_NEXT,          .id = BTN_ID_NEXT,         .dbl_click_en = false },
    { .gpio = BTN_REWIND,        .id = BTN_ID_REWIND,       .dbl_click_en = false },
    { .gpio = BTN_FAST_FORWARD,  .id = BTN_ID_FAST_FORWARD, .dbl_click_en = false },
};

static btn_ctx_t g_buttons[BTN_ID_MAX];

/* ============================================================
 * 初始化
 * ============================================================ */
void button_manager_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    uint64_t pin_mask = 0;
    for (int i = 0; i < BTN_ID_MAX; i++) {
        pin_mask |= (1ULL << g_btn_config[i].gpio);
    }
    io_conf.pin_bit_mask = pin_mask;
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: 0x%x", ret);
        return;
    }

    // R033-313: 显式逐字段拷贝，避免 memcpy(g_buttons, g_btn_config) 因
    // btn_ctx_t 比 btn_config_t 多字段而越界读取配置数组（src 越界 UB）。
    for (int i = 0; i < BTN_ID_MAX; i++) {
        g_buttons[i].gpio            = g_btn_config[i].gpio;
        g_buttons[i].id              = g_btn_config[i].id;
        g_buttons[i].dbl_click_en    = g_btn_config[i].dbl_click_en;
        g_buttons[i].state           = BTN_STATE_IDLE;
        g_buttons[i].press_start_us  = 0;
        g_buttons[i].last_scan_us    = 0;
        g_buttons[i].first_release_us = 0;
        g_buttons[i].long_press_fired  = false;
        g_buttons[i].extra_long_fired  = false;
    }
}

/* ============================================================
 * 辅助：读取按键电平 (按下 = 低电平 = true)
 * ============================================================ */
static inline bool is_pressed(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == 0;
}

/* ============================================================
 * 扫描
 * ============================================================ */
int button_manager_scan(btn_event_info_t *events, int max_events)
{
    int count = 0;
    uint64_t now_us = esp_timer_get_time();
    uint32_t debounce_us    = BTN_DEBOUNCE_MS * 1000;
    uint32_t long_press_us  = BTN_LONG_PRESS_MS * 1000;
    uint32_t dbl_click_us   = BTN_DOUBLE_CLICK_MS * 1000;
    uint32_t extra_long_us  = BTN_EXTRA_LONG_MS * 1000;

    for (int i = 0; i < BTN_ID_MAX; i++) {
        btn_ctx_t *btn = &g_buttons[i];
        bool pressed = is_pressed(btn->gpio);
        btn_event_t event = BTN_EVENT_NONE;

        switch (btn->state) {

        case BTN_STATE_IDLE:
            if (pressed) {
                btn->state = BTN_STATE_DEBOUNCE;
                btn->press_start_us = now_us;
                btn->long_press_fired = false;
                btn->extra_long_fired = false;
            }
            break;

        case BTN_STATE_DEBOUNCE:
            if (!pressed) {
                btn->state = BTN_STATE_IDLE;
            } else if ((now_us - btn->press_start_us) >= debounce_us) {
                btn->state = BTN_STATE_PRESSED;
            }
            break;

        case BTN_STATE_PRESSED:
            if (!pressed) {
                /* 短按松开 */
                if (btn->dbl_click_en) {
                    /* 双击启用按键 → 进入双击等待窗口 */
                    btn->state = BTN_STATE_DBL_WAIT;
                    btn->first_release_us = now_us;
                } else {
                    /* 双击禁用按键 → 立即输出 SHORT_PRESS（0ms 延迟） */
                    btn->state = BTN_STATE_IDLE;
                    event = BTN_EVENT_SHORT_PRESS;
                }
            } else if ((now_us - btn->press_start_us) >= long_press_us) {
                btn->state = BTN_STATE_LONG_PRESS;
                event = BTN_EVENT_LONG_PRESS;
                btn->long_press_fired = true;
            }
            break;

        case BTN_STATE_DBL_WAIT:
            /* 在双击窗口内等待第二次按下 */
            if (pressed) {
                btn->state = BTN_STATE_DBL_DEBOUNCE;
                btn->press_start_us = now_us;
            } else if ((now_us - btn->first_release_us) >= dbl_click_us) {
                /* 超时：确认为单次短按 */
                btn->state = BTN_STATE_IDLE;
                event = BTN_EVENT_SHORT_PRESS;
            }
            break;

        case BTN_STATE_DBL_DEBOUNCE:
            if (!pressed && (now_us - btn->press_start_us) >= debounce_us) {
                btn->state = BTN_STATE_IDLE;
                event = BTN_EVENT_DOUBLE_CLICK;       // 第二次按下有效
            } else if (!pressed) {
                // R033-201：第二次按下在 debounce_us 内释放（抖动/短脉冲），
                // 去抖失败，不输出任何事件，仅回到 IDLE，避免幽灵 SHORT_PRESS。
                btn->state = BTN_STATE_IDLE;
            } else if ((now_us - btn->press_start_us) >= debounce_us) {
                btn->state = BTN_STATE_DBL_PRESSED;
            }
            break;

        case BTN_STATE_DBL_PRESSED:
            if (!pressed) {
                /* 第二次点击松开 → 双击确认 */
                btn->state = BTN_STATE_IDLE;
                event = BTN_EVENT_DOUBLE_CLICK;
            } else if ((now_us - btn->press_start_us) >= long_press_us) {
                /* 第二次点击也长按了 → 当作长按 */
                btn->state = BTN_STATE_LONG_PRESS;
                event = BTN_EVENT_LONG_PRESS;
                btn->long_press_fired = true;
            }
            break;

        case BTN_STATE_LONG_PRESS:
            if (!pressed) {
                btn->state = BTN_STATE_IDLE;
                event = BTN_EVENT_RELEASE;
            } else {
                btn->state = BTN_STATE_HOLD;
            }
            break;

        case BTN_STATE_HOLD:
            if (!pressed) {
                btn->state = BTN_STATE_IDLE;
                event = BTN_EVENT_RELEASE;
            } else {
                /* 检查是否达到超长按阈值（按键锁定 3s） */
                if (!btn->extra_long_fired &&
                    (now_us - btn->press_start_us) >= extra_long_us) {
                    btn->extra_long_fired = true;
                    event = BTN_EVENT_EXTRA_LONG_PRESS;
                } else {
                    event = BTN_EVENT_HOLD;
                }
            }
            break;
        }

        /* 如果有事件，加入输出 */
        if (event != BTN_EVENT_NONE && count < max_events) {
            events[count].id = btn->id;
            events[count].event = event;
            events[count].hold_ms = (uint32_t)((now_us - btn->press_start_us) / 1000);
            count++;
        }
    }

    return count;
}
