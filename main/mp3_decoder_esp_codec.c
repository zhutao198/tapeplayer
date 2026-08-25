/*
 * mp3_decoder_esp_codec.c
 *
 * R076-CODEC-17: 单路径用 espressif/esp_audio_codec v2.6.2 (component manager)
 *
 * 用户决策 (2026-08-25): '用最新稳定版, 不再混搭'
 *
 * 完全依赖 component manager 拉的 v2.6.2 (含 esp_mp3_dec_* + esp_audio_simple_dec_*)。
 * 不再手工 link ADF 内置的 libesp_audio_codec.a / libesp_audio_simple_dec.a。
 * 之前 R076-CODEC-7~13 双链路冲突是当前唯一可能造成偶发 DoubleException 的原因。
 *
 * 数据流 (v2.6.2 path):
 *   file ringbuffer -> esp_audio_simple_dec_process
 *     -> mpeg_parser (v1.0.1) 切完整 MP3 frame
 *     -> esp_mp3_dec_decode 解码 PCM
 *     -> decoder ringbuffer -> i2s_stream -> MAX98357A
 *
 * 注意：v2.6.2 之前测试时崩在 es_parser v1.0.1 内部 (R076-CODEC-11/13)。
 * 本次赌单路径能消除双链路冲突, 让 v1.0.1 parser 在无冲突环境下正常工作。
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "audio_element.h"
#include "esp_audio_simple_dec.h"   // simple_dec API
#include "esp_mp3_dec.h"             // 注册 MP3 decoder 到全局表
#include "audio_type_def.h"
#include "mp3_decoder_esp_codec.h"

static const char *TAG = "MP3_ESP_CODEC";

/* 单次从 input ringbuffer 读取的 MP3 数据量 (官方推荐 2KB) */
#define MP3_IN_READ_SIZE    (2048)
/* 解码输出 PCM 缓冲区大小 (单帧 PCM 最大 4608B) */
#define MP3_OUT_FRAME_SIZE  (8192)

typedef struct {
    esp_audio_simple_dec_handle_t dec_handle;
    uint8_t *in_buf;
    uint8_t *out_buf;
    bool info_reported;
} mp3_esp_codec_t;

static esp_err_t _mp3_esp_codec_open(audio_element_handle_t self)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)audio_element_getdata(self);
    if (!dec) return ESP_FAIL;

    /* R076-CODEC-11 关键修复: 注册 MP3 decoder 到全局表,
     * 否则 simple_dec 内部 esp_audio_dec_open 查不到 MP3 类型返回 -7 */
    static bool s_registered = false;
    if (!s_registered) {
        esp_audio_err_t reg_ret = esp_mp3_dec_register();
        if (reg_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "esp_mp3_dec_register failed: %d", reg_ret);
            return ESP_FAIL;
        }
        s_registered = true;
        ESP_LOGI(TAG, "registered esp_mp3_dec (once)");
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type      = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg       = NULL,
        .cfg_size      = 0,
        .use_frame_dec = false,  // 启用 mpeg_parser 自动切帧
    };
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &dec->dec_handle);
    if (ret != ESP_AUDIO_ERR_OK || !dec->dec_handle) {
        ESP_LOGE(TAG, "esp_audio_simple_dec_open failed: %d", ret);
        return ESP_FAIL;
    }

    dec->in_buf  = (uint8_t *)malloc(MP3_IN_READ_SIZE);
    dec->out_buf = (uint8_t *)malloc(MP3_OUT_FRAME_SIZE);
    if (!dec->in_buf || !dec->out_buf) {
        ESP_LOGE(TAG, "out of memory for decode buffers");
        if (dec->in_buf)  free(dec->in_buf);
        if (dec->out_buf) free(dec->out_buf);
        dec->in_buf = NULL;
        dec->out_buf = NULL;
        esp_audio_simple_dec_close(dec->dec_handle);
        dec->dec_handle = NULL;
        return ESP_FAIL;
    }

    dec->info_reported = false;
    ESP_LOGI(TAG, "esp_audio_simple_dec(MP3, parser=auto) opened (handle %p)", dec->dec_handle);
    return ESP_OK;
}

static audio_element_err_t _mp3_esp_codec_process(audio_element_handle_t self,
                                                  char *el_buffer, int el_buf_len)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)audio_element_getdata(self);
    if (!dec || !dec->dec_handle) {
        return AEL_PROCESS_FAIL;
    }

    int rlen = audio_element_input(self, (char *)dec->in_buf, MP3_IN_READ_SIZE);
    if (rlen <= 0) {
        if (rlen == AEL_IO_DONE) {
            return AEL_IO_DONE;
        }
        return (audio_element_err_t)rlen;
    }

    esp_audio_simple_dec_raw_t raw = {
        .buffer   = dec->in_buf,
        .len      = (uint32_t)rlen,
        .eos      = false,
        .consumed = 0,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = dec->out_buf,
        .len    = MP3_OUT_FRAME_SIZE,
    };

    while (raw.len > 0) {
        frame.decoded_size = 0;
        frame.needed_size  = 0;
        esp_audio_err_t ret = esp_audio_simple_dec_process(dec->dec_handle, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (frame.needed_size > MP3_OUT_FRAME_SIZE) {
                uint8_t *nb = (uint8_t *)realloc(dec->out_buf, frame.needed_size);
                if (nb) {
                    dec->out_buf = nb;
                    frame.buffer = nb;
                    frame.len    = frame.needed_size;
                    continue;
                }
            }
            break;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "esp_audio_simple_dec_process err %d, skip %u bytes",
                     ret, raw.len);
            break;
        }
        if (frame.decoded_size > 0) {
            int wlen = audio_element_output(self, (char *)frame.buffer, (int)frame.decoded_size);
            if (wlen <= 0) {
                return (audio_element_err_t)wlen;
            }
            if (!dec->info_reported) {
                esp_audio_simple_dec_info_t info = {0};
                if (esp_audio_simple_dec_get_info(dec->dec_handle, &info) == ESP_AUDIO_ERR_OK
                    && info.sample_rate > 0) {
                    audio_element_info_t music_info = {0};
                    music_info.sample_rates = info.sample_rate;
                    music_info.channels     = info.channel;
                    music_info.bits         = info.bits_per_sample;
                    music_info.codec_fmt    = ESP_CODEC_TYPE_MP3;
                    audio_element_setinfo(self, &music_info);
                    audio_element_report_info(self);
                    dec->info_reported = true;
                    ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch, %d bit",
                             info.sample_rate, info.channel, info.bits_per_sample);
                }
            }
        }
        if (raw.consumed == 0) {
            break;
        }
        raw.buffer += raw.consumed;
        raw.len    -= raw.consumed;
    }

    return AEL_IO_OK;
}

static esp_err_t _mp3_esp_codec_close(audio_element_handle_t self)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)audio_element_getdata(self);
    if (dec) {
        if (dec->dec_handle) {
            esp_audio_simple_dec_close(dec->dec_handle);
            dec->dec_handle = NULL;
        }
        if (dec->in_buf)  { free(dec->in_buf);  dec->in_buf = NULL; }
        if (dec->out_buf) { free(dec->out_buf); dec->out_buf = NULL; }
    }
    ESP_LOGI(TAG, "esp_audio_simple_dec closed");
    return ESP_OK;
}

static esp_err_t _mp3_esp_codec_destroy(audio_element_handle_t self)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)audio_element_getdata(self);
    if (dec) {
        free(dec);
    }
    return ESP_OK;
}

audio_element_handle_t mp3_decoder_esp_codec_init(mp3_decoder_esp_codec_cfg_t *config)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)calloc(1, sizeof(mp3_esp_codec_t));

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open    = _mp3_esp_codec_open;
    cfg.process = _mp3_esp_codec_process;
    cfg.close   = _mp3_esp_codec_close;
    cfg.destroy = _mp3_esp_codec_destroy;
    cfg.task_stack   = (config && config->task_stack > 0) ? config->task_stack : DEFAULT_ELEMENT_STACK_SIZE;
    cfg.out_rb_size  = (config && config->out_rb_size > 0) ? config->out_rb_size : DEFAULT_ELEMENT_RINGBUF_SIZE;
    cfg.stack_in_ext = config ? config->stack_in_ext : false;
    cfg.tag = "esp_codec_mp3";

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        free(dec);
        return NULL;
    }
    audio_element_setdata(el, dec);
    ESP_LOGI(TAG, "esp_codec_mp3 element created (CODEC-17 v2.6.2 single-path, stack=%u, out_rb=%u, ext=%d)",
             (unsigned)cfg.task_stack, (unsigned)cfg.out_rb_size, cfg.stack_in_ext);
    return el;
}
