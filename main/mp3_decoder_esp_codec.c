/*
 * mp3_decoder_esp_codec.c
 *
 * R076-CODEC-7~9: 基于 esp_audio_codec 开源 MP3 解码器 (esp_mp3_dec_*) 的
 * audio_element wrapper，替代 ADF 闭源 PV-MP3 decoder。
 *
 * R076-CODEC-10: 改用 esp_audio_simple_dec_* API（带 mpeg_parser 切帧路径）
 *
 * 数据流：
 *   audio_element_input() 读 MP3 字节 (来自上游 fatfs_stream)
 *     -> esp_audio_simple_dec_process() 内部 mpeg_parser 切完整帧 + esp_mp3_dec_decode 解码为 PCM
 *     -> audio_element_output() 写 PCM (到下游 i2s_stream)
 *   首帧解码成功后，通过 audio_element_setinfo + report_info 通知下游采样率/声道/位深。
 *
 * 关键变更 (CODEC-10):
 *   - 直接调 esp_mp3_dec_decode 时要求 caller 提供完整 MP3 frame，我们 wrapper 之前
 *     直接喂 2KB 任意字节让 minimp3 自己找帧头，遇到 ID3v2 + VBR 异常帧组合会触发
 *     minimp3 内部栈/堆破坏 -> task 寄存器全废 -> DoubleException @ 0x403743c0
 *     (与闭源 PV-MP3 完全相同的崩溃现象)
 *   - esp_audio_simple_dec_* 是 ESP-ADF 官方推荐的"任意字节输入"路径，内部 mpeg_parser
 *     先跳过 ID3v2/坏字节切出合法 frame 再喂给 esp_mp3_dec_decode，从根本上隔离
 *     异常输入对解码器的冲击
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "audio_element.h"
#include "esp_audio_simple_dec.h"   // R076-CODEC-10: 新 API 入口
// 注：CMakeLists 已把 esp_audio_codec/include/simple_dec/ 加到 INCLUDE_DIRS
#include "esp_audio_dec_default.h"   // R076-CODEC-11: esp_audio_dec_register_default
#include "esp_audio_simple_dec_default.h"  // R076-CODEC-11: esp_audio_simple_dec_register_default
#include "audio_type_def.h"          // ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 枚举
#include "mp3_decoder_esp_codec.h"   // 本组件头文件 (配置结构体定义)

static const char *TAG = "MP3_ESP_CODEC";

/* 单次从 input ringbuffer 读取的 MP3 数据量。
 * 2KB 是 ESP-ADF 推荐值（参考 play_sdcard_mp3_control 示例）。 */
#define MP3_IN_READ_SIZE    (2048)
/* 解码输出 PCM 缓冲区大小 (最大一帧 PCM，MP3 单帧最多 1152 样本/声道 * 2ch * 2字节 = 4608，
 * 8192 留 2x 余量)。 */
#define MP3_OUT_FRAME_SIZE  (8192)

typedef struct {
    esp_audio_simple_dec_handle_t dec_handle;  /*!< esp_audio_simple_dec_* 解码器句柄
                                                 *   内部包含 mpeg_parser + esp_mp3_dec */
    uint8_t *in_buf;                  /*!< MP3 输入缓冲区 */
    uint8_t *out_buf;                 /*!< PCM 输出缓冲区 */
    bool info_reported;               /*!< 是否已上报 music info 给下游 */
} mp3_esp_codec_t;

static esp_err_t _mp3_esp_codec_open(audio_element_handle_t self)
{
    mp3_esp_codec_t *dec = (mp3_esp_codec_t *)audio_element_getdata(self);
    if (!dec) return ESP_FAIL;

    /* R076-CODEC-11: 关键修复 - 必须先注册全局 decoder 表，否则 simple_dec 内部
     * esp_audio_dec_open 会查不到 MP3 类型 → 返回 ESP_AUDIO_ERR_NOT_SUPPORT (-7)
     *
     * 参考 ESP-ADF audio_decoder_test.c:359-362 / simple_decoder_test.c:359-362：
     *   esp_audio_dec_register_default();         // 注册所有 decoder (含 esp_mp3_dec)
     *   esp_audio_simple_dec_register_default();  // 注册所有 simple_dec parser
     *   esp_audio_simple_dec_open(...);
     *
     * 两个 register API 内部用标志位保证幂等，可以每首播放都安全调用。
     * 但为了减少冗余开销，用静态标志只在首次 open 时调一次。 */
    static bool s_decoders_registered = false;
    if (!s_decoders_registered) {
        esp_audio_err_t reg_ret = esp_audio_dec_register_default();
        if (reg_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "esp_audio_dec_register_default failed: %d", reg_ret);
            return ESP_FAIL;
        }
        reg_ret = esp_audio_simple_dec_register_default();
        if (reg_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "esp_audio_simple_dec_register_default failed: %d", reg_ret);
            return ESP_FAIL;
        }
        s_decoders_registered = true;
        ESP_LOGI(TAG, "registered esp_audio_dec + esp_audio_simple_dec defaults (once)");
    }

    /* R076-CODEC-10: 走 esp_audio_simple_dec_* 路径
     *   - dec_type: MP3
     *   - dec_cfg: NULL (esp_mp3_dec 的 cfg 参数 "no need to set"，见 esp_mp3_dec.h:62)
     *   - cfg_size: 0
     *   - use_frame_dec: false → 启用 mpeg_parser 自动切帧（处理 ID3v2 / VBR / 异常字节） */
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type      = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg       = NULL,
        .cfg_size      = 0,
        .use_frame_dec = false,
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

    /* 1. 从上游 ringbuffer 读取 MP3 原始字节（任意大小，内部 parser 自己找帧） */
    int rlen = audio_element_input(self, (char *)dec->in_buf, MP3_IN_READ_SIZE);
    if (rlen <= 0) {
        if (rlen == AEL_IO_DONE) {
            return AEL_IO_DONE;
        }
        return (audio_element_err_t)rlen;
    }

    /* 2. 喂给 esp_audio_simple_dec_process
     *   - 内部分两步：mpeg_parser 切出完整 MP3 frame + esp_mp3_dec_decode 解码为 PCM
     *   - 输入 raw.len 不一定是 frame 整数倍（parser 会缓存未消费字节）
     *   - output 写入 frame.buffer，给到 caller 的 size 是 frame.decoded_size
     *   - DATA_LACK / BUFF_NOT_ENOUGH 等返回码由 simple_dec 内部处理，不再外露给 caller */
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

    /* loop 退出条件：parser/consume 推进到 raw.len==0（即本块全被消费）
     * 或返回非 OK 但 DATA_LACK 视为合法（继续喂新块）
     * 或 BUFF_NOT_ENOUGH 扩容后重试 */
    while (raw.len > 0) {
        frame.decoded_size = 0;
        frame.needed_size  = 0;
        esp_audio_err_t ret = esp_audio_simple_dec_process(dec->dec_handle, &raw, &frame);
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
            /* 其它错误（FAIL / NOT_SUPPORT 等）：坏帧或不支持格式，跳过本块剩余 */
            ESP_LOGW(TAG, "esp_audio_simple_dec_process err %d, consumed=%u/%u",
                     ret, raw.consumed, (uint32_t)rlen);
            /* 如果 parser 没消费任何字节，避免无限循环：直接按 input rlen 全推进 */
            if (raw.consumed == 0) {
                ESP_LOGW(TAG, "consumed==0, force advance to avoid loop");
                raw.buffer += rlen;
                raw.len    = 0;
            } else {
                raw.buffer += raw.consumed;
                raw.len    -= raw.consumed;
            }
            /* 若还剩，跳出等下次 process 拿新块；这里采用"余下本次丢弃"策略
             * 防止被同一个坏字节粘住。MP3 parser 本身有同步恢复，下次喂入会重新对齐。 */
            break;
        }

        /* ret == OK：处理解码输出 */
        if (frame.decoded_size > 0) {
            int wlen = audio_element_output(self, (char *)frame.buffer, (int)frame.decoded_size);
            if (wlen <= 0) {
                return (audio_element_err_t)wlen;
            }
            /* 首帧成功：上报采样率/声道/位深给下游 i2s */
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

        /* 推进 raw 输入（parser 会缓存未消费字节到内部） */
        if (raw.consumed == 0) {
            /* 本次未消费任何字节 + 无解码输出 = parser 还在攒帧头，不算错。
             * 直接 break 等下次 process 拿新块拼起来。 */
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
    ESP_LOGI(TAG, "esp_codec_mp3 element created (CODEC-10 simple_dec path, stack=%d, out_rb=%d, ext=%d)",
             cfg.task_stack, cfg.out_rb_size, cfg.stack_in_ext);
    return el;
}
