/*
 * MP3 decoder wrapper based on Helix MP3 decoder (chmorgan/esp-libhelix-mp3, Apache-2.0)
 *
 * 取代 ADF 闭源 PV-MP3：
 *   - PV-MP3 对部分合法 MP3 有确定性崩溃（PC=0x403743c0），且不在 main 仓库可改；
 *   - Helix 以"返回错误码"方式处理坏帧，绝不解引用坏数据，天然避免崩溃；
 *   - 坏帧/连续错误时自动跳到下一段（不返回崩溃），由 audio_player_tick() 在元素
 *     FINISHED 时触发 on_track_finished -> 自动播下一首，实现"跳曲保护"。
 *
 * R080: 新增本文件替换 pvmp3。
 */
#include <string.h>
#include "audio_element.h"
#include "audio_mem.h"
#include "mp3dec.h"
#include "mp3_decoder_libhelix.h"

/* 单次喂给 Helix 的输入缓冲（足够容纳一个最大 MP3 帧 ~1441B + 余量） */
#define HELIX_IN_BUF   2048
/* 单帧 PCM 输出上限：2ch * 1152 样本 * 2B = 4608B */
#define HELIX_PCM_SAMPLES (1152 * 2)

static short      s_pcm[HELIX_PCM_SAMPLES];
static unsigned char s_in[HELIX_IN_BUF];
static size_t     s_in_len = 0;   /* s_in 中尚未喂给 Helix 的字节数 */

typedef struct {
    HMP3Decoder decoder;
    int         last_rate;  /* 上次上报的采样率，用于变化时重配 i2s 时钟 */
    int         last_ch;    /* 上次上报的声道数 */
    int         last_bits;  /* 上次上报的位宽 */
    int         err_cnt;    /* 连续不可恢复错误计数（坏帧/溢出） */
} helix_ctx_t;

static esp_err_t _mp3_helix_open(audio_element_handle_t self)
{
    helix_ctx_t *c = audio_calloc(1, sizeof(helix_ctx_t));
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    c->decoder = MP3InitDecoder();
    if (!c->decoder) {
        audio_free(c);
        return ESP_ERR_NO_MEM;
    }
    audio_element_setdata(self, c);
    return ESP_OK;
}

static esp_err_t _mp3_helix_close(audio_element_handle_t self)
{
    helix_ctx_t *c = (helix_ctx_t *)audio_element_getdata(self);
    if (c) {
        if (c->decoder) {
            MP3FreeDecoder(c->decoder);
        }
        audio_free(c);
        audio_element_setdata(self, NULL);
    }
    s_in_len = 0;
    return ESP_OK;
}

static audio_element_err_t _mp3_helix_process(audio_element_handle_t self, char *el_buffer, int el_buf_len)
{
    (void)el_buffer;
    (void)el_buf_len;
    helix_ctx_t *c = (helix_ctx_t *)audio_element_getdata(self);

    /* 从上游 ringbuf 取输入（自动处理 EOS：返回 AEL_IO_DONE） */
    audio_element_err_t r = audio_element_input(self, (char *)(s_in + s_in_len),
                                                (int)(HELIX_IN_BUF - s_in_len));
    bool eos = false;
    if (r == AEL_IO_DONE) {
        eos = true;
    } else if (r < 0) {
        return r;
    } else {
        s_in_len += (size_t)r;
    }

    if (eos && s_in_len == 0) {
        return AEL_IO_DONE;
    }

    audio_element_err_t out_total = 0;
    while (s_in_len > 0) {
        unsigned char *inptr = s_in;
        int bytesLeft = (int)s_in_len;
        int err = MP3Decode(c->decoder, &inptr, &bytesLeft, s_pcm, 0);
        size_t consumed = s_in_len - (size_t)bytesLeft;

        if (err == ERR_MP3_NONE) {
            if (consumed > 0) {
                memmove(s_in, s_in + consumed, s_in_len - consumed);
                s_in_len -= consumed;
            }
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(c->decoder, &fi);
            /* 每帧比对：首帧上报；若 Helix 对后续帧上报的采样率/声道/位宽变化，
               立即重配 i2s 时钟（避免首帧误报高采样率导致整首变快） */
            if (fi.samprate != c->last_rate || fi.nChans != c->last_ch ||
                fi.bitsPerSample != c->last_bits) {
                audio_element_set_music_info(self, fi.samprate, fi.nChans, fi.bitsPerSample);
                c->last_rate  = fi.samprate;
                c->last_ch    = fi.nChans;
                c->last_bits  = fi.bitsPerSample;
            }
            size_t outlen = (size_t)fi.outputSamps * sizeof(short);
            audio_element_err_t w = audio_element_output(self, (char *)s_pcm, (int)outlen);
            if (w < 0) {
                return w;
            }
            out_total += w;
        } else if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            /* 数据不足：等下次补更多输入。若缓冲已塞满仍解不出一整帧 -> 损坏，丢弃重来 */
            if (s_in_len >= HELIX_IN_BUF) {
                c->err_cnt++;
                if (c->err_cnt > 50) {
                    return AEL_IO_DONE;
                }
                s_in_len = 0;
            }
            if (eos) {
                /* 上游已结束且残片不足一帧，直接结束（跳曲保护） */
                return AEL_IO_DONE;
            }
            break;
        } else {
            /* 坏帧：定位下一个同步字跳过，连续坏帧过多则结束（跳曲） */
            c->err_cnt++;
            if (c->err_cnt > 50) {
                return AEL_IO_DONE;
            }
            int off = MP3FindSyncWord(s_in, (int)s_in_len);
            if (off < 0) {
                s_in_len = 0;
            } else {
                memmove(s_in, s_in + off, s_in_len - (size_t)off);
                s_in_len -= (size_t)off;
            }
            if (s_in_len == 0) {
                if (eos) {
                    return AEL_IO_DONE;
                }
                break;
            }
        }
    }

    return (out_total > 0) ? out_total : AEL_IO_OK;
}

audio_element_handle_t mp3_decoder_libhelix_init(const mp3_decoder_libhelix_cfg_t *config)
{
    mp3_decoder_libhelix_cfg_t cfg_buf;
    if (config == NULL) {
        cfg_buf = (mp3_decoder_libhelix_cfg_t)DEFAULT_MP3_DECODER_LIBHELIX_CONFIG();
        config = &cfg_buf;
    }
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open  = _mp3_helix_open;
    cfg.close = _mp3_helix_close;
    cfg.process = _mp3_helix_process;
    cfg.task_stack   = config->task_stack;
    cfg.out_rb_size  = config->out_rb_size;
    cfg.stack_in_ext = config->stack_in_ext;
    cfg.tag = "mp3_dec";

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        return NULL;
    }
    audio_element_setdata(el, NULL);
    return el;
}
