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

/* 自行解析 MPEG 音频帧头，取真实采样率/声道。
   原因：Helix 的 MP3GetLastFrameInfo 对部分文件（如 躲避的爱.mp3, 24000Hz）误报为更高采样率，
   导致 i2s 时钟跑快变尖；自行解析帧头可得与 ffmpeg 一致的真实值。 */
static int mp3_hdr_samplerate(const unsigned char *p)
{
    if (!p || (p[0] & 0xFF) != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return 0;
    }
    int ver = (p[1] >> 3) & 0x03;
    int sri = (p[2] >> 2) & 0x03;
    if (sri == 3) {
        return 0;
    }
    if (ver == 3) {        /* MPEG1 */
        int t[] = {44100, 48000, 32000, 0};
        return t[sri];
    } else if (ver == 2) { /* MPEG2 */
        int t[] = {22050, 24000, 16000, 0};
        return t[sri];
    }
    int t[] = {11025, 12000, 8000, 0}; /* MPEG2.5 (ver==0) */
    return t[sri];
}

static int mp3_hdr_channels(const unsigned char *p)
{
    if (!p || (p[0] & 0xFF) != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return -1;
    }
    int ch = (p[3] >> 6) & 0x03;
    return (ch == 3) ? 1 : 2; /* 3=mono, 其余=stereo */
}

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
        /* 解码前捕获当前帧头（缓冲区起点即 Helix 即将解的那一帧），自行解析真实采样率/声道 */
        unsigned char hdr[4];
        int have_hdr = 0;
        if (s_in_len >= 4 && (s_in[0] & 0xFF) == 0xFF && (s_in[1] & 0xE0) == 0xE0) {
            memcpy(hdr, s_in, 4);
            have_hdr = 1;
        }
        int err = MP3Decode(c->decoder, &inptr, &bytesLeft, s_pcm, 0);
        size_t consumed = s_in_len - (size_t)bytesLeft;

        if (err == ERR_MP3_NONE) {
            c->err_cnt = 0;   // R085: 成功解出一帧即清零坏帧计数，避免 err_cnt 跨整曲单调累积到 >50 导致误判曲终(AEL_IO_DONE→跳下一首)
            if (consumed > 0) {
                memmove(s_in, s_in + consumed, s_in_len - consumed);
                s_in_len -= consumed;
            }
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(c->decoder, &fi);
            /* 用自行解析的真实采样率/声道上报 i2s（不信任 Helix 误报的高采样率）；
               逐帧比对，变化才重配时钟 */
            int true_rate = have_hdr ? mp3_hdr_samplerate(hdr) : fi.samprate;
            int true_ch   = have_hdr ? mp3_hdr_channels(hdr)  : fi.nChans;
            if (true_rate <= 0) true_rate = fi.samprate;
            if (true_ch   < 0)  true_ch   = fi.nChans;
            if (true_rate != c->last_rate || true_ch != c->last_ch ||
                fi.bitsPerSample != c->last_bits) {
                audio_element_set_music_info(self, true_rate, true_ch, fi.bitsPerSample);
                c->last_rate  = true_rate;
                c->last_ch    = true_ch;
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
