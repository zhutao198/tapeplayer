/**
 * @file menu.cpp
 * @brief 统一设置菜单 (R049) — 通用菜单状态机 + 菜单树
 *
 * 全二级功能都是"菜单树节点 + 一个 handler"，交互全局唯一（见 menu.h）。
 * 本文件自包含菜单模型与导航；叶子动作通过 main.cpp 提供的宿主回调衔接。
 */

#include "menu.h"
#include "button_manager.h"
#include "display.h"
#include "settings.h"
#include "power_mgmt.h"
#include "audio_player.h"
#include "bookmark.h"
#include "esp_log.h"
#include <cstdio>

static const char *TAG = "menu";

/* 由 main.cpp 提供的宿主回调 (非 static, C++ 链接) */
void app_menu_exit(void);
void app_enter_browse(void);
int  app_get_play_mode(void);
void app_set_play_mode(int m);
void app_show_info(const char *title, const char *text);
void app_play_beep(void);
void app_enter_ota(void);
int  app_get_current_track_idx(void);
void app_bookmark_add_current(void);
void app_bookmark_jump(int position_s);

/* R049b / R049c 菜单动作（本文件实现，调用 audio_player / main） */
void app_ota_enter(void);
void app_usb_enter(void);
void app_about_enter(void);
void app_bookmark_enter(void);
void bookmark_fill_and_enter(void);  /* R114: 前向声明 */

/* ============================================================
 * 菜单模型
 * ============================================================ */
typedef struct {
    const menu_item_t *items;   // 当前层菜单项数组
    int          count;
    int          sel;           // 当前选中索引
    const char  *title;         // 当前层标题
} menu_level_t;

#define MENU_MAX_DEPTH 8

/* ---- TOGGLE 数据: 定时关机 ---- */
static const char *s_timer_opts[] = {"关", "15 分钟", "30 分钟", "60 分钟", "90 分钟"};
static const int   s_timer_vals[] = {0, 15, 30, 60, 90};
static int timer_get_idx(void)
{
    int m = settings_load_auto_off();
    for (int i = 0; i < 5; i++) {
        if (s_timer_vals[i] == m) return i;
    }
    return 0;
}
static void timer_set_idx(int i)
{
    int m = s_timer_vals[i];
    settings_save_auto_off(m);
    power_mgmt_set_auto_off(m);   // 引擎已就绪: 落地即"武装"定时关机
    ESP_LOGI(TAG, "Auto-off set: %d min", m);
}

/* ---- TOGGLE 数据: 播放模式 ---- */
static const char *s_mode_opts[] = {"顺序播放", "列表循环", "单曲循环"};

/* ---- TOGGLE 数据: 开关 (R049c) ---- */
static const char *s_onoff_opts[] = {"关", "开"};

/* ---- 语音播报 (桩) ---- */
static int voice_get_idx(void) { return settings_load_voice() ? 1 : 0; }
static void voice_set_idx(int i) { settings_save_voice(i != 0); }

/* ---- R049c 按键提示音开关 ---- */
static int key_beep_get_idx(void) { return settings_load_key_beep() ? 1 : 0; }
static void key_beep_set_idx(int i) { settings_save_key_beep(i != 0); }

/* ============================================================
 * 菜单树（完整：R049a 已落地 + R049b/c 接入 + R049d 桩）
 * ============================================================ */
/* R115: 播放模式改为列表选择式(动态子菜单), 不再用TOGGLE */
void mode_fill_and_enter(void);  /* 前向声明 */

/* R049c / R049d：系统子菜单（蓝牙音箱在 USE_BT_SPEAKER 时置于此，占原 A-B 复读位置） */
static const menu_item_t g_system_sub[] = {
#if defined(CONFIG_USE_BT_SPEAKER)
    { "蓝牙音箱", MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_enter_bt_speaker },
#endif
    { "固件升级",   MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_ota_enter },
    { "关于",       MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_about_enter },
    { "定时关机",   MI_TOGGLE,  s_timer_opts, 5, timer_get_idx,    timer_set_idx,    NULL, 0, NULL },
};

static const menu_item_t g_root[] = {
    { "浏览文件", MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_enter_browse },
    { "书签",     MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_bookmark_enter },
    { "播放模式", MI_ACTION, NULL, 0, NULL, NULL, NULL, 0, mode_fill_and_enter },
    { "系统设置", MI_SUBMENU, NULL, 0, NULL, NULL, g_system_sub,
#if defined(CONFIG_USE_BT_SPEAKER)
        4, NULL },
#else
        3, NULL },
#endif
};
static const int g_root_count = 4;   // 根菜单顺序：浏览文件/书签/播放模式/系统设置 (R113: 移除A-B复读菜单项, 仅保留快捷键)

/* ============================================================
 * R049b/c 动作实现
 * ============================================================ */
void app_ota_enter(void)   { app_enter_ota(); }
void app_usb_enter(void)   { app_show_info("USB 存储", "大容量存储模式\n需 USB OTG\n功能未开放"); }
void app_about_enter(void) { app_show_info("关于", "有声书播放器\nESP32-S3\nV1.1 · R049"); }
void app_bookmark_enter(void) { bookmark_fill_and_enter(); }

/* ============================================================
 * 导航状态
 * ============================================================ */
static bool        s_open = false;
static menu_level_t s_stack[MENU_MAX_DEPTH];
static int         s_depth = 0;
static bool        s_edit = false;   // R050：TOGGLE 编辑态（VOL± 调值，PLAY/STOP 退出）

/* 前向声明 */
static void menu_render(void);

/* ============================================================
 * 书签动态子菜单 (R114)
 * ============================================================ */
static menu_item_t s_bookmark_items[BOOKMARK_MAX_PER_FILE + 1];
static char s_bookmark_labels[BOOKMARK_MAX_PER_FILE + 1][24];

static void bookmark_fill_items(void)
{
    int track = app_get_current_track_idx();
    bookmark_t bms[BOOKMARK_MAX_PER_FILE];
    int n = bookmark_get_all(track, bms, BOOKMARK_MAX_PER_FILE);

    snprintf(s_bookmark_labels[0], sizeof(s_bookmark_labels[0]), "+ 添加当前位置");
    s_bookmark_items[0].label = s_bookmark_labels[0];
    s_bookmark_items[0].kind = MI_ACTION;
    s_bookmark_items[0].on_enter = NULL;  /* 添加项特殊处理 */
    s_bookmark_items[0].options = NULL;
    s_bookmark_items[0].option_count = 0;
    s_bookmark_items[0].get_idx = NULL;
    s_bookmark_items[0].set_idx = NULL;
    s_bookmark_items[0].children = NULL;
    s_bookmark_items[0].child_count = 0;

    for (int i = 0; i < n; i++) {
        int pos = bms[i].position_s;
        snprintf(s_bookmark_labels[i + 1], sizeof(s_bookmark_labels[i + 1]),
                 "%02d:%02d", pos / 60, pos % 60);
        s_bookmark_items[i + 1].label = s_bookmark_labels[i + 1];
        s_bookmark_items[i + 1].kind = MI_ACTION;
        s_bookmark_items[i + 1].on_enter = NULL;
        s_bookmark_items[i + 1].options = NULL;
        s_bookmark_items[i + 1].option_count = 0;
        s_bookmark_items[i + 1].get_idx = NULL;
        s_bookmark_items[i + 1].set_idx = NULL;
        s_bookmark_items[i + 1].children = NULL;
        s_bookmark_items[i + 1].child_count = 0;
    }
}

void bookmark_fill_and_enter(void)
{
    bookmark_fill_items();
    int track = app_get_current_track_idx();
    bookmark_t bms[BOOKMARK_MAX_PER_FILE];
    int n = bookmark_get_all(track, bms, BOOKMARK_MAX_PER_FILE);

    if (s_depth < MENU_MAX_DEPTH) {
        s_stack[s_depth].items = s_bookmark_items;
        s_stack[s_depth].count = n + 1;
        s_stack[s_depth].sel   = 0;
        s_stack[s_depth].title = "书签";
        s_depth++;
        s_edit = false;
        menu_render();
    }
}

/* ============================================================
 * 播放模式动态子菜单 (R115): 列表选择式
 * ============================================================ */
static menu_item_t s_mode_items[3];
static const char *s_mode_labels[3] = {"顺序播放", "列表循环", "单曲循环"};

void mode_fill_and_enter(void)
{
    int cur = app_get_play_mode();
    for (int i = 0; i < 3; i++) {
        s_mode_items[i].label = s_mode_labels[i];
        s_mode_items[i].kind = MI_ACTION;
        s_mode_items[i].on_enter = NULL;  /* 特殊处理 */
        s_mode_items[i].options = NULL;
        s_mode_items[i].option_count = 0;
        s_mode_items[i].get_idx = NULL;
        s_mode_items[i].set_idx = NULL;
        s_mode_items[i].children = NULL;
        s_mode_items[i].child_count = 0;
    }

    if (s_depth < MENU_MAX_DEPTH) {
        s_stack[s_depth].items = s_mode_items;
        s_stack[s_depth].count = 3;
        s_stack[s_depth].sel   = (cur >= 0 && cur < 3) ? cur : 0;
        s_stack[s_depth].title = "播放模式";
        s_depth++;
        s_edit = false;
        menu_render();
    }
}

void menu_init(void)
{
    s_open = false;
    s_depth = 0;
}

void menu_open(void)
{
    s_open = true;
    s_depth = 1;
    s_edit = false;
    s_stack[0].items = g_root;
    s_stack[0].count = g_root_count;
    s_stack[0].sel   = 0;
    s_stack[0].title = "菜单";
    menu_render();
}

void menu_close(void)
{
    s_open = false;
    s_depth = 0;
}

bool menu_is_open(void)
{
    return s_open;
}

void menu_refresh(void)
{
    if (s_open) menu_render();
}

static void menu_render(void)
{
    if (!s_open) return;
    menu_level_t *lv = &s_stack[s_depth - 1];

    /* R111: 结构化菜单项 (支持序号/子菜单箭头/TOGGLE值右对齐) */
    menu_disp_item_t items[BROWSE_VISIBLE_LINES];
    int shown = lv->count;
    if (shown > BROWSE_VISIBLE_LINES) shown = BROWSE_VISIBLE_LINES;
    for (int i = 0; i < shown; i++) {
        const menu_item_t *it = &lv->items[i];
        items[i].label = it->label;
        if (it->kind == MI_SUBMENU) {
            items[i].kind = MENU_DISP_SUBMENU;
            items[i].value = NULL;
        } else if (it->kind == MI_TOGGLE && it->get_idx) {
            items[i].kind = MENU_DISP_TOGGLE;
            int idx = it->get_idx();
            items[i].value = (idx >= 0 && idx < it->option_count) ? it->options[idx] : "";
        } else {
            items[i].kind = MENU_DISP_ACTION;
            items[i].value = NULL;
        }
    }
    const char *hint = s_edit
        ? "编辑中  VOL+ 调值  PLAY/STOP 完成"
        : "PREV/NEXT/VOL 选择  PLAY 进入  STOP 返回";
    display_show_menu(lv->title, items, lv->count, lv->sel, hint);
}

void menu_handle_button(const btn_event_info_t *events, int n)
{
    for (int k = 0; k < n; k++) {
        const btn_event_info_t *e = &events[k];
        if (e->event == BTN_EVENT_NONE) continue;

        menu_level_t *lv = &s_stack[s_depth - 1];
        const menu_item_t *it = &lv->items[lv->sel];

        /* ---- 编辑态：VOL± 调值，PLAY/STOP/PREV/NEXT 退出 ---- */
        if (s_edit) {
            if (e->event != BTN_EVENT_SHORT_PRESS) continue;
            switch (e->id) {
            case BTN_ID_VOL_DOWN:
                if (it->kind == MI_TOGGLE && it->set_idx) {
                    int idx = it->get_idx();
                    idx = (idx - 1 + it->option_count) % it->option_count;
                    it->set_idx(idx);
                    menu_render();
                }
                break;
            case BTN_ID_VOL_UP:
                if (it->kind == MI_TOGGLE && it->set_idx) {
                    int idx = it->get_idx();
                    idx = (idx + 1) % it->option_count;
                    it->set_idx(idx);
                    menu_render();
                }
                break;
            case BTN_ID_PLAY_PAUSE:
            case BTN_ID_STOP:
            case BTN_ID_PREV:
            case BTN_ID_NEXT:
                s_edit = false;          // 退出编辑（确认/取消/收起）
                menu_render();
                break;
            default:
                break;
            }
            continue;   // 编辑态已消费该事件
        }

        /* ---- 浏览态 ---- */
        switch (e->id) {
        case BTN_ID_PREV:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                lv->sel = (lv->sel - 1 + lv->count) % lv->count;
                menu_render();
            }
            break;

        case BTN_ID_NEXT:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                lv->sel = (lv->sel + 1) % lv->count;
                menu_render();
            }
            break;

        case BTN_ID_VOL_DOWN:
            if (e->event == BTN_EVENT_SHORT_PRESS) {   /* R111: 往下拨 = 向下 */
                lv->sel = (lv->sel + 1) % lv->count;
                menu_render();
            }
            break;

        case BTN_ID_VOL_UP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {   /* R111: 往上拨 = 向上 */
                lv->sel = (lv->sel - 1 + lv->count) % lv->count;
                menu_render();
            }
            break;

        case BTN_ID_PLAY_PAUSE:
            /* R115: 书签子菜单 — 长按PLAY删除当前选中书签 */
            if (e->event == BTN_EVENT_LONG_PRESS &&
                lv->items == s_bookmark_items && lv->sel > 0) {
                int idx = lv->sel - 1;
                int track = app_get_current_track_idx();
                bookmark_t bms[BOOKMARK_MAX_PER_FILE];
                int n = bookmark_get_all(track, bms, BOOKMARK_MAX_PER_FILE);
                if (idx >= 0 && idx < n) {
                    bookmark_delete(track, bms[idx].slot);
                    ESP_LOGI(TAG, "Bookmark deleted: track=%d slot=%d pos=%ds", track, bms[idx].slot, bms[idx].position_s);
                    bookmark_fill_items();
                    n = bookmark_get_all(track, bms, BOOKMARK_MAX_PER_FILE);
                    lv->count = n + 1;
                    if (lv->sel > n) lv->sel = n;  /* 删除最后一项时调整选中 */
                    menu_render();
                }
                break;
            }
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                if (it->kind == MI_SUBMENU) {
                    if (s_depth < MENU_MAX_DEPTH) {
                        s_stack[s_depth].items = it->children;
                        s_stack[s_depth].count = it->child_count;
                        s_stack[s_depth].sel   = 0;
                        s_stack[s_depth].title = it->label;
                        s_depth++;
                        s_edit = false;
                        // R050：仅含单个 TOGGLE 的子菜单（如「播放模式」）自动进入编辑态
                        const menu_item_t *top = &s_stack[s_depth - 1].items[0];
                        if (s_stack[s_depth - 1].count == 1 &&
                            top->kind == MI_TOGGLE && top->set_idx) {
                            s_edit = true;
                        }
                        menu_render();
                    }
                } else if (it->kind == MI_ACTION && it->on_enter) {
                    it->on_enter();   // 动作由宿主处理
                } else if (it->kind == MI_ACTION && it->on_enter == NULL &&
                           lv->items == s_bookmark_items) {
                    /* R114: 书签子菜单 */
                    if (lv->sel == 0) {
                        /* 添加当前位置 */
                        app_bookmark_add_current();
                        bookmark_fill_items();
                        int track = app_get_current_track_idx();
                        bookmark_t bms[BOOKMARK_MAX_PER_FILE];
                        int n = bookmark_get_all(track, bms, BOOKMARK_MAX_PER_FILE);
                        lv->count = n + 1;
                        menu_render();
                    } else {
                        /* 跳转到书签位置 */
                        int idx = lv->sel - 1;
                        bookmark_t bms[BOOKMARK_MAX_PER_FILE];
                        int track = app_get_current_track_idx();
                        int n = bookmark_get_all(track, bms, BOOKMARK_MAX_PER_FILE);
                        if (idx >= 0 && idx < n) {
                            app_bookmark_jump(bms[idx].position_s);
                        }
                    }
                } else if (it->kind == MI_ACTION && it->on_enter == NULL &&
                           lv->items == s_mode_items) {
                    /* R115: 播放模式子菜单 -> 选中确认并返回 */
                    app_set_play_mode(lv->sel);
                    ESP_LOGI(TAG, "Play mode set: %d (%s)", lv->sel, s_mode_labels[lv->sel]);
                    /* 返回上一级 */
                    if (s_depth > 1) {
                        s_depth--;
                        menu_render();
                    }
                } else if (it->kind == MI_TOGGLE && it->set_idx) {
                    s_edit = true;    // 列表·TOGGLE：PLAY 进入编辑态
                    menu_render();
                }
            }
            break;

        case BTN_ID_STOP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                s_edit = false;
                if (s_depth > 1) {
                    s_depth--;
                    menu_render();
                } else {
                    app_menu_exit();
                }
            }
            break;

        default:
            break;
        }
    }
}
