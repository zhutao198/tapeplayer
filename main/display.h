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

#define BROWSE_VISIBLE_LINES 6

/**
 * @brief 文件夹浏览列表
 * @param selected   当前选中索引 (0-based)
 * @param total      总文件数
 * @param lines      预格式化的显示行数组（每行含标记+文件名，如 "> song.mp3"）
 * @param count      行数
 */
void display_show_browse(int selected, int total, char lines[][24], int count);

/**
 * @brief 统一设置菜单显示 (R049)
 * @param title  当前层标题
 * @param lines  预格式化显示行数组（含标记+文本，如 "> 播放模式: 顺序播放"）
 * @param count  总行数
 * @param sel    当前选中索引 (未使用, 预留)
 * @param hint   底部操作提示
 */
void display_show_menu(const char *title, char lines[][24], int count, int sel, const char *hint);

/**
 * @brief A-B 复读状态屏 (R051)：迷你进度条(白A/橙B) + 实时状态 + 动作列表
 * @param title     标题（如 "A-B 复读"）
 * @param lines     动作行（含标记前缀，如 "> 标记 A 点"），每行 ≤23 字节
 * @param count     行数
 * @param sel       当前选中索引
 * @param edit      编辑态（复读开关）
 * @param scrub     微调态：0=无, 1=A, 2=B
 * @param ab_a_ms   A 点(ms)，-1 未标记
 * @param ab_b_ms   B 点(ms)，-1 未标记
 * @param ab_on     复读开关是否开启
 * @param total_ms  总时长(ms)，0=未知（此时不定位 A/B 标记）
 * @param cur_ms    当前播放位置(ms)，用于进度条填充
 * @param hint      底部操作提示
 */
void display_show_ab_menu(const char *title, char lines[][24], int count, int sel,
                          bool edit, int scrub,
                          int ab_a_ms, int ab_b_ms, bool ab_on,
                          int total_ms, int cur_ms, const char *hint);

/**
 * @brief 通用信息屏 (R049c 桩功能提示，如 OTA/USB/关于)
 * @param title 标题（居中首行）
 * @param text  多行正文（'\n' 分隔，居中）
 */
void display_show_info(const char *title, const char *text);

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

#ifdef __cplusplus
}
#endif
