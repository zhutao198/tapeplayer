/*
 * flac_decoder_esp_codec.h
 *
 * R122-FLAC-CODEC: 用 esp_audio_codec 组件自带的开源 FLAC 解码器 (esp_flac_dec_*)
 * 替代 ADF 闭源的 flac_decoder (libesp_processing.a)。
 *
 * 背景：
 *   ADF 闭源 FLAC 解码器在解码 FLAC(无论 16/24bit)时内部 PCM 缓冲越界,
 *   污染 PSRAM/LVGL 对象, 导致 Guru Meditation(LoadProhibited)。闭源无法 patch。
 *   esp_audio_codec 的 simple_dec(FLAC) 路径与已验证可用的 MP3 解码器同源,
 *   能正确输出 16/24bit PCM 并经 music_info 上报, 彻底绕开崩溃。
 */
#pragma once

#include "audio_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  FLAC decoder 配置 (基于 esp_audio_codec 开源解码器)
 */
typedef struct {
    int task_stack;      /*!< 解码任务栈大小 (字节), -1 用默认 */
    int out_rb_size;     /*!< 输出 ringbuffer 大小 (字节) */
    bool stack_in_ext;   /*!< 栈是否分配到 PSRAM (建议 false, 避免 Harvard 冲突) */
} flac_decoder_esp_codec_cfg_t;

/* R122-FLAC-CODEC: esp_codec FLAC 解码器为软件解码, README 建议运行任务栈 ~20K。
   R135: 48K 栈溢出, 放大到 96K。
   R-FIX: 96K 强制片内(ext=0)在 ESP32-S3(DRAM≈284KB)上 xTaskCreate 分配不出连续片内
   栈 -> "Error creating task decoder" 整首被 skip。改回 stack_in_ext=true 放 PSRAM(8MB
   充足); 且 LVGL 对象已由 sdkconfig SPIRAM_MALLOC_ALWAYSINTERNAL=16384 隔离到片内 DRAM,
   PSRAM 上的解码栈/缓冲再也踩不到 LVGL, 不再 Guru Meditation。 */
#define DEFAULT_FLAC_DECODER_ESP_CODEC_CONFIG() { \
    .task_stack   = 96 * 1024,  \
    .out_rb_size  = 16 * 1024,  \
    .stack_in_ext = true,        \
}

/**
 * @brief  创建基于 esp_flac_dec_* 的 FLAC decoder audio_element
 *
 * @param[in]  config  解码器配置
 * @return     audio_element handle, 失败返回 NULL
 */
audio_element_handle_t flac_decoder_esp_codec_init(flac_decoder_esp_codec_cfg_t *config);

#ifdef __cplusplus
}
#endif
