/*
 * aac_decoder_esp_codec.h
 *
 * R122-AAC-CODEC: 用 esp_audio_codec 组件自带的开源 AAC 解码器 (esp_aac_dec_*)
 * 替代 ADF 闭源的 aac_decoder (libesp_processing.a)。
 *
 * 背景：
 *   原 ADF aac_decoder 为预编译闭源库，虽能播 ADTS AAC，但不符合项目"全开源解码器"
 *   取向（MP3 已用 Helix、FLAC 已用 esp_codec）。esp_audio_codec 的 simple_dec(AAC)
 *   路径与已验证的 FLAC wrapper 同构，集中走开源解码器。
 *   仅支持 ADTS 裸流 AAC；MP4 容器(m4a)由 aac_probe_is_adts 在 play() 中预先拒绝
 *   （容器解封装需另行实现，见讨论）。
 */
#pragma once

#include "audio_element.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  AAC decoder 配置 (基于 esp_audio_codec 开源解码器)
 */
typedef struct {
    int task_stack;      /*!< 解码任务栈大小 (字节), -1 用默认 */
    int out_rb_size;     /*!< 输出 ringbuffer 大小 (字节) */
    bool stack_in_ext;   /*!< 栈是否分配到 PSRAM (R145: 改 true, 规避长播 DRAM 碎片化致解码任务创建失败; FLAC ext=1 在 S3 已验证可正常工作) */
} aac_decoder_esp_codec_cfg_t;

/* R122-AAC-CODEC: esp_codec AAC 解码器为软件解码, README 建议运行任务栈 ~20K。
   故默认 24K (留余量); out_rb 16K 避免 44.1k 立体声欠载爆音。 */
#define DEFAULT_AAC_DECODER_ESP_CODEC_CONFIG() { \
    .task_stack   = 48 * 1024,  \
    .out_rb_size  = 16 * 1024,  \
    .stack_in_ext = true,        \
}

/**
 * @brief  创建基于 esp_aac_dec_* 的 AAC decoder audio_element
 *
 * @param[in]  config  解码器配置
 * @return     audio_element handle, 失败返回 NULL
 */
audio_element_handle_t aac_decoder_esp_codec_init(aac_decoder_esp_codec_cfg_t *config);

#ifdef __cplusplus
}
#endif
