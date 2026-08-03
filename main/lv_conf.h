/**
 * @file lv_conf.h
 * @brief LVGL v9 配置 (TapeBook: 原生 esp_lcd ST7789, RGB565)
 *
 * 由 ESP-IDF 的 LVGL 组件通过 #include "lv_conf.h" 引用（main/ 在 include 路径中）。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 颜色：RGB565, 与 esp_lcd bits_per_pixel=16 一致 */
#define LV_COLOR_DEPTH      16
#define LV_COLOR_16_SWAP    0

/* 渲染引擎 (软件) */
#define LV_USE_DRAW_SW      1
#if LV_USE_DRAW_SW
#define LV_DRAW_SW_COMPLEX  1
#endif

/* 平台：FreeRTOS */
#define LV_USE_OS           LV_OS_FREERTOS

/* 内存 */
#define LV_MEM_CUSTOM       0
#define LV_MEM_SIZE         (256 * 1024)
#define LV_MEM_ADDR         0

/* 心跳：由 display.cpp 的 LVGL 任务调用 lv_tick_inc() */
#define LV_TICK_CUSTOM      0

/* 日志 */
#define LV_USE_LOG          0
#if LV_USE_LOG
#define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF       1
#endif

/* UI 组件 */
#define LV_USE_WIDGETS      1

/* 字体：内置 Montserrat (拉丁) + 中文子集 (TapeBook UI 全中文) */
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(lv_font_chinese_12) \
    LV_FONT_DECLARE(lv_font_chinese_14) \
    LV_FONT_DECLARE(lv_font_chinese_16)
#define LV_FONT_DEFAULT         &lv_font_chinese_14

/* 允许挂载用户数据 */
#define LV_USE_USER_DATA    1

#endif /* LV_CONF_H */
