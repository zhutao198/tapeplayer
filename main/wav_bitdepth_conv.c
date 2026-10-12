/* R123-WAV / R125: WAV 原始 PCM(16/24/32bit) -> 16bit PCM 转换 element。
   直通的 WAV 数据由 fatfs_reader 直接喂入；本 element 按真实位深把每个采样
   截断/移位为 16bit（24bit 取高 16bit、32bit 取高 16bit），输出到下游 i2s_writer。
   跨块未对齐的尾巴字节由 carry 缓冲暂存，下一拍拼接，保证 24/32bit 采样永不错位。

   R125 修复:
   - process 必须返回 AEL_IO_TIMEOUT 让 ADF 继续(返回 AEL_IO_OK(0) 会被 ADF 当作
     "完成" set_ringbuf_done+FINISHED, 导致解完首块即秒结束)。
   - 输入与输出用独立缓冲(in_buf/out_buf), 杜绝 24/32bit 原地转换时"写覆盖未读输入"
     造成的采样损坏(L/R 错位/爆音)。 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_element.h"
#include "audio_common.h"
#include "wav_bitdepth_conv.h"

static const char *TAG = "WAV_CONV";

typedef struct {
    int  src_bits;
    int  is_float;      /* 1 = 32-bit IEEE float 源 */
    int  sample_rate;   /* WAV 真实采样率 */
    int  sample_size;   /* 2/3/4 */
    uint8_t carry[4];
    int  carry_len;
    uint8_t *in_buf;    /* 上游 PCM 暂存(与输出分离) */
    uint8_t *out_buf;   /* 16bit 输出缓冲 */
    int  buf_cap;
    int  diag_done;     /* 每文件首拍打印原始字节+两种位深解释 */
    int  diag_out_done; /* 每文件首拍打印实际输出样本 */
} wav_conv_t;

static esp_err_t _wav_conv_open(audio_element_handle_t self)
{
    wav_conv_t *c = audio_element_getdata(self);
    if (c) { c->carry_len = 0; c->diag_done = 0; c->diag_out_done = 0; }
    return ESP_OK;
}

static audio_element_err_t _wav_conv_process(audio_element_handle_t self, char *el_buffer, int el_buf_len)
{
    (void)el_buffer;
    wav_conv_t *c = audio_element_getdata(self);
    if (!c || c->src_bits < 16) return AEL_PROCESS_FAIL;

    int r = audio_element_input(self, (char *)c->in_buf, c->buf_cap - 4);
    if (r == AEL_IO_DONE) {
        /* 流尾: 残余不足一采样, 丢弃(carry 不构成整采样) */
        c->carry_len = 0;
        return AEL_IO_DONE;
    }
    if (r < 0) return (audio_element_err_t)r;
    if (r == 0) return AEL_IO_TIMEOUT;   /* 暂无数据, 继续等待 */

    /* R131c: 实测 SD->conv 输入吞吐, 与需求对比, 定位 48000/96000 杂音是否 SD 欠载。
       need = rate * ch * sample_size (字节/秒)。若 actual 长期 < need 则 SD 带宽瓶颈。
       R131f: need 用 audio_player 解析出的真实码率 c->sample_rate(ch 恒 2), 不再依赖
       getinfo(self)(conv 首拍会把自身 info 覆盖成默认 44100, 致 need 永远错算 352800)。 */
    static uint64_t s_in = 0;
    static int64_t  s_it0 = 0;
    if (r > 0) {
        s_in += (uint64_t)r;
        int64_t now = esp_timer_get_time();
        if (s_it0 == 0) s_it0 = now;
        int64_t dt = now - s_it0;
        if (dt >= 2000000) {
            int rate = (int)((double)s_in * 1000000.0 / (double)dt);
            int ch = 2;
            int sr = c->sample_rate;
            int need = sr * ch * c->sample_size;
            ESP_LOGW(TAG, "INRATE: actual=%d need=%d (rate=%d ch=%d src_bits=%d) -> %s",
                     rate, need, sr, ch, c->src_bits, (rate >= need) ? "OK" : "UNDERRUN");
            s_in = 0; s_it0 = now;
        }
    }

    /* R131-DIAG: 每文件首拍打印原始样本字节, 并同时按 32bit(s>>16) 与 24bit(s>>8) 两种解释
       打印前若干样本, 以判别真实位深(真实音频两种解释中仅一种会平滑, 另一种剧烈跳变/爆音)。 */
    if (!c->diag_done) {
        c->diag_done = 1;
        int dump = (r > 48) ? 48 : r;
        ESP_LOGW(TAG, "R131-DIAG src_bits=%d sample_size=%d first %d bytes:", c->src_bits, c->sample_size, dump);
        ESP_LOG_BUFFER_HEX(TAG, c->in_buf, dump);
        ESP_LOGW(TAG, "R131-DIAG try 32bit(s>>16):");
        for (int i = 0; i + 4 <= dump && i / 4 < 8; i += 4) {
            int32_t s; memcpy(&s, c->in_buf + i, 4);
            ESP_LOGW(TAG, "  [%d]=%d", i / 4, (int)(int16_t)(s >> 16));
        }
        ESP_LOGW(TAG, "R131-DIAG try 24bit(s>>8):");
        for (int i = 0; i + 3 <= dump && i / 3 < 8; i += 3) {
            int32_t s = c->in_buf[i] | (c->in_buf[i + 1] << 8) | (c->in_buf[i + 2] << 16);
            if (s & 0x800000) s |= 0xFF000000;
            ESP_LOGW(TAG, "  [%d]=%d", i / 3, (int)(int16_t)(s >> 8));
        }
    }

    uint8_t *work = c->in_buf;
    int total = r;
    if (c->carry_len > 0) {
        memmove(work + c->carry_len, work, r);
        memcpy(work, c->carry, c->carry_len);
        total += c->carry_len;
        c->carry_len = 0;
    }

    int n = total / c->sample_size;
    int consumed = n * c->sample_size;
    int leftover = total - consumed;
    if (leftover > 0) {
        memcpy(c->carry, work + consumed, leftover);
        c->carry_len = leftover;
    }

    int16_t *out = (int16_t *)c->out_buf;
    int out_samples = 0;
    if (c->src_bits == 16) {
        for (int i = 0; i < n; i++) {
            out[i] = (int16_t)((work[i * 2]) | (work[i * 2 + 1] << 8));
        }
        out_samples = n;
    } else if (c->src_bits == 24) {
        for (int i = 0; i < n; i++) {
            int32_t s = work[i * 3] | (work[i * 3 + 1] << 8) | (work[i * 3 + 2] << 16);
            if (s & 0x800000) s |= 0xFF000000;   /* 24bit 符号扩展 */
            out[i] = (int16_t)(s >> 8);           /* 取高 16bit */
        }
        out_samples = n;
    } else { /* 32bit */
        if (c->is_float) {
            /* IEEE float: 值域 [-1,1]，乘 32767 并钳位后转 16bit 整数。
               直接按整数移位(s>>16)会把 float 位模式当整数，产生满屏杂音。 */
            for (int i = 0; i < n; i++) {
                float fv;
                memcpy(&fv, work + i * 4, 4);
                if (fv > 1.0f) fv = 1.0f;
                else if (fv < -1.0f) fv = -1.0f;
                out[i] = (int16_t)(fv * 32767.0f);
            }
        } else {
            for (int i = 0; i < n; i++) {
                int32_t s;
                memcpy(&s, work + i * 4, 4);
                out[i] = (int16_t)(s >> 16);          /* 取高 16bit */
            }
        }
        out_samples = n;
        }

        /* R131-DIAG: 打印按当前位深转换后的前 12 个 16bit 输出样本(十进制),
           确认是否为真实音频(应平滑、幅度适中、不过载); 若剧烈跳变/满幅爆音则位深错位。 */
        if (!c->diag_out_done && out_samples > 0) {
            c->diag_out_done = 1;
            int m = (out_samples > 12) ? 12 : out_samples;
            ESP_LOGW(TAG, "R131-OUT src_bits=%d sample_size=%d first %d out samples:",
                     c->src_bits, c->sample_size, m);
            for (int i = 0; i < m; i++) {
                ESP_LOGW(TAG, "  [%d]=%d (0x%04x)", i, (int)out[i], (unsigned)(uint16_t)out[i]);
            }
        }

        if (out_samples <= 0) return AEL_IO_TIMEOUT;
    /* R131g: 强制下游 music_info bits=16。否则 i2s_stream 会用上游(file)转发的 32bit
       music_info 在首块数据到达时自动重配 I2S 为 32bit 帧, 导致本 element 的 16bit
       输出被错位接收——表现为杂音 + 速率减半(变慢)。rate/ch 沿用文件头值(随 input 更新)。 */
    audio_element_info_t mi;
    audio_element_getinfo(self, &mi);
    if (mi.sample_rates > 0) {
        audio_element_set_music_info(self, mi.sample_rates, mi.channels, 16);
    }
    int w = audio_element_output(self, (char *)out, out_samples * 2);
    if (w <= 0) return (audio_element_err_t)w;
    return AEL_IO_TIMEOUT;   // R125: 继续, 不能返回 AEL_IO_OK(会被 ADF 误判曲终)
}

static esp_err_t _wav_conv_close(audio_element_handle_t self)
{
    (void)self;
    return ESP_OK;
}

static esp_err_t _wav_conv_destroy(audio_element_handle_t self)
{
    wav_conv_t *c = audio_element_getdata(self);
    if (c) {
        if (c->in_buf)  { free(c->in_buf);  c->in_buf = NULL; }
        if (c->out_buf) { free(c->out_buf); c->out_buf = NULL; }
        free(c);
    }
    return ESP_OK;
}

audio_element_handle_t wav_bitdepth_conv_init(wav_conv_cfg_t *cfg)
{
    wav_conv_t *c = calloc(1, sizeof(wav_conv_t));
    if (!c) return NULL;
    c->src_bits = cfg->src_bits;
    c->is_float = cfg->is_float ? 1 : 0;
    c->sample_rate = (cfg->sample_rate > 0) ? cfg->sample_rate : 48000;
    c->sample_size = (cfg->src_bits == 16) ? 2 : (cfg->src_bits == 24 ? 3 : 4);
    c->carry_len = 0;

    /* 输入/输出独立缓冲: 放大到 8192 以减少 SD 读事务数(1-bit SPI 吞吐瓶颈下,
       大块读显著提升有效吞吐、吸收间歇停顿); 输出字节数恒 <= 输入, 同容量足够 */
    int cap = 8192;
    c->buf_cap = cap;
    c->in_buf  = (uint8_t *)malloc(cap);
    c->out_buf = (uint8_t *)malloc(cap);
    if (!c->in_buf || !c->out_buf) {
        if (c->in_buf)  free(c->in_buf);
        if (c->out_buf) free(c->out_buf);
        free(c);
        return NULL;
    }

    audio_element_cfg_t el_cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    el_cfg.open = _wav_conv_open;
    el_cfg.close = _wav_conv_close;
    el_cfg.process = _wav_conv_process;
    el_cfg.destroy = _wav_conv_destroy;
    el_cfg.task_stack = 8192;     /* R130: 原 2048 过小, 切歌 terminate 路径栈溢出死机; 提到 8192 彻底消除 */
    el_cfg.task_prio = 5;
    el_cfg.buffer_len = cap;      /* 元素工作缓冲(留 4 字节给 carry 前插余量) */
    el_cfg.out_rb_size = 16384;   /* 下游 ringbuffer(解码器->i2s 之间), 放大吸收 SD 停顿 */
    el_cfg.tag = "wav_conv";
    audio_element_handle_t h = audio_element_init(&el_cfg);
    if (!h) {
        free(c->in_buf);
        free(c->out_buf);
        free(c);
        return NULL;
    }
    audio_element_setdata(h, c);
    return h;
}
