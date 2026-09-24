/* AUTO-GENERATED */
#pragma once
#include <stdint.h>

typedef struct { uint16_t unicode; uint16_t idx; } cjk_map_t;
extern const uint8_t cjk_font_raw[];
extern const cjk_map_t cjk_unicode_map[];
extern const int cjk_unicode_map_num;
extern const int cjk_font_glyph_bytes;
extern const int cjk_font_w;
extern const int cjk_font_h;

/* 兼容 draw_cjk_text 使用的宏名 */
#define CJK_FONT_W  cjk_font_w
#define CJK_FONT_H  cjk_font_h
#define CJK_FONT_GLYPH_BYTES cjk_font_glyph_bytes

/* 渲染接口 (实现见 cjk_font.c, C 链接) */
#ifdef __cplusplus
extern "C" {
#endif
int  cjk_unicode_to_glyph_idx(uint32_t u);
void cjk_glyph_render(uint16_t *fb, int idx, uint16_t fg, uint16_t bg);
#ifdef __cplusplus
}
#endif
