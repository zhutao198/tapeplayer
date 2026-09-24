#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化独立 flash 字库分区：
 *   1) 以只读 FAT 方式挂载 font 分区到 /font
 *   2) 启动 freetype 并从 /font/cjk.ttf 创建中文回退字体
 *   3) 把该字体挂到现有子集字体 (lv_font_chinese_*) 的 fallback 链末端
 *
 * 必须在 lv_init() 之后、任何 label 渲染之前调用一次。
 * 字库分区里的 cjk.ttf 需由 host 侧用 tools/make_font_image.py 生成并通过
 * esptool 烧录到 font 分区（详见 flash_font.bat）。换 SD 卡不影响中文显示。
 */
void font_partition_init(void);

/* 中文回退字体（freetype）是否就绪。未就绪时上层应使用 ASCII 兜底文本，
 * 避免渲染触发未初始化的 freetype 路径导致 LVGL 死循环 + task_wdt。 */
bool font_partition_ready(void);

#ifdef __cplusplus
}
#endif
