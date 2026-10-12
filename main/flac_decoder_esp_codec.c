/*
 * flac_decoder_esp_codec.c
 *
 * R122-FLAC-CODEC: 替代 ADF 闭源 FLAC 解码器 (flac_decoder_init, libesp_processing.a)。
 * 闭源解码器在 16/24bit FLAC 上都有内部 PCM 缓冲越界, 污染 PSRAM/LVGL -> Guru Meditation。
 * 改用 esp_audio_codec 的 simple_dec(FLAC) 路径 (与已验证 MP3 解码器同源)。
 *
 * 数据流:
 *   file ringbuffer -> esp_audio_simple_dec_process(FLAC)
 *     -> flac parser (切完整 FLAC frame) -> esp_flac_dec_decode 解码 PCM
 *     -> decoder ringbuffer -> i2s_stream -> MAX98357A
 *
 * 24bit FLAC 由解码器输出 24bit PCM 并经 music_info(bits=24) 上报, I2S 自动切 24bit
 * (与 R122 WAV 24bit 直通共用同一 I2S 24bit 路径)。
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "audio_element.h"
#include "esp_audio_simple_dec.h"   // simple_dec API
#include "esp_flac_dec.h"            // esp_flac_dec_register (注册 FLAC 解码器到全局表)
#include "audio_type_def.h"
#include "esp_heap_caps.h"   /* R135-FLAC: out_buf 放 PSRAM 避免片内 RAM 不足/越界踩 LVGL */
#include "flac_decoder_esp_codec.h"

static const char *TAG = "FLAC_ESP_CODEC";

/* 单次从 input ringbuffer 读取的 FLAC 数据量 */
#define FLAC_IN_READ_SIZE   (4096)
/* 解码输出 PCM 缓冲区大小: 24bit stereo 48k 最大帧(8192 samples*2ch*3B=49KB) 留余量 */
#define FLAC_OUT_FRAME_SIZE (256 * 1024)  /* R135: FLAC 最大帧 256KB, 原 64KB 越界踩 LVGL 堆 */

typedef struct {
    esp_audio_simple_dec_handle_t dec_handle;
    uint8_t *in_buf;
    uint8_t *out_buf;
    bool info_reported;
    int  err_count;   // 连续解码错误计数, 超阈值放弃本曲
} flac_esp_codec_t;

static esp_err_t _flac_esp_codec_open(audio_element_handle_t self)
{
    flac_esp_codec_t *dec = (flac_esp_codec_t *)audio_element_getdata(self);
    if (!dec) return ESP_FAIL;

    /* 注册 FLAC decoder 到全局表, 否则 simple_dec 内部 esp_audio_dec_open 查不到 FLAC 类型 */
    static bool s_registered = false;
    if (!s_registered) {
        esp_audio_err_t reg_ret = esp_flac_dec_register();
        if (reg_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "esp_flac_dec_register failed: %d", reg_ret);
            return ESP_FAIL;
        }
        s_registered = true;
        ESP_LOGI(TAG, "registered esp_flac_dec (once)");
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type      = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
        .dec_cfg       = NULL,
        .cfg_size      = 0,
        .use_frame_dec = false,  // 由 simple_dec 内部 parser 切帧
    };
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &dec->dec_handle);
    if (ret != ESP_AUDIO_ERR_OK || !dec->dec_handle) {
        ESP_LOGE(TAG, "esp_audio_simple_dec_open(FLAC) failed: %d", ret);
        return ESP_FAIL;
    }

    dec->in_buf  = (uint8_t *)malloc(FLAC_IN_READ_SIZE);
    dec->out_buf = (uint8_t *)heap_caps_malloc(FLAC_OUT_FRAME_SIZE, MALLOC_CAP_SPIRAM);  /* R135: 256KB 放 PSRAM */
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
    dec->err_count = 0;
    ESP_LOGI(TAG, "esp_audio_simple_dec(FLAC, parser=auto) opened (handle %p)", dec->dec_handle);
    return ESP_OK;
}

static audio_element_err_t _flac_esp_codec_process(audio_element_handle_t self, char *el_buffer, int el_buf_len)
{
    flac_esp_codec_t *dec = (flac_esp_codec_t *)audio_element_getdata(self);
    if (!dec || !dec->dec_handle) {
        return AEL_PROCESS_FAIL;
    }

    int rlen = audio_element_input(self, (char *)dec->in_buf, FLAC_IN_READ_SIZE);
    bool eos = false;
    if (rlen <= 0) {
        if (rlen == AEL_IO_DONE) {
            eos = true;   // FLAC parser 需要 eos 才 flush 缓存的末帧
        } else {
            return (audio_element_err_t)rlen;
        }
    }

    esp_audio_simple_dec_raw_t raw = {
        .buffer   = dec->in_buf,
        .len      = (uint32_t)(rlen > 0 ? rlen : 0),
        .eos      = eos,
        .consumed = 0,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = dec->out_buf,
        .len    = FLAC_OUT_FRAME_SIZE,
    };

    /* 1) 常规解码 (有输入数据时) */
    while (raw.len > 0) {
        frame.decoded_size = 0;
        frame.needed_size  = 0;
        esp_audio_err_t ret = esp_audio_simple_dec_process(dec->dec_handle, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (frame.needed_size > FLAC_OUT_FRAME_SIZE) {
                uint8_t *nb = (uint8_t *)heap_caps_realloc(dec->out_buf, frame.needed_size, MALLOC_CAP_SPIRAM);
                if (nb) {
                    dec->out_buf = nb;
                    frame.buffer = nb;
                    frame.len    = frame.needed_size;
                    continue;
                }
            }
            break;
        }
        if (ret == ESP_AUDIO_ERR_DATA_LACK) {
            break;  // 流式输入不够凑一帧, 续喂新块即可
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "esp_audio_simple_dec_process(FLAC) err %d, skip %u bytes", ret, raw.len);
            if (raw.consumed > 0) {
                raw.buffer += raw.consumed;
                raw.len    -= raw.consumed;
                continue;
            }
            if (++dec->err_count > 50) {
                ESP_LOGE(TAG, "too many decode errors, abort this track");
                return AEL_IO_DONE;
            }
            break;
        }
        if (frame.decoded_size > 0) {
            dec->err_count = 0;   // 成功解出帧, 重置连续错误计数
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
                    music_info.bits         = (info.bits_per_sample > 16) ? 16 : info.bits_per_sample;  // R122-FLAC-16BIT: 强制 16bit 上报, 让 I2S 恒 16bit(规避 24bit 越界)
                    music_info.codec_fmt    = ESP_CODEC_TYPE_FLAC;
                    audio_element_setinfo(self, &music_info);
                    audio_element_report_info(self);
                    dec->info_reported = true;
                    ESP_LOGI(TAG, "FLAC info: %d Hz, %d ch, %d bit",
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

    /* 2) 流尾 flush (eos 时 parser 可能还缓存着末帧) */
    if (eos) {
        int guard = 0;
        do {
            frame.decoded_size = 0;
            frame.needed_size  = 0;
            raw.len = 0;   // flush 不再喂新数据
            raw.eos = true;
            esp_audio_err_t ret = esp_audio_simple_dec_process(dec->dec_handle, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (frame.needed_size > FLAC_OUT_FRAME_SIZE) {
                    uint8_t *nb = (uint8_t *)heap_caps_realloc(dec->out_buf, frame.needed_size, MALLOC_CAP_SPIRAM);
                    if (nb) {
                        dec->out_buf = nb;
                        frame.buffer = nb;
                        frame.len    = frame.needed_size;
                        continue;
                    }
                }
                break;
            }
            if (ret != ESP_AUDIO_ERR_OK) break;
            if (frame.decoded_size > 0) {
                int wlen = audio_element_output(self, (char *)frame.buffer, (int)frame.decoded_size);
                if (wlen <= 0) return (audio_element_err_t)wlen;
            }
            guard++;
        } while (frame.decoded_size > 0 && guard < 1000);
        return AEL_IO_DONE;
    }

    return AEL_IO_TIMEOUT;   // R125: 必须返回 TIMEOUT 让 ADF 继续; 返回 AEL_IO_OK(0) 会被 ADF 当作"完成", 导致解码完一帧即秒结束
}

static esp_err_t _flac_esp_codec_close(audio_element_handle_t self)
{
    flac_esp_codec_t *dec = (flac_esp_codec_t *)audio_element_getdata(self);
    if (dec) {
        if (dec->dec_handle) {
            esp_audio_simple_dec_close(dec->dec_handle);
            dec->dec_handle = NULL;
        }
        if (dec->in_buf)  { free(dec->in_buf);  dec->in_buf = NULL; }
        if (dec->out_buf) { free(dec->out_buf); dec->out_buf = NULL; }
    }
    ESP_LOGI(TAG, "esp_audio_simple_dec(FLAC) closed");
    return ESP_OK;
}

static esp_err_t _flac_esp_codec_destroy(audio_element_handle_t self)
{
    flac_esp_codec_t *dec = (flac_esp_codec_t *)audio_element_getdata(self);
    if (dec) {
        free(dec);
    }
    return ESP_OK;
}

audio_element_handle_t flac_decoder_esp_codec_init(flac_decoder_esp_codec_cfg_t *config)
{
    flac_decoder_esp_codec_cfg_t cfg_buf;
    if (config == NULL) {
        cfg_buf = (flac_decoder_esp_codec_cfg_t)DEFAULT_FLAC_DECODER_ESP_CODEC_CONFIG();
        config = &cfg_buf;
    }
    flac_esp_codec_t *dec = (flac_esp_codec_t *)calloc(1, sizeof(flac_esp_codec_t));

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open    = _flac_esp_codec_open;
    cfg.process = _flac_esp_codec_process;
    cfg.close   = _flac_esp_codec_close;
    cfg.destroy = _flac_esp_codec_destroy;
    cfg.task_stack   = config->task_stack > 0 ? config->task_stack : DEFAULT_ELEMENT_STACK_SIZE;
    cfg.out_rb_size  = config->out_rb_size > 0 ? config->out_rb_size : DEFAULT_ELEMENT_RINGBUF_SIZE;
    cfg.stack_in_ext = config->stack_in_ext;
    cfg.tag = "esp_codec_flac";

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        free(dec);
        return NULL;
    }
    audio_element_setdata(el, dec);
    ESP_LOGI(TAG, "esp_codec_flac element created (stack=%u, out_rb=%u, ext=%d)",
             (unsigned)cfg.task_stack, (unsigned)cfg.out_rb_size, cfg.stack_in_ext);
    return el;
}
