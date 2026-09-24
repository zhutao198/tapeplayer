/**
 * @file button_manager.h
 * @brief 按键管理模块 - 扫描、去抖、短按、长按、超长按、持续按住检测
 *
 * R047：双击检测已移除（BTN_EVENT_DOUBLE_CLICK 已删除）。所有按键统一走
 * 短按路径。如未来需启用双击，恢复 BTN_EVENT_DOUBLE_CLICK 并配合
 * button_manager.cpp 中的双击状态机。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 按键事件类型 */
typedef enum {
    BTN_EVENT_NONE = 0,             // 无事件
    BTN_EVENT_SHORT_PRESS,          // 短按 (松开时立即触发，无双击窗口)
    BTN_EVENT_LONG_PRESS,           // 长按 (按住超过 500ms 时触发一次)
    BTN_EVENT_EXTRA_LONG_PRESS,     // 超长按 (按住超过 3s，用于按键锁定)
    BTN_EVENT_HOLD,                 // 持续按住 (每扫描周期持续触发)
    BTN_EVENT_RELEASE,              // 松开 (从按住变为松开)
} btn_event_t;

/** 按键ID枚举 */
typedef enum {
    BTN_ID_PLAY_PAUSE = 0,
    BTN_ID_STOP,
    BTN_ID_PREV,
    BTN_ID_NEXT,
    BTN_ID_REWIND,
    BTN_ID_FAST_FORWARD,
    BTN_ID_VOL_DOWN,
    BTN_ID_VOL_UP,
    BTN_ID_MAX
} btn_id_t;

/** 单个按键事件 */
typedef struct {
    btn_id_t    id;             // 按键ID
    btn_event_t event;          // 事件类型
    uint32_t    hold_ms;        // 已按住时长 (ms)，所有事件均填充
} btn_event_info_t;

/**
 * @brief 初始化按键 GPIO（配置内部上拉）
 */
void button_manager_init(void);

/**
 * @brief 扫描按键，获取事件
 * @param events    输出事件数组
 * @param max_events 数组最大容量
 * @return 实际获取的事件数量
 */
int button_manager_scan(btn_event_info_t *events, int max_events);

#ifdef __cplusplus
}
#endif
