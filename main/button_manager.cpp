/**
 * @file button_manager.cpp
 * @brief 按键管理实现 - 状态机去抖 + 短按/长按/超长按/持续按住检测
 *
 * 状态机（双击检测已移除，所有按键统一走短按路径）：
 * - IDLE → DEBOUNCE → PRESSED → (松开) → SHORT_PRESS
 * - PRESSED → (≥500ms) → LONG_PRESS → HOLD → (≥3s) → EXTRA_LONG_PRESS → HOLD
 * - HOLD → (松开) → RELEASE
 *
 * R047：dbl_click_en 字段 + 三个双击状态已删除（所有按键 dbl_click_en=false，
 * 双击分支永不执行）。如未来需启用双击，按下列接口位置恢复：
 *   - btn_config_t::dbl_click_en
 *   - 状态枚举 BTN_STATE_DBL_WAIT/DBL_DEBOUNCE/DBL_PRESSED
 *   - PRESSED 释放分支的 dbl_click_en 判断
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
    BTN_STATE_LONG_PRESS,
    BTN_STATE_HOLD,
} btn_state_t;

typedef struct {
    gpio_num_t      gpio;
    btn_id_t        id;
    btn_state_t     state;
    uint64_t        press_start_us;     // 当前状态下的按下起始时刻
    uint64_t        last_scan_us;       // 上次扫描时刻
    bool            long_press_fired;   // 长按事件是否已触发
    bool            extra_long_fired;   // 超长按事件是否已触发
} btn_ctx_t;

/* 按键配置 */
static const struct {
    gpio_num_t      gpio;
    btn_id_t        id;
} g_btn_config[BTN_ID_MAX] = {
    { .gpio = BTN_PLAY_PAUSE,    .id = BTN_ID_PLAY_PAUSE   },
    { .gpio = BTN_STOP,          .id = BTN_ID_STOP         },
    { .gpio = BTN_PREV,          .id = BTN_ID_PREV         },
    { .gpio = BTN_NEXT,          .id = BTN_ID_NEXT         },
    { .gpio = BTN_REWIND,        .id = BTN_ID_REWIND       },
    { .gpio = BTN_FAST_FORWARD,  .id = BTN_ID_FAST_FORWARD },
    { .gpio = BTN_VOL_DOWN,      .id = BTN_ID_VOL_DOWN     },
    { .gpio = BTN_VOL_UP,        .id = BTN_ID_VOL_UP       },
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

    for (int i = 0; i < BTN_ID_MAX; i++) {
        g_buttons[i].gpio            = g_btn_config[i].gpio;
        g_buttons[i].id              = g_btn_config[i].id;
        g_buttons[i].state           = BTN_STATE_IDLE;
        g_buttons[i].press_start_us  = 0;
        g_buttons[i].last_scan_us    = 0;
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
    uint32_t extra_long_us  = BTN_EXTRA_LONG_MS * 1000;

    /* 诊断：每次扫描都打印所有按键 GPIO 实时电平（0=按下, 1=松开）
       临时关闭以验证 BTN 刷屏日志是否拖垮主循环（方案B） */
#if 0
    {
        char line[160] = {0};
        int off = 0;
        for (int j = 0; j < BTN_ID_MAX; j++) {
            int lvl = gpio_get_level(g_buttons[j].gpio);
            off += snprintf(line + off, sizeof(line) - off, "G%d=%d ", g_buttons[j].gpio, lvl);
        }
        ESP_LOGW("BTN", "DBG: gpio levels (0=pressed): %s", line);
    }
#endif

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
                /* 短按松开：直接输出 SHORT_PRESS（双击已移除） */
                btn->state = BTN_STATE_IDLE;
                event = BTN_EVENT_SHORT_PRESS;
            } else if ((now_us - btn->press_start_us) >= long_press_us) {
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
