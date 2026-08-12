/**
 * @file menu.h
 * @brief 统一设置菜单 (R049) — 通用菜单模型与导航 API
 *
 * 交互规则（菜单内全局一致）:
 *   PREV/NEXT  上下移动选项
 *   VOL±       改当前项的值 (MI_TOGGLE)
 *   播放       进入子菜单 / 触发动作 / (TOGGLE) 步进值
 *   停止       返回上一级; 根级停止 = 退出菜单
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "button_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 菜单项类型 */
typedef enum {
    MI_SUBMENU,   // 子菜单: PLAY 进入, 下层是一组 menu_item
    MI_TOGGLE,    // 可切换值: VOL± / PLAY 步进, 值由 get_idx/set_idx 管理
    MI_ACTION,    // 动作: PLAY 触发 on_enter (进入浏览 / 确认弹窗等)
} menu_item_kind_t;

typedef struct menu_item menu_item_t;

/** 通用菜单项 */
struct menu_item {
    const char *label;          // 显示名
    menu_item_kind_t kind;
    /* MI_TOGGLE */
    const char **options;       // 选项字符串数组 (option_count 个)
    int          option_count;
    int  (*get_idx)(void);      // 当前选项索引
    void (*set_idx)(int i);     // 设值 (持久化在此做)
    /* MI_SUBMENU */
    const menu_item_t *children; // 子层菜单项数组
    int          child_count;
    /* MI_ACTION */
    void (*on_enter)(void);     // PLAY 触发
};

/** 初始化菜单模块 */
void menu_init(void);

/** 打开菜单 (重置到根) */
void menu_open(void);

/** 关闭菜单 */
void menu_close(void);

/** 菜单是否打开 */
bool menu_is_open(void);

/** 处理一批按键事件 (菜单打开时由 main 转发) */
void menu_handle_button(const btn_event_info_t *events, int n);

#ifdef __cplusplus
}
#endif
