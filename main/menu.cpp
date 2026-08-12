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

/* R049b / R049c 菜单动作（本文件实现，调用 audio_player / main） */
void app_ab_mark_a(void);
void app_ab_mark_b(void);
void app_ab_clear(void);
void app_ota_enter(void);
void app_usb_enter(void);
void app_about_enter(void);
void app_bookmark_enter(void);

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

/* ---- TOGGLE 数据: EQ (R049d 桩) ---- */
static const char *s_eq_opts[] = {"关闭", "流行", "摇滚", "古典", "人声"};

/* ---- 语音播报 / EQ 仅持久化，无效果 (R049d 桩) ---- */
static int voice_get_idx(void) { return settings_load_voice() ? 1 : 0; }
static void voice_set_idx(int i) { settings_save_voice(i != 0); }
static int eq_get_idx(void) { return settings_load_eq(); }
static void eq_set_idx(int i) { settings_save_eq(i); }

/* ---- R049c 按键提示音开关 ---- */
static int key_beep_get_idx(void) { return settings_load_key_beep() ? 1 : 0; }
static void key_beep_set_idx(int i) { settings_save_key_beep(i != 0); }

/* ---- R049b A-B 复读开关 ---- */
static int ab_get_idx(void) { return audio_player_is_ab_enabled() ? 1 : 0; }
static void ab_set_idx(int i) { audio_player_set_ab_enabled(i != 0); }

/* ============================================================
 * 菜单树（完整：R049a 已落地 + R049b/c 接入 + R049d 桩）
 * ============================================================ */
static const menu_item_t g_play_sub[] = {
    { "播放模式", MI_TOGGLE, s_mode_opts, 3, app_get_play_mode, app_set_play_mode, NULL, 0, NULL },
    { "定时关机", MI_TOGGLE, s_timer_opts, 5, timer_get_idx,    timer_set_idx,    NULL, 0, NULL },
};

/* R049b：A-B 复读子菜单 */
static const menu_item_t g_ab_sub[] = {
    { "标记 A 点", MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_ab_mark_a },
    { "标记 B 点", MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_ab_mark_b },
    { "复读开关",   MI_TOGGLE, s_onoff_opts, 2, ab_get_idx, ab_set_idx, NULL, 0, NULL },
    { "清除",       MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_ab_clear },
};

/* R049c / R049d：系统子菜单 */
static const menu_item_t g_system_sub[] = {
    { "A-B 复读",   MI_SUBMENU, NULL, 0, NULL, NULL, g_ab_sub,    4, NULL },
    { "固件升级",   MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_ota_enter },
    { "USB 存储",   MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_usb_enter },
    { "按键提示音", MI_TOGGLE,  s_onoff_opts, 2, key_beep_get_idx, key_beep_set_idx, NULL, 0, NULL },
    { "语音播报",   MI_TOGGLE,  s_onoff_opts, 2, voice_get_idx,    voice_set_idx,    NULL, 0, NULL },
    { "EQ",         MI_TOGGLE,  s_eq_opts,    5, eq_get_idx,       eq_set_idx,       NULL, 0, NULL },
    { "关于",       MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_about_enter },
};

static const menu_item_t g_root[] = {
    { "浏览文件", MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_enter_browse },
    { "播放",     MI_SUBMENU, NULL, 0, NULL, NULL, g_play_sub,   2, NULL },
    { "书签",     MI_ACTION,  NULL, 0, NULL, NULL, NULL, 0, app_bookmark_enter },
    { "系统",     MI_SUBMENU, NULL, 0, NULL, NULL, g_system_sub, 7, NULL },
};
static const int g_root_count = 4;

/* ============================================================
 * R049b/c 动作实现
 * ============================================================ */
void app_ab_mark_a(void)   { audio_player_mark_a(); }
void app_ab_mark_b(void)   { audio_player_mark_b(); }
void app_ab_clear(void)    { audio_player_clear_ab(); }
void app_ota_enter(void)   { app_enter_ota(); }
void app_usb_enter(void)   { app_show_info("USB 存储", "大容量存储模式\n需 USB OTG\n功能未开放"); }
void app_about_enter(void) { app_show_info("关于", "有声书播放器\nESP32-S3\nV1.1 · R049"); }
void app_bookmark_enter(void) { app_show_info("书签", "书签列表管理\n开发中 (R049d)"); }

/* ============================================================
 * 导航状态
 * ============================================================ */
static bool        s_open = false;
static menu_level_t s_stack[MENU_MAX_DEPTH];
static int         s_depth = 0;

/* 前向声明 */
static void menu_render(void);

void menu_init(void)
{
    s_open = false;
    s_depth = 0;
}

void menu_open(void)
{
    s_open = true;
    s_depth = 1;
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

static void menu_render(void)
{
    if (!s_open) return;
    menu_level_t *lv = &s_stack[s_depth - 1];
    char lines[BROWSE_VISIBLE_LINES][24];
    int shown = lv->count;
    if (shown > BROWSE_VISIBLE_LINES) shown = BROWSE_VISIBLE_LINES;
    for (int i = 0; i < shown; i++) {
        const menu_item_t *it = &lv->items[i];
        if (it->kind == MI_TOGGLE && it->get_idx) {
            int idx = it->get_idx();
            const char *val = (idx >= 0 && idx < it->option_count) ? it->options[idx] : "";
            snprintf(lines[i], sizeof(lines[i]), "%s %s: %s",
                     (i == lv->sel) ? ">" : " ", it->label, val);
        } else {
            snprintf(lines[i], sizeof(lines[i]), "%s %s",
                     (i == lv->sel) ? ">" : " ", it->label);
        }
    }
    display_show_menu(lv->title, lines, lv->count, lv->sel,
                      "PREV/NEXT 移动  VOL 调值  播放 进入  停止 返回");
}

void menu_handle_button(const btn_event_info_t *events, int n)
{
    for (int k = 0; k < n; k++) {
        const btn_event_info_t *e = &events[k];
        if (e->event == BTN_EVENT_NONE) continue;
        app_play_beep();   // R049c：菜单内按键提示音（设置开启时）

        menu_level_t *lv = &s_stack[s_depth - 1];
        const menu_item_t *it = &lv->items[lv->sel];

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
            if (e->event == BTN_EVENT_SHORT_PRESS && it->kind == MI_TOGGLE && it->set_idx) {
                int idx = it->get_idx();
                idx = (idx - 1 + it->option_count) % it->option_count;
                it->set_idx(idx);
                menu_render();
            }
            break;

        case BTN_ID_VOL_UP:
            if (e->event == BTN_EVENT_SHORT_PRESS && it->kind == MI_TOGGLE && it->set_idx) {
                int idx = it->get_idx();
                idx = (idx + 1) % it->option_count;
                it->set_idx(idx);
                menu_render();
            }
            break;

        case BTN_ID_PLAY_PAUSE:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
                if (it->kind == MI_SUBMENU) {
                    if (s_depth < MENU_MAX_DEPTH) {
                        s_stack[s_depth].items = it->children;
                        s_stack[s_depth].count = it->child_count;
                        s_stack[s_depth].sel   = 0;
                        s_stack[s_depth].title = it->label;
                        s_depth++;
                        menu_render();
                    }
                } else if (it->kind == MI_ACTION && it->on_enter) {
                    it->on_enter();   // 宿主负责关闭菜单并切换 app 状态
                } else if (it->kind == MI_TOGGLE && it->set_idx) {
                    int idx = it->get_idx();
                    idx = (idx + 1) % it->option_count;
                    it->set_idx(idx);
                    menu_render();
                }
            }
            break;

        case BTN_ID_STOP:
            if (e->event == BTN_EVENT_SHORT_PRESS) {
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
