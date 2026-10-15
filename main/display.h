/**
 * @file display.h
 * @brief ST7789 TFT 显示模块 (原生 esp_lcd + LVGL v9)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 播放状态 */
typedef enum {
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_FAST_FORWARD,
    PLAYER_STATE_REWIND,
} player_state_t;

/**
 * @brief 初始化 TFT 显示屏 (esp_lcd ST7789 + LVGL)
 */
void display_init(void);

/**
 * @brief 启动 LVGL 渲染任务（须在 display_init 及所有初始化期 LVGL 写操作完成、
 *        进入主循环前调用，避免 main_task 与 lvgl_task 并发访问 LVGL 对象树死锁）
 */
void display_start_lvgl_task(void);

/**
 * @brief 更新屏幕显示
 * @param state       播放状态
 * @param track_name  曲目名称
 * @param track_idx   当前曲目索引 (1-based)
 * @param total       总曲目数
 * @param current_sec 当前播放秒数
 * @param total_sec   总时长秒数 (0=未知)
 * @param speed       播放速度倍率
 * @param gear        加速档位 (0=正常)
 * @param volume      音量 level 0..14 (15 档逻辑音量)
 */
void display_update(player_state_t state,
                    const char *track_name,
                    int track_idx, int total,
                    int current_sec, int total_sec,
                    float speed, int gear, int volume);

/**
 * @brief 显示启动画面
 */
void display_show_splash(void);

/**
 * @brief 显示"无音频文件"提示
 */
void display_show_no_files(void);

/**
 * @brief 显示"无 SD 卡"提示
 */
void display_show_no_card(void);

/**
 * @brief 清除全屏消息并返回播放器界面（插卡成功/正常播放时调用）
 */
void display_clear_msg(void);

/**
 * @brief R111: 底部临时提示 (A-B标记等), 3秒自动消失
 * @param msg 提示文本 (ASCII)
 */
void display_toast(const char *msg);

/**
 * @brief 设置 TF 卡在位状态 (状态栏 SD 图标 + 插拔瞬时提示)
 * @param present true=已插入, false=已弹出
 */
void display_set_sd_present(bool present);

/**
 * @brief 仅初始化 TF 卡图标状态 (无插拔提示, 用于开机)
 * @param present true=已插入, false=已弹出
 */
void display_set_sd_present_init(bool present);

/**
 * @brief 设置屏幕背光亮度 (0-100)
 * @note TFT_BLK (IO15) 由 LEDC PWM 控制
 */
void display_set_brightness(int percent);

/**
 * @brief 控制 LCD 软件电源 (PMOS 软开关, IO39)
 * @param on true=上电显示, false=断电省电
 */
void display_power(bool on);

/**
 * @brief 设置当前播放模式 (供主界面状态栏显示)
 * @param mode 0=顺序(SEQ) 1=全部循环(ALL) 2=单曲循环(ONE)
 */
void display_set_play_mode(int mode);

/**
 * @brief 触发音量条显示 (调节音量时调用)
 * @note 调用后音量条立即显示最新音量, 并在停止调节 3 秒后自动隐藏
 * @param volume 当前音量 level 0..VOLUME_LEVEL_MAX (15 档逻辑音量)
 */
void display_show_volume(int volume);

#define BROWSE_VISIBLE_LINES 10

/**
 * @brief 文件夹浏览列表
 * @param selected   当前选中索引 (0-based)
 * @param total      总文件数
 * @param lines      预格式化的显示行数组（每行含标记+文件名，如 "> song.mp3"）
 * @param count      行数
 */
void display_show_browse(int selected, int total, char lines[][64], int count);

/**
 * @brief 统一设置菜单显示 (R049)
 * @param title  当前层标题
 * @param lines  预格式化显示行数组（含标记+文本，如 "> 播放模式: 顺序播放"）
 * @param count  总行数
 * @param sel    当前选中索引 (未使用, 预留)
 * @param hint   底部操作提示
 */
/** R111: 菜单显示项 (结构化) */
typedef enum {
    MENU_DISP_ACTION = 0,
    MENU_DISP_SUBMENU,
    MENU_DISP_TOGGLE,
} menu_disp_kind_t;

typedef struct {
    const char *label;
    menu_disp_kind_t kind;
    const char *value;
} menu_disp_item_t;

void display_show_menu(const char *title, const menu_disp_item_t *items, int count, int sel, const char *hint);

/**
 * @brief 菜单关闭通知 (R102): 清点阵菜单缓存, 恢复 player 渲染
 *        须在退出菜单时(如 app_menu_exit)调用
 */
void display_menu_closed(void);

/**
 * @brief 通用信息屏 (R049c 桩功能提示，如 OTA/USB/关于)
 * @param title 标题（居中首行）
 * @param text  多行正文（'\n' 分隔，居中）
 */
void display_show_info(const char *title, const char *text);

/**
 * @brief R161: 点阵 canvas 文本页（关于/说明）—— 中文友好
 *
 * 与文件列表/菜单同一套路：点阵字库渲染（cjk_canvas_text），不走 LVGL 标签
 * （本项目 LVGL 中文路径坏，R098h-hotfix：ui_font v8 格式与 v9.5 不兼容，
 *  汉字显示为方框——R159 曾因此被迫把关于屏改英文，本函数恢复中文）。
 * 常驻显示直到 display_textpage_dismiss()；“任意键返回”由调用方处理
 * （菜单打开时 menu_handle_button 顶部拦截，见 menu.cpp）。
 *
 * @param title 标题（顶部栏，紫色）
 * @param text  多行正文（'\n' 分隔；每行按显示宽度截断，全角=2格/ASCII=1格，
 *               上限 36 格≈288px，不截断 UTF-8 多字节序列）
 */
void display_show_text_page(const char *title, const char *text);

/** R161: 点阵文本页是否正在显示（菜单据此拦截按键） */
bool display_textpage_active(void);

/** R161: 关闭点阵文本页（调用方随后应重绘菜单） */
void display_textpage_dismiss(void);

/**
 * @brief 打印 PSRAM/DRAM 堆水位与当前屏 LVGL 对象数（内存诊断，每 10s 自动触发）
 */
void display_mem_report(void);

/* ---- 蓝牙音箱状态屏（R050-BT）---- */
void display_show_bt_status(const char *device_name, bool connected, int volume);

/**
 * @brief 注册主循环显示更新回调（由 lvgl_task 持有 LVGL 锁时调用）
 * @note 用于消除 main_task 与 lvgl_task 并发访问 LVGL 导致的死锁与看门狗超时。
 *       main 不再直接调 LVGL API，改设为 dirty 由 lvgl_task 在自己的循环里调度。
 */
typedef void (*display_main_tick_fn_t)(void);
void display_register_main_tick(display_main_tick_fn_t fn);
/** 设置 dirty 标志，请求 lvgl_task 在下一帧调用注册的 main tick 回调 */
void display_request_main_tick(void);

/* ---- SD-OTA 升级界面（R049c 真实化）---- */
void display_show_ota_confirm(const char *cur_ver, const char *new_ver,
                              long size_kb, const char *img_name, bool battery_ok);
void display_show_ota_progress(int percent);
void display_show_ota_done(void);
void display_show_ota_error(const char *msg);

/**
 * @brief R115: 显示关机倒计时提示 (居中覆盖)
 * @param type      0=无, 1=低电量, 2=定时关机
 * @param remaining 剩余秒数
 */
void display_show_shutdown_countdown(int type, int remaining);

#ifdef __cplusplus
}
#endif
