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
#include <math.h>
#include "esp_log.h"
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

static bool g_seek_dbg_first = false;

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

/* R098: 从合法 Layer III 帧头计算帧长(字节)，供 seek/坏帧后跳过当前帧到下一帧边界。
   非法/非 Layer III/非 free-format 头返回 -1。 */
static int mp3_frame_len_from_hdr(const unsigned char *p)
{
    if (!p || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return -1;
    int version = (p[1] >> 3) & 0x03;
    int layer   = (p[1] >> 1) & 0x03;
    if (version == 0x01 || layer != 0x01) return -1;   /* reserved 或非 Layer III */
    int bitrate_idx = (p[2] >> 4) & 0x0F;
    if (bitrate_idx == 0x00 || bitrate_idx == 0x0F) return -1;  /* 0=free,15=bad */
    int sr_idx = (p[2] >> 2) & 0x03;
    if (sr_idx == 0x03) return -1;
    int padding = (p[2] >> 1) & 0x01;

    static const int br_v1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int br_v2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int sr_v1[4]  = {44100,48000,32000,0};
    static const int sr_v2[4]  = {22050,24000,16000,0};
    static const int sr_v25[4] = {11025,12000,8000,0};

    int bitrate, samplerate;
    if (version == 3)      { bitrate = br_v1[bitrate_idx] * 1000; samplerate = sr_v1[sr_idx]; }
    else if (version == 2) { bitrate = br_v2[bitrate_idx] * 1000; samplerate = sr_v2[sr_idx]; }
    else                   { bitrate = br_v2[bitrate_idx] * 1000; samplerate = sr_v25[sr_idx]; }
    if (bitrate == 0 || samplerate == 0) return -1;
    int flen = ((version == 3) ? 144 : 72) * bitrate / samplerate + padding;
    return (flen > 0) ? flen : -1;
}

/* R101: 取任意 MPEG 音频帧头的 layer 原始值(1=LayerIII/MP3, 2=LayerII/MP2, 3=LayerI)，
   仅用于诊断日志。Helix 只解 Layer III：SD 卡中混入的 MP2(扩展名却是 .mp3)会被
   mp3_frame_len_from_hdr 一律判非法，导致永久扫不到帧头。 */
static int mp3_hdr_layer(const unsigned char *p)
{
    if (!p || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return -1;
    return (p[1] >> 1) & 0x03;
}

static short      s_pcm[HELIX_PCM_SAMPLES];
static unsigned char s_in[HELIX_IN_BUF];
static size_t     s_in_len = 0;   /* s_in 中尚未喂给 Helix 的字节数 */

/* R101: 连续"整个输入缓冲都扫不到合法 Layer III 帧头"的次数。
   达阈值即判定本文件非 Helix 可解码格式(MP2/损坏/非音频)，快速放弃。 */
static int        s_no_valid_hdr_runs = 0;
#define HELIX_NO_HDR_MAX_RUNS   3

/* R091: 软件音量。Q15 固定点增益(32768=1.0 统一)，由 mp3_decoder_set_volume 设置，
   _mp3_helix_process 输出前缩放 PCM。替代 ADF 脆弱的 i2s ALC（IDF5.x 会 BREAK 崩溃）。 */
static int g_vol_gain_q15 = 32768;

void mp3_decoder_set_volume(int level)
{
    /* level 0..14 -> 线性增益 gain = level/14 (Q15: 32768=1.0)。
        用户要求"线性"：dB 曲线(即使 0..-50dB)最低几档仍低至 -40dB 以下经功放听不到。
        线性增益下第 1 档≈-23dB(可闻)、第 2/3 档 -17/-13.4dB(清楚)，高档步进小不跳变。 */
    if (level <= 0) {
        g_vol_gain_q15 = 0;
    } else if (level >= 14) {
        g_vol_gain_q15 = 32768;
    } else {
        g_vol_gain_q15 = (int)(((uint32_t)level * 32768u) / 14u);
    }
}

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
    /* R101: 每首新曲目重置模块级流式状态。s_in/s_in_len 与无帧头计数都跨 element
       生命周期保留，若上一首遗留(尤其"扫到无帧头但未放弃"时切歌)会污染新曲目的
       首帧判定，导致新曲目被误判不可解码而跳过。 */
    s_in_len = 0;
    s_no_valid_hdr_runs = 0;
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

/* R094: seek/跳曲前重置解码器。根因（经 mp3_dbg 实测确认）：
   s_in/s_in_len 为模块级 static，跨 pause/resume/seek 保留；更关键的是 Helix 解码器
   内部状态 c->decoder(MP3DecInfo，含位保留/哈夫曼等上下文) 在多次 seek 后与新位置
   的流严重失配——实测第三跳时数据落点为合法帧头(ff fd)却连续 51 帧 Helix 解帧失败
   (err_cnt 累加至 51 误返 AEL_IO_DONE 跳曲)。仅清 s_in/err_cnt 不够，必须重新初始化
   Helix 解码器，与切歌新建 decoder 效果一致：从新位置干净重新同步帧边界。
   注：首帧因位保留引用了"上一帧主数据"(已跳跃丢失)可能产出 1 帧轻微杂音或单帧错误，
   但次帧起主数据连续即可正常解码(err_cnt 在成功帧清零)，不会累积误判。 */
void mp3_decoder_libhelix_reset(audio_element_handle_t el)
{
    if (el) {
        helix_ctx_t *c = (helix_ctx_t *)audio_element_getdata(el);
        if (c) {
            c->err_cnt = 0;
            /* R098: 重建 Helix 解码器以重置内部位保留/哈夫曼状态，否则 seek 到合法帧边界后
               仍会连续解坏帧(err_cnt>50)误判曲终跳曲(R094 根因)。
               崩溃根因已确认是 frame_align 伪同步字(R095)导致垃圾 nSlots(R097 已修)，
               与重建本身无关。此处直接重建安全：调用时机为 pause_seek_resume(主任务、
               解码任务已暂停)，无并发。R095: 先分配新 decoder，成功才释放旧，避免 OOM 变 NULL。 */
            void *new_dec = MP3InitDecoder();
            if (new_dec) {
                MP3FreeDecoder(c->decoder);
                c->decoder = new_dec;
            } else {
                ESP_LOGE("mp3_dbg", "R098 MP3InitDecoder OOM, keep old decoder (seek may glitch)");
            }
        }
    }
    s_in_len = 0;
    s_no_valid_hdr_runs = 0;   /* R101: 新曲目/新位置重新计数，避免上一首的放弃状态残留 */
    g_seek_dbg_first = true;
}

/* R098: FF/REW secure: clear err_cnt only, no decoder free/realloc
   (avoids free-while-decoding tlsf double-free). */
void mp3_decoder_libhelix_clear_errors(audio_element_handle_t el)
{
    if (el) {
        helix_ctx_t *c = (helix_ctx_t *)audio_element_getdata(el);
        if (c) {
            c->err_cnt = 0;
        }
    }
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

    if (g_seek_dbg_first && s_in_len > 0) {
        ESP_LOGW("mp3_dbg", "R095 seek-data first %d bytes: %02x%02x%02x%02x %02x%02x%02x%02x (eos=%d)",
                 (int)s_in_len, s_in[0],s_in[1],s_in[2],s_in[3],s_in[4],s_in[5],s_in[6],s_in[7], eos);
        g_seek_dbg_first = false;
    }

    /* R098: 自对齐——丢弃 s_in 开头的非合法 Layer III 帧边界前导字节，确保 MP3Decode
       始终从合法帧边界起解。seek/暂停恢复后文件位置可能落在中帧，垃圾头会产生异常
       nSlots -> mp3dec.c:380 超大 memcpy 越界崩 Cache error。正常播放 s_in 已对齐(offset 0)，
       扫描零开销；仅错位时才前移。 */
    if (s_in_len >= 4) {
        size_t j = 0;
        while (j + 4 <= s_in_len && mp3_frame_len_from_hdr(s_in + j) <= 0) {
            j++;
        }
        /* R101: 扫完整个缓冲仍无合法 Layer III 帧头 -> 该文件不是 Helix 可解的 MP3。
           典型场景：实为 MP2(Layer II) 却以 .mp3 结尾、或文件损坏/非音频数据。
           旧逻辑每次只留 3 字节尾部、整块反复丢弃(实测连续 17 次 discarded 2045，
           静音约 1.3s)才靠 err_cnt>50 放弃；期间 decoder 忙循环不消费下游 ringbuf，
           上游 file 元素阻塞在写 ringbuf -> 切歌时 file task destroy 超时约 9s。
           这里连续 HELIX_NO_HDR_MAX_RUNS 次仍无帧头即直接放弃，快速跳曲。 */
        if (j + 4 > s_in_len) {
            s_no_valid_hdr_runs++;
            int sync_off = MP3FindSyncWord(s_in, (int)s_in_len);
            int lyr = (sync_off >= 0) ? mp3_hdr_layer(s_in + sync_off) : -1;
            ESP_LOGW("mp3_dbg", "R101 no LayerIII hdr in %dB (run=%d/%d, sync=%d layer=%d)",
                     (int)s_in_len, s_no_valid_hdr_runs, HELIX_NO_HDR_MAX_RUNS, sync_off, lyr);
            if (s_no_valid_hdr_runs >= HELIX_NO_HDR_MAX_RUNS) {
                ESP_LOGE("mp3_dbg", "R101 undecodable: layer=%d (need 1=LayerIII). skip.", lyr);
                s_no_valid_hdr_runs = 0;
                return AEL_IO_DONE;
            }
        } else {
            s_no_valid_hdr_runs = 0;
        }
        if (j > 0 && j < s_in_len) {
            memmove(s_in, s_in + j, s_in_len - j);
            s_in_len -= (size_t)j;
            ESP_LOGW("mp3_dbg", "R098 self-align discarded %d bytes", (int)j);
        }
    }

    if (eos && s_in_len == 0) {
        ESP_LOGW("mp3_dbg", "R095 DONE(input eos, s_in_len=0)");
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
                ESP_LOGI("Helix", "R100 diag: true_rate=%d ch=%d bits=%d (fi.samprate=%d nChans=%d)",
                         true_rate, true_ch, fi.bitsPerSample, fi.samprate, fi.nChans);
                audio_element_set_music_info(self, true_rate, true_ch, fi.bitsPerSample);
                c->last_rate  = true_rate;
                c->last_ch    = true_ch;
                c->last_bits  = fi.bitsPerSample;
            }
            size_t outlen = (size_t)fi.outputSamps * sizeof(short);
            if (g_vol_gain_q15 != 32768) {
                /* R091: 软件音量缩放 Q15，输出前处理，避免 i2s ALC 崩溃 */
                for (int i = 0; i < fi.outputSamps; i++) {
                    int v = ((int)s_pcm[i] * g_vol_gain_q15) >> 15;
                    if (v >  32767) v =  32767;
                    if (v < -32768) v = -32768;
                    s_pcm[i] = (short)v;
                }
            }
            /* R100: 单声道文件上混为立体声(每样本复制到 L+R)。
               I2S 固定 2 声道, 不混则单声道 PCM 被当立体声消耗 → 2x 快。
               原地反向交错避免覆盖(2i+1 > i 恒成立); 缓冲 2304 够容 1152→2304。 */
            if (fi.nChans == 1 && fi.outputSamps > 0 &&
                (int)fi.outputSamps * 2 <= HELIX_PCM_SAMPLES) {
                int n = (int)fi.outputSamps;
                for (int i = n - 1; i >= 0; i--) {
                    s_pcm[2*i + 1] = s_pcm[i];
                    s_pcm[2*i]     = s_pcm[i];
                }
                fi.outputSamps = n * 2;
                outlen = (size_t)fi.outputSamps * sizeof(short);
            }
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
                    ESP_LOGW("mp3_dbg", "R095 DONE(UNDERFLOW) err=%d s_in=%02x%02x%02x%02x", c->err_cnt, s_in[0],s_in[1],s_in[2],s_in[3]);
                    return AEL_IO_DONE;
                }
                /* R098: 满缓冲仍解不出一整帧(seek 后 MAINDATA 位保留缺失)。原地重读相同数据
                   会死循环，须按帧长/下一同步字前移，直至位保留落回缓冲恢复。 */
                int off = -1;
                if (have_hdr) {
                    int flen = mp3_frame_len_from_hdr(hdr);
                    if (flen > 0 && flen <= (int)s_in_len) off = flen;
                }
                if (off <= 0) { off = MP3FindSyncWord(s_in + 1, (int)s_in_len - 1); if (off >= 0) off += 1; }
                if (off <= 0) off = 1;
                memmove(s_in, s_in + off, s_in_len - (size_t)off);
                s_in_len -= (size_t)off;
                if (s_in_len == 0) {
                    break;
                }
                continue;   /* 前移后继续尝试解码下一帧 */
            }
            if (eos) {
                /* 上游已结束且残片不足一帧，直接结束（跳曲保护） */
                ESP_LOGW("mp3_dbg", "R095 DONE(UNDERFLOW eos)");
                return AEL_IO_DONE;
            }
            break;
        } else {
            /* 坏帧：定位下一个同步字跳过，连续坏帧过多则结束（跳曲） */
            c->err_cnt++;
            if (c->err_cnt > 50) {
                ESP_LOGW("mp3_dbg", "R095 DONE(BADFRAME) err=%d s_in=%02x%02x%02x%02x", c->err_cnt, s_in[0],s_in[1],s_in[2],s_in[3]);
                return AEL_IO_DONE;
            }
            /* R098: 用帧长跳过当前坏帧。seek/中途开始首帧因位保留(reservoir)缺失解坏，
               跳到下一帧边界后 1-2 帧内位保留数据落回缓冲即恢复。避免旧逻辑
               MP3FindSyncWord(s_in,len) 停在当前帧头(offset 0)原地死循环累计 51 坏帧误跳曲。 */
            int off = -1;
            if (have_hdr) {
                int flen = mp3_frame_len_from_hdr(hdr);
                if (flen > 0 && flen <= (int)s_in_len) {
                    off = flen;
                }
            }
            if (off <= 0) {
                off = MP3FindSyncWord(s_in + 1, (int)s_in_len - 1);
                if (off >= 0) off += 1;
            }
            if (off <= 0) off = 1;   /* 兜底：保证前移，避免死循环 */
            memmove(s_in, s_in + off, s_in_len - (size_t)off);
            s_in_len -= (size_t)off;
            if (s_in_len == 0) {
                if (eos) {
                    return AEL_IO_DONE;
                }
                break;
            }
        }
    }

    // R086: 本帧未能解出(out_total==0，多为输入尚不足一整帧的 UNDERFLOW)时，必须返回
    // AEL_IO_TIMEOUT 而非 AEL_IO_OK。ADF 的 audio_element_process_running 把 AEL_IO_OK(0)
    // 与 AEL_IO_DONE 同等对待(直接 set_ringbuf_done + finish)，会导致 decoder 一恢复/seek 后
    // 因“暂无解码输出”被误判曲终跳下一首。AEL_IO_TIMEOUT 让 ADF 继续等待后续输入重试，
    // 真正的曲终仍由 eos 分支(AEL_IO_DONE)处理。
    return (out_total > 0) ? out_total : AEL_IO_TIMEOUT;
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
