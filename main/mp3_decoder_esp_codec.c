/*
 * mp3_decoder_esp_codec.c
 *
 * R076-CODEC-7: 基于 esp_audio_codec 开源 MP3 解码器 (esp_mp3_dec_*) 的
 * audio_element wrapper，替代 ADF 闭源 PV-MP3 decoder。
 *
 * 数据流：
 *   audio_element_input() 读 MP3 字节 (来自上游 fatfs_stream)
 *     -> esp_mp3_dec_decode() 解码为 PCM
 *     -> audio_element_output() 写 PCM (到下游 i2s_stream)
 *   首帧解码成功后，通过 audio_element_setinfo + report_info 通知下游采样率/声道/位深。
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "audio_element.h"
#include "esp_mp3_dec.h"
#include "esp_audio_dec.h"
#include "audio_type_def.h"   // ESP_CODEC_TYPE_MP3 枚举
#include "mp3_decoder_esp_codec.h"   // 本组件头文件 (配置结构体定义)

static const char *TAG = "MP3_ESP_CODEC";

/* 单次从 input ringbuffer 读取的 MP3 数据量 */
#define MP3_IN_READ_SIZE    (2048)
/* 解码输出 PCM 缓冲区大小 (最大一帧 PCM) */
#define MP3_OUT_FRAME_SIZE  (8192)

typedef struct {
    void *dec_handle;                 /*!< esp_mp3_dec_* 解码器句柄 */
    uint8_t *in_buf;                  /*!< MP3 输入缓冲区 */
    uint8_t *out_buf;                 /*!< PCM 输出缓冲区 */
    bool info_reported;               /*!< 是否已上报 music info 给下游 */
} mp3_esp_codec_t;

static esp_err_t _mp3_esp_codec_open(audio_element_handle_t self)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)audio_element_getdata(self);
    if (!dec) return ESP_FAIL;

    esp_audio_err_t ret = esp_mp3_dec_open(NULL, 0, &dec->dec_handle);
    if (ret != ESP_AUDIO_ERR_OK || !dec->dec_handle) {
        ESP_LOGE(TAG, "esp_mp3_dec_open failed: %d", ret);
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
        esp_mp3_dec_close(dec->dec_handle);
        dec->dec_handle = NULL;
        return ESP_FAIL;
    }

    dec->info_reported = false;
    ESP_LOGI(TAG, "esp_mp3_dec opened (handle %p)", dec->dec_handle);
    return ESP_OK;
}

static audio_element_err_t _mp3_esp_codec_process(audio_element_handle_t self,
                                                  char *el_buffer, int el_buf_len)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)audio_element_getdata(self);
    if (!dec || !dec->dec_handle) {
        return AEL_PROCESS_FAIL;
    }

    /* 1. 从上游 ringbuffer 读取 MP3 数据 */
    int rlen = audio_element_input(self, (char *)dec->in_buf, MP3_IN_READ_SIZE);
    if (rlen <= 0) {
        /* AEL_IO_DONE/AEL_IO_OK 等由框架处理；负数视为需要停止 */
        if (rlen == AEL_IO_DONE) {
            return AEL_IO_DONE;
        }
        return (audio_element_err_t)rlen;
    }

    /* 2. 循环解码本块 MP3 数据 (可能含多帧) */
    esp_audio_dec_in_raw_t raw = {
        .buffer  = dec->in_buf,
        .len     = (uint32_t)rlen,
        .consumed = 0,
    };
    esp_audio_dec_out_frame_t frame = {
        .buffer = dec->out_buf,
        .len    = MP3_OUT_FRAME_SIZE,
    };
    esp_audio_dec_info_t info = {0};

    while (raw.len > 0) {
        frame.decoded_size = 0;
        frame.needed_size  = 0;
        esp_audio_err_t ret = esp_mp3_dec_decode(dec->dec_handle, &raw, &frame, &info);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            /* 输出 buffer 不足：按解码器报告的 needed_size 扩容后重试 */
            ESP_LOGW(TAG, "decode buff not enough, needed %u", frame.needed_size);
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
            /* 解码错误 (坏帧等)：跳过本块剩余，继续后续块 */
            ESP_LOGW(TAG, "esp_mp3_dec_decode err %d, skip %u bytes", ret, raw.len);
            break;
        }
        /* ret == OK：无论本帧是否攒够 PCM，均按 raw.consumed 推进输入 */
        if (frame.decoded_size > 0) {
            int wlen = audio_element_output(self, (char *)frame.buffer, (int)frame.decoded_size);
            if (wlen <= 0) {
                return (audio_element_err_t)wlen;
            }
            /* 首帧成功：上报采样率/声道/位深给下游 i2s */
            if (!dec->info_reported && info.sample_rate > 0) {
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
        /* OK 但 consumed==0 才是真正的死循环风险，需跳出 */
        if (raw.consumed == 0) {
            ESP_LOGW(TAG, "esp_mp3_dec_decode consumed 0, break to avoid loop");
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
            esp_mp3_dec_close(dec->dec_handle);
            dec->dec_handle = NULL;
        }
        if (dec->in_buf)  { free(dec->in_buf);  dec->in_buf = NULL; }
        if (dec->out_buf) { free(dec->out_buf); dec->out_buf = NULL; }
    }
    ESP_LOGI(TAG, "esp_mp3_dec closed");
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
    ESP_LOGI(TAG, "esp_codec_mp3 element created (stack=%d, out_rb=%d, ext=%d)",
             cfg.task_stack, cfg.out_rb_size, cfg.stack_in_ext);
    return el;
}
