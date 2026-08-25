/*
 * mp3_decoder_esp_codec.c
 *
 * R076-CODEC-14: 回退到 ADF release/v2.x 官方推荐方案
 *
 *   官方示例 pipeline_sdcard_mp3_control/main/play_sdcard_mp3_control_example.c
 *   使用闭源 PV-MP3 (mp3_decoder_init) 作为 MP3 decoder element，
 *   配套的 link_tags 为 "file", "mp3", "i2s"。
 *
 *   之前 R076-CODEC-7 ~ 13 一路追开源 esp_audio_codec / esp_audio_simple_dec_*，
 *   反复遇到崩溃 / 静音 / 快速切歌等问题，根因复杂（vtable / es_parser / 注册
 *   流程 / es_parser v1.0.0 vs v1.0.1 多版本混搭）。
 *
 *   用户 (2026-08-25) 指出: "版本组合乱了是根因, SDK 本身不至于这么多严重 bug"
 *   决定: 全部用官方, 不再自创兼容层. 即便 PV-MP3 在某些 MP3 上偶发崩溃
 *   (DoubleException, R076-CODEC-6 已确认) 也是官方已知状态.
 *
 * 音频链路: file -> mp3 -> i2s  (全部用 ADF release/v2.x 标准 API)
 *   - audio_element 的 tag 改回 "mp3" 与 ADF 示例一致
 *   - 用 ADF 官方 mp3_decoder_init, 自动 link libesp_processing.a (PV-MP3)
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "mp3_decoder.h"           // R076-CODEC-14: ADF release/v2.x 官方 PV-MP3 element
#include "mp3_decoder_esp_codec.h" // 本组件头文件 (配置结构体定义)

static const char *TAG = "MP3_DECODER";

/* 配置默认参数 (参考 play_sdcard_mp3_control 示例 + 我们的 stack/out_rb 配置) */
audio_element_handle_t mp3_decoder_esp_codec_init(mp3_decoder_esp_codec_cfg_t *config)
{
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();

    /* task_stack / out_rb_size 留 caller 改; 默认 DEFAULT_MP3_DECODER_CONFIG 是
     *   - task_stack  = 8 * 1024
     *   - out_rb_size = 8 * 1024
     * 我们之前调到 32K stack 仍未消除崩溃 (PV-MP3 偶发崩), 这次先回退默认.
     * caller 若有特殊需要可从 config 覆盖. */
    if (config && config->task_stack > 0) {
        mp3_cfg.task_stack = config->task_stack;
    }
    if (config && config->out_rb_size > 0) {
        mp3_cfg.out_rb_size = config->out_rb_size;
    }
    if (config) {
        mp3_cfg.stack_in_ext = config->stack_in_ext;
    }

    audio_element_handle_t el = mp3_decoder_init(&mp3_cfg);
    if (!el) {
        ESP_LOGE(TAG, "mp3_decoder_init failed (ADF PV-MP3 element)");
        return NULL;
    }
    ESP_LOGI(TAG, "mp3_decoder element created (ADF release/v2.x PV-MP3, stack=%u, out_rb=%u)",
             (unsigned)mp3_cfg.task_stack, (unsigned)mp3_cfg.out_rb_size);
    return el;
}
