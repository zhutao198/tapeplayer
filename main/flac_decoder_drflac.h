/*
 * flac_decoder_drflac.h
 *
 * C 方案: 用 dr_flac (mackron, public domain) 自托管 FLAC 解码器, 替代 esp_codec / 闭源
 * flac_decoder。dr_flac 仅分配少量内部状态(几 KB), 绝不分配大 PSRAM 缓冲, 因此不会像
 * esp_codec(256KB out_buf 越界) 或闭源 flac_decoder 那样踩坏同在 PSRAM 的 LVGL 堆。
 *
 * 数据流: file ringbuffer -> drflac_read_pcm_frames_s16 -> decoder ringbuffer -> i2s。
 * dr_flac 通过 audio_element_input 回调从上游 in_rb 流式读 FLAC 字节(不可 seek, 顺序解码)。
 */
#pragma once

#include "audio_element.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  task_stack;     /*!< 解码任务栈大小 (字节), -1 用默认 */
    int  out_rb_size;    /*!< 输出 ringbuffer 大小 (字节) */
    bool stack_in_ext;   /*!< 栈是否分配到 PSRAM (保留在 PSRAM: DRAM 仅 284KB 放不下大栈) */
} flac_decoder_drflac_cfg_t;

/* R141 定论: FLAC 崩溃真凶与 dr_flac 解码器/解码任务栈无关 —— 是 audio_player.cpp 的
 * flac_parse_duration_ms 等嗅探函数在 8KB main 任务栈上开 64KB/4KB 大数组导致 main 栈溢出,
 * 踩坏 DRAM 堆与 PSRAM LVGL(详见 audio_player.cpp R141)。dr_flac 自身仅分配少量 DRAM 内部
 * 状态, 不碰 PSRAM。解码任务栈保留在 PSRAM(有 8MB), 128KB 对 dr_flac 足够。 */
#define DEFAULT_FLAC_DECODER_DRFLAC_CONFIG() { \
    .task_stack   = 128 * 1024, \
    .out_rb_size  = 64 * 1024,  \
    .stack_in_ext = true,        \
}

audio_element_handle_t flac_decoder_drflac_init(flac_decoder_drflac_cfg_t *config);

#ifdef __cplusplus
}
#endif
