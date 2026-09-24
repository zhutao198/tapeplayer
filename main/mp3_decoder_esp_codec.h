/*
 * mp3_decoder_esp_codec.h
 *
 * R076-CODEC-7: 用 esp_audio_codec 组件自带的 *开源* MP3 解码器
 * (esp_mp3_dec_*) 替代 ADF 闭源的 PV-MP3 (libesp_processing.a)。
 *
 * 背景：
 *   ADF 的 mp3_decoder 元素使用闭源 PV-MP3 库，在解码特定 MP3 文件时
 *   于 mp3_decoder_open 内部崩溃 (Guru Meditation / BREAK / DoubleException)，
 *   且 PV-MP3 闭源无法 patch / 升级。
 *   esp_audio_codec 组件自带一套独立的 MP3 解码器 (esp_mp3_dec_open/close/...)，
 *   已编译进 libesp_audio_codec.a，与 PV-MP3 完全独立，且开源可维护。
 *
 * 本文件提供一个 audio_element wrapper，把 esp_mp3_dec_* 接入 audio_pipeline，
 * 使 file -> mp3_decoder_esp_codec -> i2s 链路彻底绕开 PV-MP3。
 */
#pragma once

#include "audio_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  MP3 decoder 配置 (基于 esp_audio_codec 开源解码器)
 */
typedef struct {
    int task_stack;      /*!< 解码任务栈大小 (字节), -1 用默认 */
    int out_rb_size;     /*!< 输出 ringbuffer 大小 (字节) */
    bool stack_in_ext;   /*!< 栈是否分配到 PSRAM (建议 false, 避免 Harvard 冲突) */
} mp3_decoder_esp_codec_cfg_t;

#define DEFAULT_MP3_DECODER_ESP_CODEC_CONFIG() { \
    .task_stack   = 8 * 1024,   \
    .out_rb_size  = 16 * 1024,  \
    .stack_in_ext = false,       \
}

/**
 * @brief  创建基于 esp_mp3_dec_* 的 MP3 decoder audio_element
 *
 * @param[in]  config  解码器配置
 * @return     audio_element handle, 失败返回 NULL
 */
audio_element_handle_t mp3_decoder_esp_codec_init(mp3_decoder_esp_codec_cfg_t *config);

#ifdef __cplusplus
}
#endif
