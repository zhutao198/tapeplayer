#include "font_partition.h"

#include "lvgl.h"
#include "esp_log.h"

static const char * TAG = "font_part";

/* LVGL 内置拉丁字体，作英文/数字兜底（子集未覆盖的字符自动回退到此） */
LV_FONT_DECLARE(lv_font_montserrat_14);
/* R101: 之前判断"v8 字体不兼容 v9"有误——核对 v9 源码确认 lv_font_t/cmap/glyph_dsc
 * 与 v8 完全一致，仅多一个 stride 字段。真正根因是 gen_font.py 的 cmap/stride/度量
 * bug，现已修正并重新生成。 */
extern lv_font_t lv_font_chinese_12;
extern lv_font_t lv_font_chinese_14;
extern lv_font_t lv_font_chinese_16;

static bool s_font_ready = false;  /* 子集字体是否就绪（此处恒为 false: freetype 不可用） */

void font_partition_init(void)
{
    /* R098h-final3: 子集字体 lv_font_chinese_* 存放在 flash 只读区（const lv_font_t），
     * 运行时对其 .fallback 字段做写操作会触发 Dbus write rejected -> Cache error 死机
     * (实测崩溃 PC 落在 font_partition_init，地址 0x3c109xxx)。故此处禁止写 flash
     * 字体结构体。子集字体自身已含常用中文 + ASCII 字形，无需挂 montserrat 兜底即可
     * 显示；生僻字留空但不死机。LV_USE_FREETYPE=0，cjk.ttf 分区/mmap/VFS 全部跳过。 */
    s_font_ready = false;
    ESP_LOGI(TAG, "freetype 禁用；子集字体只读不修改 fallback（避免 Cache error），"
                  "UI 用中文子集字体 lv_font_chinese_*");
}

/* 中文回退字体（freetype）是否就绪。未就绪时上层应避免渲染任何可能
 * 触发 freetype 路径的文本（如启动 splash 用 ASCII 兜底），否则未初始化的
 * freetype 会在 LVGL 渲染时死循环并触发 task_wdt。 */
bool font_partition_ready(void)
{
    return s_font_ready;
}
