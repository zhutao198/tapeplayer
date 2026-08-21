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
#define LV_COLOR_16_SWAP    0   /* 注: 本工程 main/lv_conf.h 未被 LVGL 组件真正包含, 此宏无效。
                                      端序补偿改在 display.cpp lvgl_flush_cb 内显式 SWAP16 实现。 */


/* 渲染引擎 (软件) */
#define LV_USE_DRAW_SW      1
#if LV_USE_DRAW_SW
#define LV_DRAW_SW_COMPLEX  1
#endif

/* 平台：FreeRTOS */
#define LV_USE_OS           LV_OS_FREERTOS

/* 内存：LVGL 用标准 malloc。已开启 CONFIG_SPIRAM_USE_MALLOC（ESP-IDF 5.5 中
   让 malloc() 也能返回 PSRAM 指针，与 SPIRAM_USE_CAPS_ALLOC 互斥），
   并设 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0，使 malloc() 优先分配 PSRAM
   （本机 8MB Octal PSRAM），LVGL 控件/对象与 freetype 字形缓存均落 PSRAM，
   仅 PSRAM 耗尽时回退内部 DRAM，避免挤占内部静态内存池。 */
#define LV_USE_STDLIB_MALLOC   1   /* LV_STDLIB_CLIB */
#define LV_MEM_SIZE            (256 * 1024)   /* 仅 BUILTIN 模式生效，此处保留 */

/* 中文 TTF 运行时渲染（字库位于独立 flash 分区 /font/cjk.ttf） */
#define LV_USE_FREETYPE                 0
#define LV_FREETYPE_USE_LVGL_PORT       0   /* 走标准 stdio，配合 FAT 分区挂载路径 */
#define LV_FREETYPE_CACHE_FT_GLYPH_CNT  1024
/* FreeType 推荐绘制线程栈 >=32KB（若启用绘制线程） */
#ifndef LV_DRAW_THREAD_STACK_SIZE
#define LV_DRAW_THREAD_STACK_SIZE       (32 * 1024)
#endif

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
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/* 允许挂载用户数据 */
#define LV_USE_USER_DATA    1

#endif /* LV_CONF_H */
