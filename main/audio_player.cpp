/**
 * @file audio_player.cpp
 * @brief 音频播放引擎实现
 *
 * 核心设计变更（评审修正）：
 * 1. 每次 play() 重建 pipeline，避免 terminate 后复用失败 (S-09)
 * 2. WAV 使用 wav_decoder，不回退到 mp3_decoder (M-09)
 * 3. seek/tick 使用毫秒级精度 (M-03/M-04)
 * 4. 跳帧仅在 ≥8x 最高档位执行，1.5x/2.0x/3.0x 仅变速不跳帧（M-10，S11 修正）
 * 5. 移除未使用的 opus_decoder.h (L-01)
 * 6. M3：估算时长按格式选 bytes/ms 系数（MP3/AAC/OGG=16, FLAC=64, Opus=12, WAV=176）
 * 7. S5：seek_ms 保留原暂停态
 * 8. S6：负速度按 |speed| 拉高 I2S 采样率实现变调快退
 * 9. M5：i2s element register 前 NULL 守卫
 */

#include "audio_player.h"
#include "config.h"
#include "tape_control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include "display.h"  // R074: 调 display_request_main_tick() 强制切歌后 player 渲染

#ifdef CONFIG_USE_ESP_ADF

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"   // R076-CODEC-18: 监听 decoder REPORT_MUSIC_INFO 调 i2s_stream_set_clk
// R035-014：审计确认 audio_common.h 不是间接依赖（注释后构建通过 exit 0），正式删除
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "mp3_decoder.h"
#include "mp3_decoder_esp_codec.h"   // R076-CODEC-7: 开源 MP3 解码器替代闭源 PV-MP3
#include "mp3_decoder_libhelix.h"    // R080: Helix MP3 替代闭源 PV-MP3（不崩+坏帧跳过）
#include "aac_decoder_esp_codec.h"   // R122-AAC-CODEC: 开源 AAC 解码器替代闭源 aac_decoder
#include "flac_decoder.h"
#include "flac_decoder_esp_codec.h"   // R122-FLAC-CODEC: 开源 FLAC 解码器替代闭源 flac_decoder(崩)
#include "ogg_decoder.h"
#include "wav_decoder.h"
#include "wav_bitdepth_conv.h"   // R123-WAV: WAV 原始 PCM(16/24/32bit) -> 16bit 转换 element
#include "esp_timer.h"
#include "filter_resample.h"
#include "driver/i2s.h"        // 遗留 I2S 驱动: i2s_set_pin 显式绑定 GPIO
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>
// 2026-07-03 R003: 注释 board.h（项目用 MAX98357 + SSD1306 非 ADF 开发板，未配置 audio_board Kconfig，
//   而代码未实际使用 board.h 中任何 API）
// #include "board.h"

#if defined(CONFIG_USE_BT_SPEAKER)
#include "bt_speaker.h"
static bool g_bt_active = false;           // BT 音箱模式是否激活（复用 g_i2s_writer）
#endif

static const char *TAG = "audio_player";

/* --- 全局状态 --- */
static audio_pipeline_handle_t  g_pipeline = NULL;
static audio_element_handle_t   g_fatfs_reader = NULL;
static audio_element_handle_t   g_decoder = NULL;
static audio_element_handle_t   g_flac_conv = NULL;    // R122-FLAC-16BIT: FLAC 24bit->16bit 转换 element (复用 WAV 已验证路径)
static audio_element_handle_t   g_i2s_writer = NULL;   // R068：每次 play 重建（弃用 R036-001 跨曲目复用）

static uint32_t     g_wav_data_off = 0;  // R123-WAV: WAV data 块偏移(跳过头部用 + 时长修正)
static bool         g_is_wav = false;     // R123-WAV: 当前曲目是否为 WAV(变速/采样率切换时据此保持 16bit I2S)
static bool         g_is_flac = false;    // R122-FLAC-16BIT: 当前曲目是否为 FLAC(24bit 走 16bit 转换, 规避 I2S 24bit 越界)

static bool         g_is_playing = false;
static bool         g_is_paused = false;
static int          g_volume = AUDIO_OUTPUT_VOL;
static int          g_volume_saved = AUDIO_OUTPUT_VOL;
static bool         g_scrub_active = false;   /* R099: FF/REW 期间进度不计播放流逝，仅由 seek 推进 */

/* V1.2 音量 dB 线性映射边界 (MAX98357A ALC 范围) */
#define VOL_DB_MIN  (-96)   // 静音
#define VOL_DB_MAX  (12)    // 最大增益
static int          g_total_duration_ms = 0;
static uint32_t     g_total_file_bytes = 0;
// R067：当前曲目 ID3v2 标签字节数（0 = 无 ID3 或非 MP3）。
// seek byte_pos = id3_skip + (ms * audio_bytes / duration_ms)，
// audio_bytes = total_file_bytes - id3_skip_bytes。
static int          g_id3_skip_bytes = 0;
static char         g_seek_path[256] = {0};  // R085: 保存当前曲目真实路径，供 seek 帧对齐扫描使用
static int          g_current_sample_rate = AUDIO_SAMPLE_RATE;  // R076-CODEC: I2S 当前采样率缓存 (去重 i2s_set_clk 调用)
static int          g_base_sample_rate   = AUDIO_SAMPLE_RATE;  // R083: 当前曲目真实基准速率(供 speed 倍率计算)
static int          g_file_bits = 16;                    // R122-WAV: 当前曲目真实位深(供 speed 变速保持，避免 24bit 退回 16bit)
static int          g_file_ch   = 2;                     // R122-WAV: 当前曲目真实声道(供 speed 变速保持)


static uint64_t     g_play_start_us = 0;               // 本次播放起始（pause/resume 时重置）
static int64_t      g_play_offset_us = 0;              // pause 时锁存的已播放时长，resume 时叠加
static uint64_t     g_last_scrub_us = 0;               // M1: 上次跳帧时间戳（模块级全局）

/* R049b：A-B 区间复读状态（ms，-1=未标记） */
static int  g_ab_a_ms = -1;
static int  g_ab_b_ms = -1;
static bool g_ab_enabled = false;
static int  g_ab_loop_count = 0;   /* R111: A-B 复读遍数计数 */

/* --- R067-fix：ID3v2 跳过工具 ---
 * 问题：ESP-ADF v5.5 + esp_audio_codec 静态库的 mp3 decoder 在含 ID3v2
 *       标签的 MP3 文件上稳定崩（Guru Meditation BREAK，
 *       CODEC_ELEMENT_HELPER: reserve data 2 is 0x0）。
 *       现象：1/4/5 有 ID3v2 → 必崩；2/3/6 无 ID3v2 → 正常。
 * 解决：fatfs_stream 不会自动跳 ID3v2。在 set_uri 前我们手动 peek
 *       前 10 字节 → 解析 syncsafe size → 让 audio_element_set_byte_pos
 *       把 reader 起点设到 MP3 frame 开始。decoder 收到的就是纯音频数据。
 * 注意：seek/resume 走 g_total_file_bytes 比例映射，ID3 size 必须计入
 *       g_total_file_bytes 才能正确换算 ms ↔ byte_pos（见 seek path）。*/

// 计算 MP3 文件前部的 ID3v2 总长度（syncsafe size 解析）。返回值：
//   -1：未检测到 ID3v2 (非 mp3 / 无标签 / 文件 < 10B)
//    N：ID3v2 标签总长度（含 10B header），seek 到 file[N] 就是首个 MP3 frame
static int id3v2_total_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    uint8_t hdr[10];
    size_t n = fread(hdr, 1, 10, fp);
    fclose(fp);
    if (n < 10) return -1;
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return -1;
    if (hdr[3] == 0 || hdr[4] == 0xFF) return -1;       // v2.2/2.4 暂不处理
    // syncsafe: 4 字节 each 用 7 位
    uint32_t sz = ((uint32_t)(hdr[6] & 0x7F) << 21)
                | ((uint32_t)(hdr[7] & 0x7F) << 14)
                | ((uint32_t)(hdr[8] & 0x7F) <<  7)
                | ((uint32_t)(hdr[9] & 0x7F));
    int total = 10 + (int)sz;
    return total > 0 ? total : -1;
}

// R083/R085: 打开文件嗅探首个 MP3 音频帧头，得到真实采样率与码率。
// 采样率用于把 I2S 时钟设成文件实际速率（否则被 R076-CODEC 锁死 48000Hz 放快）；
// 码率用于按真实比特率估算 duration（替代固定 128kbps 估算，修复进度条到不了一端）。
// 返回采样率(Hz)，失败返回 0；*bitrate_kbps 输出码率(kbps)，失败置 0。需跳过 ID3v2(id3_sz)。
/* layer_out: 输出 MPEG 帧头层字段(1=LayerIII/MP3, 2=LayerII/MP2, 3=LayerI)。
   R101: 供上层在建流前拒绝 Helix 不支持的格式——Helix 只解 Layer III，遇到扩展名
   是 .mp3 实为 MP2 的文件会一直扫不到帧头、最终误判曲终。 */
static int mp3_sniff_sample_rate(const char *path, int id3_sz, int *bitrate_kbps, int *channels, int *layer_out)
{
    if (bitrate_kbps) *bitrate_kbps = 0;
    if (channels)     *channels     = 2;   /* 默认立体声 */
    if (layer_out)    *layer_out    = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, id3_sz, SEEK_SET) != 0) { fclose(fp); return 0; }
    uint8_t buf[1024];   // R083-fix: 仅需扫首个帧头(4B)，1KB 足够；避免大栈数组撑爆 main 任务栈
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    static const int rates[3][4] = {
        {11025, 12000, 8000, 0},   // MPEG2.5
        {22050, 24000, 16000, 0},  // MPEG2
        {44100, 48000, 32000, 0},  // MPEG1
    };
    // 码率表 [version:0=MPEG2.5,1=MPEG2,2=MPEG1][layer:0=I,1=II,2=III][bitrate_index 1..14]
    static const int br[3][3][14] = {
        { {32,48,56,64,80,96,112,128,144,160,176,192,224,256},    // MPEG2.5 L1
          { 8,16,24,32,40,48,56,64,80,96,112,128,144,160},        // MPEG2.5 L2
          { 8,16,24,32,40,48,56,64,80,96,112,128,144,160} },      // MPEG2.5 L3
        { {32,48,56,64,80,96,112,128,144,160,176,192,224,256},    // MPEG2 L1
          { 8,16,24,32,40,48,56,64,80,96,112,128,144,160},        // MPEG2 L2
          { 8,16,24,32,40,48,56,64,80,96,112,128,144,160} },      // MPEG2 L3
        { {32,64,96,128,160,192,224,256,288,320,352,384,416,448}, // MPEG1 L1
          {32,48,56,64,80,96,112,128,160,192,224,256,320,384},    // MPEG1 L2
          {32,40,48,56,64,80,96,112,128,160,192,224,256,320} },   // MPEG1 L3
    };
    for (size_t i = 0; i + 4 <= n; i++) {
        uint8_t b0 = buf[i], b1 = buf[i + 1], b2 = buf[i + 2];
        // 帧同步 11bit + 合法 layer(非00) + 合法 bitrate(非1111) + 合法 samplerate(非11)
        if (b0 == 0xFF && (b1 & 0xE0) == 0xE0 &&
            (b1 & 0x06) != 0 && (b2 & 0xF0) != 0xF0 && (b2 & 0x0C) != 0x0C) {
            int vbits = (b1 >> 3) & 3;
            int ver = (vbits == 3) ? 2 : (vbits == 2) ? 1 : 0;  // 3=MPEG1,2=MPEG2,0=MPEG2.5
            int layer = (b1 >> 1) & 3;                           // MPEG 帧头层字段: 1=III,2=II,3=I
            int bi = (b2 >> 4) & 0x0F;                           // bitrate index 1..14
            int sri = (b2 >> 2) & 3;
            int rate = rates[ver][sri];
            if (rate > 0 && layer >= 1 && bi >= 1 && bi <= 14) {
                int li = (layer == 3) ? 0 : (layer == 2) ? 1 : 2;   /* L1,L2,L3 表索引 */
                int br_kbps = br[ver][li][bi - 1];
                /* R100: 验证下一帧——按本帧头算出帧长，看偏移处是否又一个合法同步。
                   拒绝 ID3 二进制残余里的假同步字(常被误判 44100 → 拔快)。 */
                int padding = (b2 & 0x02) ? 1 : 0;
                int flen = ((ver == 2) ? 144 : 72) * (br_kbps * 1000) / rate + padding;
                size_t next = i + (size_t)flen;
                bool verified = false;
                if (next + 4 <= n) {
                    uint8_t n0 = buf[next], n1 = buf[next + 1], n2 = buf[next + 2];
                    if (n0 == 0xFF && (n1 & 0xE0) == 0xE0 && (n1 & 0x06) != 0 &&
                        (n2 & 0xF0) != 0xF0 && (n2 & 0x0C) != 0x0C) {
                        verified = true;
                    }
                } else {
                    verified = true;  /* 缓冲不够看下一帧,信任本帧(回退原行为) */
                }
                if (verified) {
                    if (bitrate_kbps) *bitrate_kbps = br_kbps;
                    if (layer_out)    *layer_out    = layer;
                    /* R100: channel mode 在第 4 字节高 2 位: 00 stereo/01 joint/10 dual/11 mono */
                    if (channels) {
                        uint8_t b3 = buf[i + 3];
                        int ch_mode = (b3 >> 6) & 3;
                        *channels = (ch_mode == 3) ? 1 : 2;
                    }
                    ESP_LOGI(TAG, "R100 diag: sniff hdr@+%zu bytes=%02X %02X %02X %02X ver=MPEG%d layer=%d sr=%d br=%d ch=%d flen=%d",
                             i, b0, b1, b2, buf[i+3], (ver==2?1:(ver==1?2:0)), layer, rate, br_kbps, channels?*channels:2, flen);
                    return rate;
                }
            }
        }
    }
    return 0;
}


/* R116: WAV sample rate/channels/bits sniff from RIFF/WAVE header */
static int wav_sniff_sample_rate(const char *path, int *channels_out, int *bits_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t hdr[44];
    size_t rd = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    if (rd < 44) return 0;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return 0;
    int channels = hdr[22] | (hdr[23] << 8);
    int rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    int bits = hdr[34] | (hdr[35] << 8);
    if (channels_out) *channels_out = channels;
    if (bits_out)     *bits_out     = bits;
    return rate;
}

/* R123-WAV: 完整解析 WAV 头，返回采样率/声道/位深，并定位 data 块起始偏移。
   跳过 RIFF/fmt/data 等所有头部块，保证 PCM 采样对齐（避免 L/R 错位与起始爆音），
   也用于准确估算时长（头部字节不计入音频数据量）。 */
static int wav_parse_header(const char *path, int *channels_out, int *bits_out, uint32_t *data_off_out, int *is_float_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) { fclose(fp); return 0; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(fp); return 0; }
    int rate = 0, ch = 0, bits = 0;
    int is_float = 0;
    uint32_t data_off = 0;
    bool got_fmt = false, got_data = false;
    uint8_t chunk[8];
    while (fread(chunk, 1, 8, fp) == 8) {
        uint32_t sz = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | (chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            int read_n = (sz < 40) ? (int)sz : 40;
            uint8_t f[40];
            if (fread(f, 1, read_n, fp) == (size_t)read_n) {
                int fmt_tag = f[0] | (f[1] << 8);   /* 1=PCM, 3=IEEE float, 0xFFFE=EXTENSIBLE */
                ch   = f[2] | (f[3] << 8);
                rate = f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24);
                int byterate = f[8] | (f[9] << 8) | (f[10] << 16) | (f[11] << 24);
                bits = f[14] | (f[15] << 8);
                is_float = 0;
                if (fmt_tag == 3) {
                    is_float = 1;
                } else if (fmt_tag == 0xFFFE && read_n >= 40) {
                    /* R131: WAVEFORMATEXTENSIBLE 布局: cbSize@16 wValidBits@18 dwChannelMask@20
                       SubFormat GUID@24(前4字节=真实format)。旧代码误从@22读, 偏移错2字节。 */
                    int sub_fmt = f[24] | (f[25] << 8) | (f[26] << 16) | (f[27] << 24);
                    int valid_bits = f[18] | (f[19] << 8);
                    if (sub_fmt == 3) is_float = 1;
                    /* 真 24-in-32(有效位宽 < 容器位宽)时收窄到有效位宽 */
                    if (valid_bits > 0 && valid_bits < bits) bits = valid_bits;
                    ESP_LOGI(TAG, "R131: EXTENSIBLE sub_fmt=0x%08x valid_bits=%d", sub_fmt, valid_bits);
                }
                /* R131b: 头部 bits 字段可能与实际打包不符(如 24bit 打包却标 32bit, 导致 conv 按 4
                   字节/样本错位读取→杂音)。nAvgBytesPerSec 反映真实打包字节数, 据此反推真实位深。 */
                int bits_hdr = bits;
                if (byterate > 0 && rate > 0 && ch > 0) {
                    int frame = byterate / rate;           /* 每帧字节数 */
                    if (frame > 0 && (frame % ch) == 0) {
                        int bps = frame / ch;              /* 每样本字节数: 2/3/4 */
                        if      (bps == 2) bits = 16;
                        else if (bps == 3) bits = 24;
                        else if (bps == 4) bits = 32;
                    }
                }
                ESP_LOGI(TAG, "R131: fmt_tag=0x%04x hdr_bits=%d byterate=%d -> bits=%d is_float=%d",
                         fmt_tag, bits_hdr, byterate, bits, is_float);
                got_fmt = true;
            }
            if (sz > (uint32_t)read_n) fseek(fp, (long)(sz - read_n), SEEK_CUR);
        } else if (memcmp(chunk, "data", 4) == 0) {
            long pos = ftell(fp);
            if (pos >= 0) data_off = (uint32_t)pos;
            got_data = true;
            break;
        } else {
            if (sz & 1) sz++;   // WAV 块按字对齐
            fseek(fp, (long)sz, SEEK_CUR);
        }
    }
    fclose(fp);
    if (!got_fmt || !got_data) return 0;
    if (channels_out) *channels_out = ch;
    if (bits_out)     *bits_out     = bits;
    if (data_off_out) *data_off_out = data_off;
    if (is_float_out) *is_float_out = is_float;
    return rate;
}

static audio_status_cb_t g_status_cb = NULL;
static void              *g_user_data = NULL;

/* ============================================================
 * 辅助：根据文件扩展名选择解码器
 * ============================================================ */
static const char *get_file_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

/* R122-AAC: 嗅探 .aac/.m4a 真实封装，避免把 MP4 容器当 ADTS 喂给解码器导致 BREAK 崩机。
   返回 true=ADTS 裸流(可解)；false=MP4 容器/未知封装(不支持，应拒绝)。 */
static bool aac_probe_is_adts(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 4) return false;
    /* 1) 扫描 ADTS 同步字 0xFFF (MPEG-4, layer=0) */
    for (size_t i = 0; i + 1 < n; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xF0) == 0xF0 && (buf[i + 1] & 0x06) == 0x00) {
            return true;
        }
    }
    /* 2) 无 ADTS 同步字：检查是否为 MP4 容器（穿 .aac 马甲的 mp4/aac_02） */
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(buf + i, "ftyp", 4) == 0) {
            ESP_LOGE(TAG, "R122-AAC: %s is MP4 container, AAC decoder only supports ADTS; reject", path);
            return false;
        }
    }
    ESP_LOGE(TAG, "R122-AAC: %s has no ADTS sync and is not MP4; unsupported AAC encapsulation", path);
    return false;
}

/* R122-FLAC: 解析 FLAC STREAMINFO 精确时长（total_samples / sample_rate），
   替代固定 64 bytes/ms 估算（对 24bit/48k FLAC 严重偏低，进度条虚长→曲终被误判"切割"）。
   返回时长(ms)，失败返回 0。 */
static int flac_parse_duration_ms(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 8) return 0;
    int off = -1;
    for (size_t i = 0; i + 4 <= n; i++) {           // 定位 'fLaC'（容错：跳过前置 ID3v2 等）
        if (buf[i] == 'f' && buf[i+1] == 'L' && buf[i+2] == 'a' && buf[i+3] == 'C') {
            off = (int)i; break;
        }
    }
    if (off < 0) return 0;
    size_t idx = (size_t)off + 4;                   // 第一个 metadata block 头
    if (idx + 4 > n) return 0;
    uint8_t b0  = buf[idx];
    int    type = b0 & 0x7F;
    uint32_t len = ((uint32_t)buf[idx+1] << 16) | ((uint32_t)buf[idx+2] << 8) | buf[idx+3];
    if (type != 0 || len < 34 || idx + 4 + len > n) return 0;   // 只要 STREAMINFO
    const uint8_t *d = buf + idx + 4;
    uint32_t sample_rate = ((uint32_t)d[10] << 12) | ((uint32_t)d[11] << 4) | (d[12] >> 4);
    uint64_t total = ((uint64_t)(d[13] & 0x0F) << 32)
                   | ((uint32_t)d[14] << 24) | ((uint32_t)d[15] << 16)
                   | ((uint32_t)d[16] << 8) | d[17];
    if (sample_rate == 0 || total == 0) return 0;
    return (int)(total * 1000 / sample_rate);
}

/* R122-FLAC: 从 STREAMINFO 取真实采样率（FLAC 无 sniff 会令 i2s 锁 48000，把 44.1k 放快 ~1.09x）。
   与 flac_parse_duration_ms 共用 STREAMINFO 解析。 */
static int flac_sniff_sample_rate(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 8) return 0;
    int off = -1;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] == 'f' && buf[i+1] == 'L' && buf[i+2] == 'a' && buf[i+3] == 'C') { off = (int)i; break; }
    }
    if (off < 0) return 0;
    size_t idx = (size_t)off + 4;
    if (idx + 4 > n) return 0;
    int type = buf[idx] & 0x7F;
    uint32_t len = ((uint32_t)buf[idx+1] << 16) | ((uint32_t)buf[idx+2] << 8) | buf[idx+3];
    if (type != 0 || len < 34 || idx + 4 + len > n) return 0;
    const uint8_t *d = buf + idx + 4;
    uint32_t sample_rate = ((uint32_t)d[10] << 12) | ((uint32_t)d[11] << 4) | (d[12] >> 4);
    return (int)sample_rate;
}

/* R122-FLAC-16BIT: 从 STREAMINFO 取真实位深(bits per sample)，供位深转换 element 配置 src_bits。
   与 flac_sniff_sample_rate 共用 'fLaC' 定位逻辑；bits-per-sample index 在 STREAMINFO 第 12 字节 bit[3:1]。 */
static int flac_sniff_bits(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 16;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 8) return 16;
    int off = -1;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] == 'f' && buf[i+1] == 'L' && buf[i+2] == 'a' && buf[i+3] == 'C') { off = (int)i; break; }
    }
    if (off < 0) return 16;
    size_t idx = (size_t)off + 4;
    if (idx + 4 > n) return 16;
    int type = buf[idx] & 0x7F;
    uint32_t len = ((uint32_t)buf[idx+1] << 16) | ((uint32_t)buf[idx+2] << 8) | buf[idx+3];
    if (type != 0 || len < 34 || idx + 4 + len > n) return 16;
    const uint8_t *d = buf + idx + 4;
    // R122-FLAC-16BIT: 正确提取 STREAMINFO 的 5-bit bits-per-sample 字段。
    // 布局(d 指向 STREAMINFO data 起点): d[12] 的 bit[0] = 位深 index 最高位,
    // d[13] 的 bit[7:4] = 位深 index 低 4 位; 实际位深 = index + 1。
    // 旧实现误取 d[12] bit[3:1](那是声道数-1) 导致 24bit 被识别成 8bit, 使 conv 错位越界崩溃。
    int bps_index = ((d[12] & 0x01) << 4) | (d[13] >> 4);
    int bits = bps_index + 1;
    if (bits < 8 || bits > 32) bits = 16;   // 非法值兜底
    return bits;
}

/* R122-AAC: 从 ADTS 帧头取采样率（AAC 无嗅探会令 i2s 锁 48000，把 44.1k 放快 ~1.09x，与 FLAC 同因）。
   与 aac_probe_is_adts 共用 ADTS 同步字扫描。采样率索引表见 ISO/IEC 13818-7 Table 8.2。 */
static const int kAacSampRateTable[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};
static int aac_sniff_sample_rate(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 4) return 0;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] == 0xFF && (buf[i+1] & 0xF0) == 0xF0) {   // ADTS 同步字 0xFFF (layer=0)
            int idx = (buf[i+2] >> 2) & 0x0F;
            if (idx < 16 && kAacSampRateTable[idx] > 0) return kAacSampRateTable[idx];
        }
    }
    return 0;
}

static audio_element_handle_t create_decoder(const char *path)
{
    const char *ext = get_file_ext(path);

    mp3_decoder_cfg_t  mp3_cfg  = DEFAULT_MP3_DECODER_CONFIG();
    ogg_decoder_cfg_t  ogg_cfg  = DEFAULT_OGG_DECODER_CONFIG();
    wav_decoder_cfg_t  wav_cfg  = DEFAULT_WAV_DECODER_CONFIG();
    /* R117: AAC 输出 ringbuffer 用 16KB 避免欠载(见 DEFAULT_AAC_DECODER_ESP_CODEC_CONFIG)。
       (FLAC/AAC 均已改用 esp_codec 开源解码器, 见 flac_decoder_esp_codec.c / aac_decoder_esp_codec.c) */

    if (strcasecmp(ext, ".mp3") == 0) {
        // R080: 换 Helix MP3 解码器（chmorgan/esp-libhelix-mp3, Apache-2.0）替代闭源 PV-MP3。
        // PV-MP3(minimp3) 对本批特定合法 MP3 确定性 BREAK(@0x403743c0) 崩溃（转码 128k/320k 均复现）；
        // Helix 健壮性更好：错误返回负码不崩溃，坏帧自动跳过，连续错误超阈值返回 DONE 触发跳曲保护。
        // ID3v2 仍由 play() 中 audio_element_set_byte_pos 手动跳过（decoder 收到纯音频帧）。
        ESP_LOGI(TAG, "Using Helix MP3 decoder (R080, replace PV-MP3)");
        return mp3_decoder_libhelix_init(NULL);
    } else if (strcasecmp(ext, ".aac") == 0 || strcasecmp(ext, ".m4a") == 0) {
        if (!aac_probe_is_adts(path)) {
            ESP_LOGE(TAG, "Using AAC decoder: rejected (unsupported encapsulation)");
            return NULL;
        }
        ESP_LOGI(TAG, "Using AAC decoder (esp_codec, R122-AAC-CODEC)");
        return aac_decoder_esp_codec_init(NULL);
    } else if (strcasecmp(ext, ".flac") == 0) {
        ESP_LOGI(TAG, "Using FLAC decoder (esp_codec, R122-FLAC-CODEC)");
        /* R122-FLAC-CODEC: 用 esp_audio_codec 的开源 FLAC 解码器(simple_dec 路径)
           替代 ADF 闭源 flac_decoder —— 闭源解码器在 16/24bit 上均有内部 PCM 缓冲越界,
           污染 PSRAM/LVGL 导致 Guru Meditation, 无法 patch。新解码器与已验证 MP3 同源,
           正确输出 16/24bit 并上报 music_info, 彻底消除崩溃。 */
        return flac_decoder_esp_codec_init(NULL);
    } else if (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".opus") == 0) {
        ESP_LOGI(TAG, "Using OGG/OPUS decoder");
        return ogg_decoder_init(&ogg_cfg);
    } else if (strcasecmp(ext, ".wav") == 0) {
        /* R117: WAV is uncompressed PCM — skip decoder entirely (file->i2s direct).
           Eliminates WAV decoder accumulated-bytes false-EOF bug on repeated seeks. */
        ESP_LOGI(TAG, "WAV PCM passthrough (no decoder)");
        return NULL;
    }

    ESP_LOGW(TAG, "Unknown format %s, trying MP3 decoder", ext);
    return mp3_decoder_init(&mp3_cfg);
}

/* ============================================================
 * 初始化（仅创建 I2S 输出流，pipeline 在 play() 中重建）
 * ============================================================ */
// R068-fix：抽 helper 让 init 和 play() 都能复用 i2s_writer 创建
static audio_element_handle_t create_i2s_writer(void)
{
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = false;  // R091: ALC 在 IDF5.x 崩溃(alc_volume_setup_process BREAK)，音量改由 decoder 软件缩放
    i2s_cfg.std_cfg.gpio_cfg.bclk = I2S_BCK_IO;
    i2s_cfg.std_cfg.gpio_cfg.ws   = I2S_WS_IO;
    i2s_cfg.std_cfg.gpio_cfg.dout = I2S_DOUT_IO;
    i2s_cfg.std_cfg.gpio_cfg.din  = GPIO_NUM_NC;
    ESP_LOGI(TAG, "i2s_stream_init enter (BCLK=IO%d WS=IO%d DIN=IO%d)",
             I2S_BCK_IO, I2S_WS_IO, I2S_DOUT_IO);
    vTaskDelay(pdMS_TO_TICKS(5));
    audio_element_handle_t h = i2s_stream_init(&i2s_cfg);
    ESP_LOGI(TAG, "i2s_stream_init returned %p", (void *)h);
    if (h) {
        ESP_LOGI(TAG, "I2S pins bound: BCLK=IO%d WS=IO%d DIN=IO%d",
                 I2S_BCK_IO, I2S_WS_IO, I2S_DOUT_IO);
    } else {
        ESP_LOGE(TAG, "i2s_stream_init failed");
    }
    return h;
}

void audio_player_init(void)
{
    ESP_LOGI(TAG, "Initializing audio subsystem...");
    vTaskDelay(pdMS_TO_TICKS(5));  /* 强制 flush 串口，避免阻塞前日志丢失 */

    // R068-fix：i2s_writer 不再"跨曲目复用"——每次 play() 都重建。
    // 原因：R036-001 的复用策略在跨曲目时与旧 element task 状态耦合，
    // 导致 R066/R067 修复未根除 BREAK（0x403743bd）。代价：~100ms 重建开销。
    g_i2s_writer = create_i2s_writer();

    ESP_LOGI(TAG, "Audio subsystem initialized (I2S writer %s)",
             g_i2s_writer ? "ready" : "FAILED-but-ignored");
}

/* ============================================================
 * 播放（每次重建 pipeline + 元素，避免 terminate 后复用 Bug）
 * ============================================================ */
bool audio_player_play(const char *filepath)
{
    if (!filepath || !*filepath) return false;

    ESP_LOGI(TAG, "Playing: %s", filepath);
    audio_player_stop(); // 确保上一个管道已销毁
    // R062-fix：复用 g_i2s_writer 跨 play 时，上一轮 stop 已将其置为
    // AEL_STATE_FINISHED，若不 reset 直接重新 register/run，i2s 元素 resume
    // 时仍为 finished 态 → pipeline 误判播放完成 → 无限重启（听不到声音）。
    // 这里在重建 pipeline 前将其强制拉回 INIT 态。
    if (g_i2s_writer) {
        audio_element_reset_state(g_i2s_writer);
    }

    // R049b：新曲目清空 A-B 标记（避免跨文件失效）
    g_ab_a_ms = -1;
    g_ab_b_ms = -1;
    g_ab_enabled = false;
    g_ab_loop_count = 0;   /* R111 */

    // 1. 创建 pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_cfg.rb_size = 32 * 1024;  /* R117: 8KB->32KB, reduce FLAC/AAC underflow clicking */
    g_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!g_pipeline) {
        ESP_LOGE(TAG, "Failed to create audio pipeline");
        return false;
    }

    // 2. 创建 FATFS 文件读取器
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    g_fatfs_reader = fatfs_stream_init(&fatfs_cfg);
    if (!g_fatfs_reader) {
        ESP_LOGE(TAG, "Failed to create FATFS reader");
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 3. 创建解码器（WAV 格式返回 NULL = PCM 直通，不需要解码器）
    bool is_wav = (strcasecmp(get_file_ext(filepath), ".wav") == 0);
    bool is_flac = (strcasecmp(get_file_ext(filepath), ".flac") == 0);
    g_is_wav = is_wav;   // R123-WAV: 记录当前格式，变速/采样率切换时保持 16bit I2S
    g_is_flac = is_flac; // R122-FLAC-16BIT: 记录当前格式，强制 16bit I2S 路径
    g_flac_conv = NULL;  // R122-FLAC-16BIT: 每首重新挂接; 上一首的 conv 已由 stop 销毁

    // R136（前置拒绝，根治 R135 仍崩）：在创建 pipeline/file/decoder/i2s 之前先嗅探 FLAC 位深，
    // 非 16bit 直接拒绝并 return，**绝不创建任何 element**。原因：R135 把拒绝分支改为只调
    // audio_pipeline_deinit，看似干净，但 24bit 的 pipeline 处于"已 register 未 run"态，裸 deinit
    // 可能让 esp_codec FLAC 元素的内部 ringbuf/buffer 释放不完整 → 堆元数据损坏 → 紧随其后的 16bit
    // FLAC 从同一堆分到坏块、运行越界踩 LVGL(Guru Meditation @ lv_refr)。提前拒绝使 24bit 分支
    // 完全不碰堆，根除该污染。若改后 16bit 续播仍崩，则根因是 FLAC 解码器 16bit 本身(转 C 方案)。
    if (is_flac) {
        const char *flac_path = strstr(filepath, "://");
        const char *real_flac = flac_path ? flac_path + 3 : filepath;
        if (real_flac[0] == '/' && real_flac[1] == '/') real_flac++;
        int flac_bits_pre = flac_sniff_bits(real_flac);
        if (flac_bits_pre != 16) {
            ESP_LOGW(TAG, "R122-FLAC-16BIT: %d-bit FLAC rejected (pre-pipeline, zero heap touch); only 16bit supported", flac_bits_pre);
            display_toast("FLAC must be\n16bit\ntranscode pls");
            return false;
        }
    }

    g_decoder = create_decoder(filepath);
    if (!g_decoder && !is_wav) {
        ESP_LOGE(TAG, "Failed to create decoder");
        audio_element_deinit(g_fatfs_reader);
        g_fatfs_reader = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 4. 注册元素到管道
    // R035-015：audio_pipeline_register 失败时清理已注册的元素，避免句柄泄漏
    if (audio_pipeline_register(g_pipeline, g_fatfs_reader, "file") != ESP_OK) {
        ESP_LOGE(TAG, "register fatfs_reader failed");
        audio_element_deinit(g_fatfs_reader);
        g_fatfs_reader = NULL;
        audio_element_deinit(g_decoder);
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }
    if (g_decoder && audio_pipeline_register(g_pipeline, g_decoder, "decoder") != ESP_OK) {
        ESP_LOGE(TAG, "register decoder failed");
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_element_deinit(g_fatfs_reader);
        g_fatfs_reader = NULL;
        audio_element_deinit(g_decoder);
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // R076-DBG：decoder 调试日志已在其创建分支拉满（esp_log_level_set DEBUG），
    // 让 minimp3 在主动 abort 前打印内部错误（帧头非法/采样率越界等）。
    // R068-fix：每首播放前重建 i2s_writer（放弃 R036-001 跨曲目复用）。
    // audio_player_stop() 已 deinit 并置 NULL；这里若还 NULL（boot 后第一首 / init失败）
    // 就重建。代价：~100ms 重建开销（i2s_driver_install + DMA buffer），换零状态耦合。
    if (!g_i2s_writer) {
        ESP_LOGI(TAG, "R068: i2s_writer not present, creating now");
        g_i2s_writer = create_i2s_writer();
        if (!g_i2s_writer) {
            ESP_LOGE(TAG, "R068: create_i2s_writer failed");
            audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
            if (g_decoder) audio_pipeline_unregister(g_pipeline, g_decoder);
            audio_element_deinit(g_fatfs_reader);
            if (g_decoder) audio_element_deinit(g_decoder);
            g_fatfs_reader = NULL;
            g_decoder = NULL;
            audio_pipeline_deinit(g_pipeline);
            g_pipeline = NULL;
            return false;
        }
    }
    // R035-015：第三次 audio_pipeline_register 添加返回值检查 + 失败清理
    // R036-001（已弃用，R068 改为每次重建）：i2s_writer 不再"跨曲目复用"——失败清理 deinit + 置 NULL
    if (audio_pipeline_register(g_pipeline, g_i2s_writer, "i2s") != ESP_OK) {
        ESP_LOGE(TAG, "register i2s_writer failed");
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        if (g_decoder) audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_element_deinit(g_fatfs_reader);
        if (g_decoder) audio_element_deinit(g_decoder);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);
        audio_element_deinit(g_i2s_writer);
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        g_i2s_writer = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 5. 链接管道：移到格式嗅探之后（R123-WAV 需在 WAV 解析后创建位深转换 element 并复用
    //    "decoder" 槽位，file→decoder→i2s 三元素链接统一在此处完成）。

    // 6. 设置文件 URI
    audio_element_set_uri(g_fatfs_reader, filepath);

    // R076-CODEC: 严格按 ADF release/v2.x examples/player/pipeline_sdcard_mp3_control 重构
    // - 去掉手动 ID3v2 字节跳过 (R067/R076-EXP)，让 DEFAULT_MP3_DECODER_CONFIG 自己处理
    // - i2s 时钟改为 48000Hz（MAX98357A 支持 8k-96kHz；PV-MP3 在 44.1kHz 路径可能有 bug 触发 BREAK）
    g_id3_skip_bytes = 0;  // R076-CODEC: 保留用于 seek 修正，但不再手动跳过
    int file_rate = AUDIO_SAMPLE_RATE;  // R083: 默认基准 48000，下述 mp3 会嗅探真实速率
    int file_bits = 16;                 // R122-WAV: WAV 直通用真实位深(默认16)；解码格式恒16
    int file_ch   = 2;                  // R122-WAV: WAV 直通用真实声道数(默认2)
    int bitrate_kbps = 0;               // R085: 嗅探到的真实码率，用于准确估算 duration
    /* R117: set g_seek_path for ALL formats (MP3 frame-align, WAV sample-align).
       Previously only MP3 set it, so WAV 4-byte alignment never triggered -> L/R swap noise. */
    {
        const char *rp = strstr(filepath, "://");
        const char *real_path = rp ? rp + 3 : filepath;
        if (real_path[0] == '/' && real_path[1] == '/') real_path++;
        strncpy(g_seek_path, real_path, sizeof(g_seek_path) - 1);
        g_seek_path[sizeof(g_seek_path) - 1] = '\0';
    }
    if (strcasecmp(get_file_ext(filepath), ".mp3") == 0) {
        const char *mp3_path = strstr(filepath, "://");
        const char *real_path = mp3_path ? mp3_path + 3 : filepath;
        if (real_path[0] == '/' && real_path[1] == '/') real_path++;

        int id3_sz = id3v2_total_size(real_path);
        if (id3_sz > 0) {
            g_id3_skip_bytes = id3_sz;
            // R079: 恢复 R067 手动跳过 ID3v2 —— 应用层把 reader 起点设到音频帧，
            // PV-MP3 收到的就是纯音频，不碰 ID3 标签（id3_parse_enable=false）。
            // R076 误以为库能自处理而移除此处，实测含 ID3v2 的 MP3 必崩。
            audio_element_set_byte_pos(g_fatfs_reader, id3_sz);
            ESP_LOGI(TAG, "R079: ID3v2 detected (%d bytes), skipping manually", id3_sz);
        }
        // R083: 嗅探首个音频帧真实采样率，让 I2S 时钟匹配文件（避免被锁 48000 放快）
        int sniff_ch = 2;
        int sniff_layer = 0;
        int sn = mp3_sniff_sample_rate(real_path, g_id3_skip_bytes, &bitrate_kbps, &sniff_ch, &sniff_layer);
        if (sn > 0) {
            file_rate = sn;
            ESP_LOGI(TAG, "R083: sniffed MP3 sample rate = %d Hz, channels = %d", sn, sniff_ch);
            /* R101: Helix 只解 Layer III(层字段==1)。层字段 2=LayerII(MP2)、3=LayerI 的
               文件即使扩展名是 .mp3 也解不出来——解码器会在输入缓冲里永远扫不到合法
               Layer III 帧头，最终误判曲终跳曲(实测 139轻~1.MP3 为 MPEG1 LayerII)。
               建流之前就拒绝：既省掉建/销毁整条 pipeline 的开销，也让上层能立刻沿
               原方向继续跳，而不是被固定的"跳下一首"弹回去（否则按上一首切到坏文件
               会被弹回原处，永远越不过去）。 */
            if (sniff_layer != 0 && sniff_layer != 1) {
                ESP_LOGE(TAG, "R101 unsupported MPEG layer=%d (need 1=LayerIII), reject: %s",
                         sniff_layer, filepath);
                audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
                if (g_decoder) audio_pipeline_unregister(g_pipeline, g_decoder);
                audio_pipeline_unregister(g_pipeline, g_i2s_writer);
                audio_element_deinit(g_fatfs_reader);
                if (g_decoder) audio_element_deinit(g_decoder);
                audio_element_deinit(g_i2s_writer);
                g_fatfs_reader = NULL;
                g_decoder = NULL;
                g_i2s_writer = NULL;
                audio_pipeline_deinit(g_pipeline);
                g_pipeline = NULL;
                return false;
            }
        } else {
            ESP_LOGW(TAG, "R083: sniff MP3 sample rate failed, fallback %d Hz", file_rate);
        }
        /* R100: 单声道文件由 Helix decoder 上混为立体声, I2S 保持 2 声道 */
    }
 else if (strcasecmp(get_file_ext(filepath), ".wav") == 0) {
        const char *wav_path = strstr(filepath, "://");
        const char *real_wav = wav_path ? wav_path + 3 : filepath;
        if (real_wav[0] == '/' && real_wav[1] == '/') real_wav++;
        int wav_ch = 2, wav_bits = 16;
        uint32_t wav_data_off = 0;
        int wav_is_float = 0;
        int wav_rate = wav_parse_header(real_wav, &wav_ch, &wav_bits, &wav_data_off, &wav_is_float);
        if (wav_rate > 0) {
            file_rate = wav_rate;
            file_ch   = wav_ch;
            file_bits = wav_bits;          // R123-WAV: 真实位深(给 conv 用)，I2S 固定 16bit
            g_wav_data_off = wav_data_off;
            ESP_LOGI(TAG, "R116: WAV sample rate = %d Hz, channels = %d, bits = %d, data_off = %u%s",
                     wav_rate, wav_ch, wav_bits, wav_data_off,
                     wav_is_float ? " (IEEE_FLOAT)" : "");
        } else {
            ESP_LOGW(TAG, "R116: parse WAV header failed, fallback %d Hz", file_rate);
        }
        /* R130: 仅 24/32bit 需要位深转换(32bit 可能含 IEEE float); 16bit 直接 file->i2s 直通,
           省去不必要的 wav_conv 任务 —— 既减少任务开销, 也消除 16bit WAV 切歌时 decoder 栈溢出死机。
           (R123-WAV 原对所有 WAV 都挂 wav_conv, 其 task_stack 过小, 切歌 terminate 路径栈溢出。)
           下游 i2s 固定 16bit(已验证路径)，软件音量缩放也正确。 */
        audio_element_set_byte_pos(g_fatfs_reader, g_wav_data_off);  // 跳过 WAV 头部(所有 WAV 都需要)
        if (file_bits != 16) {
            wav_conv_cfg_t conv_cfg = { .src_bits = file_bits, .is_float = (wav_is_float != 0), .sample_rate = file_rate };
            g_decoder = wav_bitdepth_conv_init(&conv_cfg);
            if (!g_decoder) {
                ESP_LOGE(TAG, "R130: wav_bitdepth_conv_init failed");
                audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
                audio_element_deinit(g_fatfs_reader);
                g_fatfs_reader = NULL;
                audio_pipeline_deinit(g_pipeline);
                g_pipeline = NULL;
                return false;
            }
            if (audio_pipeline_register(g_pipeline, g_decoder, "decoder") != ESP_OK) {
                ESP_LOGE(TAG, "R130: register wav_conv failed");
                audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
                audio_element_deinit(g_fatfs_reader);
                audio_element_deinit(g_decoder);
                g_fatfs_reader = NULL;
                g_decoder = NULL;
                audio_pipeline_deinit(g_pipeline);
                g_pipeline = NULL;
                return false;
            }
            ESP_LOGI(TAG, "R130: WAV bit-depth convert (src=%dbit%s), skip header %u bytes",
                     file_bits, wav_is_float ? " IEEE_FLOAT" : "", g_wav_data_off);
        } else {
            g_decoder = NULL;  // 16bit 直通: 不创建 decoder 任务(回归 R117 file->i2s 设计)
            ESP_LOGI(TAG, "R130: WAV 16bit passthrough (file->i2s direct), skip header %u bytes", g_wav_data_off);
        }
    } else if (strcasecmp(get_file_ext(filepath), ".flac") == 0) {
        const char *flac_path = strstr(filepath, "://");
        const char *real_flac = flac_path ? flac_path + 3 : filepath;
        if (real_flac[0] == '/' && real_flac[1] == '/') real_flac++;
        int flac_rate = flac_sniff_sample_rate(real_flac);
        if (flac_rate > 0) {
            file_rate = flac_rate;
            ESP_LOGI(TAG, "R122-FLAC: FLAC sample rate = %d Hz", flac_rate);
        } else {
            ESP_LOGW(TAG, "R122-FLAC: sniff FLAC sample rate failed, fallback %d Hz", file_rate);
        }

        int flac_bits = flac_sniff_bits(real_flac);
        /* R122-FLAC-16BIT (B 止血, 2026-10-10): 只支持 16bit FLAC。esp_codec FLAC 解码器固定输出源位深
           (24bit), 其内部 24bit 路径在 PSRAM 越界写, 踩坏 LVGL(Guru Meditation)。R129 的 conv 降级方案
           已证伪——越界发生在解码器输出 24bit PCM 时(早于 conv), 且 esp_flac_dec_open 不支持配置 16bit
           输出, conv 无从拦截。故非 16bit FLAC 一律拒绝以避免崩溃; 16bit 直通。后续 C 方案(dr_flac)再
           支持 24bit 以 16bit 播放。 */
        if (flac_bits != 16) {
            ESP_LOGW(TAG, "R122-FLAC-16BIT: %d-bit FLAC rejected (esp_codec 24bit output crashes ESP32-S3); only 16bit supported", flac_bits);
            display_toast("FLAC must be\n16bit\ntranscode pls");
            // 拒绝分支清理：audio_pipeline_deinit 内部会自动 terminate+unlink+deinit 所有已
            // registered 元素(file/decoder/i2s)，绝不可先手动 audio_element_deinit 再交给它——
            // 否则二次释放损坏共享堆，导致后续播放拿到坏内存踩坏 LVGL（R134 实测 16bit 续播崩）。
            // 故只调 pipeline deinit，再清句柄。
            audio_pipeline_deinit(g_pipeline);
            g_pipeline = NULL;
            g_fatfs_reader = NULL; g_decoder = NULL; g_i2s_writer = NULL;
            return false;
        }
        file_bits = flac_bits;   // 16bit FLAC 直通; i2s 实际强制 16bit, 变速参考用 g_file_bits
        g_flac_conv = NULL;      // R129 conv 路径已废弃(24bit 改拒绝); 16bit 无需转换
    } else if (strcasecmp(get_file_ext(filepath), ".aac") == 0 || strcasecmp(get_file_ext(filepath), ".m4a") == 0) {
        const char *aac_path = strstr(filepath, "://");
        const char *real_aac = aac_path ? aac_path + 3 : filepath;
        if (real_aac[0] == '/' && real_aac[1] == '/') real_aac++;
        int aac_rate = aac_sniff_sample_rate(real_aac);
        if (aac_rate > 0) {
            file_rate = aac_rate;
            ESP_LOGI(TAG, "R122-AAC: AAC sample rate = %d Hz", aac_rate);
        } else {
            ESP_LOGW(TAG, "R122-AAC: sniff AAC sample rate failed, fallback %d Hz", file_rate);
        }
    }

    // 5b. 链接管道: file→decoder→i2s；FLAC 24bit 时插位深转换 element(file→decoder→conv→i2s)。
    //     WAV 的 "decoder" 槽位复用为位深转换 element，R123-WAV 必须在创建后再链接。
    {
        const char *link_tags[4];
        int link_count;
        if (g_flac_conv) {   // R122-FLAC-16BIT: 解码器(flac_dec)→位深转换(conv)→i2s
            link_tags[0] = "file"; link_tags[1] = "decoder"; link_tags[2] = "conv"; link_tags[3] = "i2s"; link_count = 4;
        } else if (g_decoder) {
            link_tags[0] = "file"; link_tags[1] = "decoder"; link_tags[2] = "i2s"; link_count = 3;
        } else {
            link_tags[0] = "file"; link_tags[1] = "i2s"; link_count = 2;   /* R130: 16bit WAV 直通 */
        }
        esp_err_t link_err = audio_pipeline_link(g_pipeline, link_tags, link_count);
        if (link_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to link pipeline (0x%x)", link_err);
            // 同拒绝分支：audio_pipeline_deinit 自动清理所有已 register 元素，避免手动 deinit 二次释放
            audio_pipeline_deinit(g_pipeline);
            g_pipeline = NULL;
            g_fatfs_reader = NULL; g_decoder = NULL; g_flac_conv = NULL; g_i2s_writer = NULL;
            return false;
        }
    }

    // 7. 设置 I2S 时钟 — R083: 改用文件真实速率(mp3 嗅探)替代固定的 AUDIO_SAMPLE_RATE(48000)，
    //    否则低采样率文件(如 24000Hz 的躲避的爱)会被 48000 时钟放快变尖。
    //    R100: 单声道文件由 Helix decoder 上混为立体声输出，I2S 保持 2 声道。
    g_base_sample_rate   = file_rate;
    g_current_sample_rate = file_rate;
    g_file_bits = file_bits;   // R122-WAV: 变速时据此保持真实位深/声道
    g_file_ch   = file_ch;
    // R123-WAV: WAV 直通一律走 16bit I2S(由位深转换 element 把 24/32bit 软件转 16bit)，
    // 彻底规避 ESP32-S3 直配 24/32bit I2S 在 96k/32bit 下 i2s_set_clk 卡死。普通解码格式
    // 仍按真实位深(解码器自行上报 24bit)配置 I2S。
    int i2s_out_bits = (is_wav || is_flac) ? 16 : file_bits;
    esp_err_t clk_err = i2s_stream_set_clk(g_i2s_writer, file_rate, i2s_out_bits, file_ch);
    ESP_LOGI(TAG, "R130: i2s set_clk rate=%d bits=%d ch=%d -> %s",
             file_rate, i2s_out_bits, file_ch, (clk_err == ESP_OK) ? "OK" : "FAIL");

    // 8. 启动管道（R032-209: 检查返回值，失败即终止，避免进入播放态却无声）
    if (audio_pipeline_run(g_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed");
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        if (g_decoder) audio_pipeline_unregister(g_pipeline, g_decoder);
        if (g_flac_conv) audio_pipeline_unregister(g_pipeline, g_flac_conv);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);
        audio_element_deinit(g_fatfs_reader);
        if (g_decoder) audio_element_deinit(g_decoder);
        if (g_flac_conv) audio_element_deinit(g_flac_conv);
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        g_flac_conv = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    g_is_playing = true;
    g_is_paused = false;

    g_play_start_us = esp_timer_get_time();
    g_play_offset_us = 0;
    g_last_scrub_us = 0;  // M1: 跨曲目重置跳帧时间戳

    // 9. 计算文件字节数（用于 seek/位置换算）
    // R067：去掉 ID3v2 头部，让后续 seek/duration 估算按"音频数据"算，
    // 避免把 ID3 标签字节错算成音频时长。
    struct stat st;
    if (stat(filepath, &st) == 0) {
        uint32_t total = (uint32_t)st.st_size;
        uint32_t skip = (uint32_t)g_id3_skip_bytes + (is_wav ? g_wav_data_off : 0);  // R123-WAV: 也扣掉 WAV 头部
        g_total_file_bytes = (total > skip) ? total - skip : 0;
    } else {
        g_total_file_bytes = 0;
    }

    // M3：从文件大小按格式字节率估计 duration
    // 估算字节率表（bytes/ms，仅用于进度条显示）：
    //   MP3/AAC/OGG ≈ 128kbps → 16
    //   Opus ≈ 96kbps → 12
    //   FLAC ≈ 512kbps → 64（损失编码前样本率）
    //   WAV 44.1k/16bit/stereo 1411.2kbps → 176
    // 注意：实际编码比特率与文件有关，进度条仅作粗略展示，不用于精确 seek。
    g_total_duration_ms = 0;
    if (g_total_file_bytes > 0) {
        // R085: 优先用嗅探到的真实 MP3 码率算时长（duration_ms = 字节数*8/码率_kbps），
        // 修复固定 128kbps 估算导致进度条到不了一端的问题；其余格式仍回退字节率估算。
        if (bitrate_kbps > 0) {
            g_total_duration_ms = (int)((int64_t)g_total_file_bytes * 8 / bitrate_kbps);
            ESP_LOGD(TAG, "Duration from real MP3 bitrate: %d ms (%dkbps)", g_total_duration_ms, bitrate_kbps);
        } else {
            const char *ext = get_file_ext(filepath);
            int bytes_per_ms;
            if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".aac") == 0 ||
                strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".ogg") == 0) {
                bytes_per_ms = 16;
            } else if (strcasecmp(ext, ".opus") == 0) {
                bytes_per_ms = 12;
            } else if (strcasecmp(ext, ".flac") == 0) {
                int flac_ms = flac_parse_duration_ms(filepath);
                if (flac_ms > 0 && g_total_file_bytes > (uint32_t)flac_ms) {
                    bytes_per_ms = g_total_file_bytes / flac_ms;   // 反推精确字节率，消除虚长
                    ESP_LOGI(TAG, "Duration from FLAC STREAMINFO: %d ms (was ~%d ms by size/64)",
                             flac_ms, g_total_file_bytes / 64);
                } else {
                    bytes_per_ms = 64;   // STREAMINFO 解析失败时的粗估兜底
                }
            } else if (strcasecmp(ext, ".wav") == 0) {
                // R122-WAV: 用嗅探到的真实 rate/bits/ch 算精确字节率（替代固定 176，
                // 否则 24bit WAV 进度条虚长、曲终被误判"切割"，与 FLAC 同因）
                int wav_bpm = (file_rate * (file_bits / 8) * file_ch) / 1000;
                bytes_per_ms = (wav_bpm > 0) ? wav_bpm : 176;
            } else {
                bytes_per_ms = 16;  // fallback 同 MP3
            }
            g_total_duration_ms = g_total_file_bytes / bytes_per_ms;
            ESP_LOGD(TAG, "Duration estimated from file size: %d ms (bytes/ms=%d, ext=%s)",
                     g_total_duration_ms, bytes_per_ms, ext);
            ESP_LOGW(TAG, "Estimated duration is approximate; progress bar/seek may be imprecise");
        }
    }

    // 11. 应用当前音量
    audio_player_set_volume(g_volume);

    // R074-fix: 切歌后强制 player 重新渲染。某些文件切歌后 LVGL dirty tracking
    // 失效（player 对象未标 dirty），屏幕卡在切前状态或黑屏。强制 tick 一次
    // 让 lvgl_task 在持锁回调中调 ui_show_player 重新绘制。
    // 与 R073 修复（boot 1.5s 后强制 tick）机制对称——保证 play 路径也有强制刷新。
    display_request_main_tick();

    return true;
}

void audio_player_pause(void)
{
    if (g_is_playing && !g_is_paused && g_pipeline) {
        audio_pipeline_pause(g_pipeline);
        g_play_offset_us += (int64_t)(esp_timer_get_time() - g_play_start_us);
        g_is_paused = true;
        ESP_LOGI(TAG, "Paused");
    }
}

void audio_player_resume(void)
{
    if (g_is_playing && g_is_paused && g_pipeline) {
        g_play_start_us = esp_timer_get_time();
        // R086: 同 seek 路径，普通 resume 也会因 ADF pause→resume 不清 ringbuffer done 标志而
        // 残留 done 态：resume 后 decoder 一读即 AEL_IO_DONE 误判曲终跳下一首。恢复前重置 decoder
        // 的 input(reader→decoder)/output(decoder→i2s) ringbuffer。
        if (g_decoder) {
            audio_element_reset_input_ringbuf(g_decoder);
            audio_element_reset_output_ringbuf(g_decoder);
        }
        audio_pipeline_resume(g_pipeline);
        g_is_paused = false;
        ESP_LOGI(TAG, "Resumed");
    }
}

#define AUDIO_STOP_TIMEOUT_MS   200   /* I2S writer 等待终态超时 */
#define AUDIO_STOP_POLL_MS      10    /* 超时轮询间隔 */

void audio_player_stop(void)
{
#if defined(CONFIG_USE_BT_SPEAKER)
    // BT 音箱模式复用 g_i2s_writer，必须先停 BT 管线再继续
    if (g_bt_active) audio_player_stop_bt();
#endif

    // R032-211：pipeline/writer 未就绪（OOM 或初始化失败）时直接重置状态返回，
    // 避免访问已释放/未创建的音频元素。
    if (!g_pipeline || !g_i2s_writer) {
        // R035-016：早返回分支统一清零全部时间相关状态变量，避免后续 play() 残留旧值
        g_is_playing = false;
        g_is_paused = false;
        g_play_start_us = 0;
        g_play_offset_us = 0;
        g_total_duration_ms = 0;
        g_id3_skip_bytes = 0;  // R067
        g_last_scrub_us = 0;
        return;
    }

    if (g_pipeline) {
        /* R101: 先清播放标志。audio_player_tick() 以 g_is_playing 判定曲终并回调
           on_track_finished 自动切下一首；下面 abort ringbuf 会让 element 立即脱离
           阻塞、状态可能跳变，若不提前清标志，tick 会在 stop() 期间误判曲终触发
           自动切歌，与本次 stop()（用户停止/切歌）语义冲突。 */
        g_is_playing = false;
        // R075-fix：不再手动 deinit 各 element！
        // 旧逻辑先 audio_element_deinit(fatfs/decoder/i2s) 释放内存，再调
        // audio_pipeline_deinit()，而后者内部会遍历 el_list 对【同一个已释放的
        // element】再 deinit 一次 → double-free → 堆损坏 → 表现为切歌后下一首
        // play() 在 Pipeline started 后崩溃（0x403743c0 BREAK / DoubleException）。
        // 崩溃与具体歌曲相关，是因为不同解码路径踩中损坏堆元数据的概率不同。
        //
        // 修复：只调一次 audio_pipeline_deinit()，它内部已统一 terminate + deinit
        // el_list 中所有 element（含 i2s_writer 的 destroy→i2s_driver_uninstall），
        // 完全满足 R068 "i2s_writer 每次重建" 的意图，且杜绝 double-free。
        //
        // R098g：先 audio_pipeline_stop() 会让 main_task 永久阻塞——当 i2s element 因
        //   事件队列满(AUDIO_EVT: no space in external queue)卡死、不响应 STOP 命令时，
        //   audio_pipeline_stop 内部用 portMAX_DELAY 等待 element 进入 stopped，永远等不到
        //   → main_task 假死(task_wdt 触发)、音频也停不下来(用户感知"停止不了")。
        //   改用 audio_pipeline_terminate_with_ticks()：它内部同样先发 STOP 并等待 element
        //   优雅退出(关闭 fatfs fd)，但带 ticks 超时(3s)一定返回；仅真卡死才超时强删
        //   (偶发 fd 泄漏，远比永久假死可接受)。这样 main_task 最多阻塞 3s 即恢复。
        /* R101: 终止前先"掐断"可能阻塞 element task 的 ringbuf，让 task 能响应 STOP
           并走完 close 回调【正常 fclose 关闭 fatfs 文件描述符】。
           背景：R098g 为规避 main_task 永久假死，改为直接 terminate_with_ticks()，
           其注释已注明"偶发 fd 泄漏"。实测该"偶发"实为必然——下游 decoder 停止消费时
           上游 file task 永久阻塞在写 output ringbuf，收不到 QUIT 命令，terminate
           超时(日志 Element task destroy timeout[300]/[200])后强删任务，close 回调
           得不到执行 → fd 泄漏。累积数首后 vfs_fat 报 "no free file descriptors"，
           表现为连播/切歌若干次后快进快退 fopen 失败、播放无声（R076-CODEC-9 原本
           就是为防此问题而先 stop 再 terminate，被 R098g 删掉）。 */
        if (g_fatfs_reader) {
            audio_element_abort_output_ringbuf(g_fatfs_reader);  /* 解除 file 的写阻塞 */
        }
        if (g_decoder) {
            audio_element_abort_input_ringbuf(g_decoder);        /* 解除 decoder 的读阻塞 */
            audio_element_abort_output_ringbuf(g_decoder);       /* 解除 decoder 的写阻塞 */
        }
        if (g_flac_conv) {                                       /* R122-FLAC-16BIT: 解除 conv 阻塞 */
            audio_element_abort_input_ringbuf(g_flac_conv);
            audio_element_abort_output_ringbuf(g_flac_conv);
        }
        /* 给 element task 时间从阻塞中返回并执行 close 关闭 fd */
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "R101: terminating pipeline (fds closed gracefully, 3s timeout)...");
        audio_pipeline_terminate_with_ticks(g_pipeline, pdMS_TO_TICKS(3000));

        // 销毁管道 + 内部所有 element（仅此一次 deinit）
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
    }
    // element 内存已由 audio_pipeline_deinit 释放，此处仅清句柄，避免悬空指针
    g_fatfs_reader = NULL;
    g_decoder = NULL;
    g_flac_conv = NULL;    // R122-FLAC-16BIT: conv 元素已由 audio_pipeline_deinit 销毁, 仅清句柄
    g_i2s_writer = NULL;   // R068：每次 play() 重建

    g_is_playing = false;
    g_is_paused = false;
    g_is_wav = false;        // R123-WAV: 重置当前格式标志
    g_is_flac = false;       // R122-FLAC-16BIT: 重置当前格式标志
    g_wav_data_off = 0;      // R123-WAV: 重置 WAV 头部偏移

    g_total_duration_ms = 0;
    g_id3_skip_bytes = 0;  // R067：切歌时重置，新曲目重新探测
    g_play_offset_us = 0;
    g_last_scrub_us = 0;  // M1: 停止时重置跳帧时间戳
}

/* ============================================================
 * Seek（毫秒级内部实现）
 * ============================================================ */
void audio_player_seek(int seconds)
{
    audio_player_seek_ms(seconds * 1000);
}

// R095: 将 seek 落点扫描到下一个【合法 MP3 帧头】。
// 旧逻辑仅用 (buf[i+1]&0xE0)==0xE0 判同步字，会把 0xFF 0xFF / 0xFF 0xFE 等
// 伪同步字当帧边界返回；Helix 从该垃圾位置连续解坏帧(err_cnt>50)→误判曲终
// 跳下一首（实测切歌前 seek-data 首字节为 ffff fecf）。此处改校验完整帧头
// （版本/层/位率/采样率索引均合法），避免伪同步字误对齐。
static bool mp3_valid_hdr_strict(const uint8_t *p, int *frame_len_out)
{
    /* R097: 严格校验，仅接受 MPEG1/2/2.5 的 Layer III(01) 且非 free-format。
       旧 R095 版放行 0xFF 0xFE(实为 Layer I / free-format) 等伪同步字当帧边界，
       致 Helix 从垃圾位置解析出异常 nSlots -> mp3dec.c:380 超大 memcpy 越界读 Flash
       -> "Cache disabled" 崩溃。 */
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int version = (p[1] >> 3) & 0x03;
    int layer   = (p[1] >> 1) & 0x03;
    if (version == 0x01) return false;                       // 01 = reserved
    if (layer   != 0x01) return false;                       // 仅 Layer III(01)；I/II(10/11) 非 MP3
    int bitrate_idx = (p[2] >> 4) & 0x0F;
    if (bitrate_idx == 0x00 || bitrate_idx == 0x0F) return false;  // 0=free format,15=bad
    int sr_idx = (p[2] >> 2) & 0x03;
    if (sr_idx == 0x03) return false;                        // 11 = reserved
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
    if (bitrate == 0 || samplerate == 0) return false;

    int flen = ((version == 3) ? 144 : 72) * bitrate / samplerate + padding;
    if (flen <= 0) return false;
    if (frame_len_out) *frame_len_out = flen;
    return true;
}

static int mp3_frame_align(const char *path, int byte_pos)
{
    if (!path || !*path) return byte_pos;
    FILE *fp = fopen(path, "rb");
    if (!fp) return byte_pos;
    if (fseek(fp, byte_pos, SEEK_SET) != 0) { fclose(fp); return byte_pos; }
    // R095: 用 static 缓冲，避免 8KB 局部变量压垮 main 任务栈（R083/R084 曾因
    // main 栈上 8KB 缓冲溢出崩）。mp3_frame_align 仅由 main 任务串行调用，static 安全。
    static uint8_t buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    for (size_t i = 0; i + 3 < n; i++) {
        int flen = 0;
        if (mp3_valid_hdr_strict(&buf[i], &flen)) {
            // 用"连续两帧"确认真实帧边界，拒绝单字节伪同步
            if (i + flen + 4 <= n) {
                int flen2 = 0;
                if (mp3_valid_hdr_strict(&buf[i + flen], &flen2)) {
                    ESP_LOGW("mp3_dbg", "R097 frame_align +%d (byte_pos=%d -> %d, flen=%d)",
                             (int)i, byte_pos, byte_pos + (int)i, flen);
                    return byte_pos + (int)i;
                }
            } else {
                ESP_LOGW("mp3_dbg", "R097 frame_align +%d (byte_pos=%d -> %d, last-frame)",
                         (int)i, byte_pos, byte_pos + (int)i);
                return byte_pos + (int)i;
            }
        }
    }
    // 窗口内无合法帧头：不强行对齐到伪同步字，返回原落点交 decoder 自行重同步
    return byte_pos;
}

static void audio_player_seek_ms_internal(int ms)
{
    if (!g_pipeline || !g_is_playing || !g_fatfs_reader) return;

    // R100: 钳制 seek 目标在曲目时长内；超时长(损坏/越界恢复点)回退曲首 0，
    // 防止 seek 到文件尾之外 -> FATFS 立即 AEL_IO_DONE -> "播放即结束/不自动播下一首"。
    if (g_total_duration_ms > 0 && ms > g_total_duration_ms) {
        ms = 0;
    }

    // R067：g_total_file_bytes 已经是音频字节数（去掉 ID3v2）；
    // seek 目标 = id3_skip + ms * audio_bytes / duration。
    int64_t byte_pos = 0;
    if (g_total_duration_ms > 0 && g_total_file_bytes > 0) {
        byte_pos = g_id3_skip_bytes + (int64_t)ms * g_total_file_bytes / g_total_duration_ms;
    } else if (g_total_file_bytes > 0) {
        byte_pos = g_id3_skip_bytes + (int64_t)ms * g_total_file_bytes / 3600000;
    }
    // R032-002 复审修订：ADF audio_element_set_byte_pos 入参为 int（32-bit），钳位避免窄化截断。
    if (byte_pos > INT32_MAX) byte_pos = INT32_MAX;
    if (byte_pos < 0) byte_pos = 0;
    int64_t max_byte = g_id3_skip_bytes + g_total_file_bytes;
    if (byte_pos > max_byte) byte_pos = max_byte;   /* R100: 兜底，不越过音频文件尾 */
    // R085: MP3 帧对齐，减少解码器坏帧（叠加 err_cnt 误判曲终）
    if (strcasecmp(get_file_ext(g_seek_path), ".mp3") == 0) {
        byte_pos = mp3_frame_align(g_seek_path, (int)byte_pos);
    }
    // R117: WAV PCM sample alignment — 16-bit stereo = 4 bytes/sample.
    // Seeking to non-4-aligned offset causes L/R channel swap = white noise.
    if (strcasecmp(get_file_ext(g_seek_path), ".wav") == 0) {
        byte_pos &= ~(int64_t)3;  /* floor to 4-byte sample boundary */
    }
    audio_element_set_byte_pos(g_fatfs_reader, (int)byte_pos);

    // R085: 不再对 decoder 做 set_byte_pos。decoder 的底层输入是 ringbuffer（不可 seek），
    // 在其上 set_byte_pos(0) 会导致 resume 时解码器对自身输入执行 seek 失败并立即返回
    // AEL_IO_DONE —— 表现为“短按 FF/REW 直接播到曲尾跳下一首”。MP3 解码管线只需对源
    // 元素（fatfs_reader）seek，decoder 会从 ringbuffer 新数据自动重新同步帧边界。
    // （原 C1 逻辑已废弃）

    g_play_start_us = esp_timer_get_time();
    g_play_offset_us = (int64_t)ms * 1000;
}

// R085: ADF 的 pause/resume 会重开元素(fatfs_reader/decoder 重新 open)，但不清空 ringbuffer 的
// done 标志(audio_element_on_cmd_resume 仅在 state 非 PAUSED 时 reset，pause 后 state==PAUSED
// 被跳过)。若不重置，resume 后 decoder 首次 audio_element_input 即拿到 AEL_IO_DONE → 误判曲终跳
// 下一首。因此在 resume 前显式重置 decoder 的 input(reader→decoder)/output(decoder→i2s) ringbuffer。

/* R116: only MP3(Helix) needs internal reset; WAV/AAC decoders lack this function */
static void safe_mp3_decoder_reset(void)
{
    if (g_seek_path[0] && strcasecmp(get_file_ext(g_seek_path), ".mp3") == 0) {
        mp3_decoder_libhelix_reset(g_decoder);
    }
}

static void audio_player_pause_seek_resume(int ms)
{
    if (!g_pipeline || !g_is_playing) return;
    audio_pipeline_pause(g_pipeline);
    audio_player_seek_ms_internal(ms);
    if (g_decoder) {
        audio_element_reset_input_ringbuf(g_decoder);
        audio_element_reset_output_ringbuf(g_decoder);
    }
    safe_mp3_decoder_reset();
    audio_pipeline_resume(g_pipeline);
}

void audio_player_seek_ms(int ms)
{
    if (!g_pipeline || !g_is_playing) return;
    // S5：保留原暂停态——暂停时 seek 不再静默 resume
    bool was_paused = g_is_paused;
    if (!was_paused) {
            /* R117: pause reader + i2s. WAV has no decoder (PCM passthrough).
               Keep input rb so compressed-format decoders never see empty. */
            audio_element_pause(g_fatfs_reader);
            if (g_i2s_writer) audio_element_pause(g_i2s_writer);
            audio_player_seek_ms_internal(ms);
            audio_element_resume(g_fatfs_reader, 0, pdMS_TO_TICKS(100));
            if (g_decoder) {
                audio_element_reset_output_ringbuf(g_decoder);
            }
            if (g_i2s_writer) audio_element_resume(g_i2s_writer, 0, pdMS_TO_TICKS(100));
    } else {
        audio_player_seek_ms_internal(ms);
        if (g_decoder) {
            audio_element_reset_input_ringbuf(g_decoder);
            audio_element_reset_output_ringbuf(g_decoder);
            safe_mp3_decoder_reset();   // R094: 同 pause_seek_resume，避免残留缓冲误判曲终
        }
        // R035-020：保持 paused：清掉内部函数的 start_us 赋值，避免 get_position_ms 在暂停态累积。
        g_play_start_us = 0;
    }
}

void audio_player_scrub_seek(int ms)
{
    if (!g_pipeline || !g_is_playing || !g_fatfs_reader) return;
    if (g_decoder) {
        /* Compressed formats: direct seek, decoder handles frame boundaries.
           R122-AAC/FLAC-CODEC: 仅 MP3(Helix) 有 helix_ctx_t->err_cnt 需清;
           AAC/FLAC 已改 esp_codec 解码器, 其 data 首字段是 dec_handle 指针,
           误调 clear_errors 会破坏句柄 -> 解码头崩溃。故仅 MP3 调用。 */
        audio_player_seek_ms_internal(ms);
        if (g_seek_path[0] && strcasecmp(get_file_ext(g_seek_path), ".mp3") == 0) {
            mp3_decoder_libhelix_clear_errors(g_decoder);
        }
    } else {
        /* R117 WAV PCM passthrough: reader is already paused in scrub_enter.
           Only update position variables for display — actual seek happens once
           in scrub_exit. Avoids any pipeline operation during scrub ticks. */
        g_play_start_us = esp_timer_get_time();
        g_play_offset_us = (int64_t)ms * 1000;
    }
}

static void apply_volume_alc(int volume);

void audio_player_scrub_enter(void)
{
    if (!g_pipeline) return;
    g_volume_saved = g_volume;
    apply_volume_alc(0);
    g_scrub_active = true;                    /* R099: 快进/快退期进度不计播放流逝，仅由 seek 推进 */
    g_last_scrub_us = esp_timer_get_time();   /* 重置 tick 计时 */
    /* R117 WAV PCM passthrough: pause reader during scrub. i2s keeps running and plays
       silence when rb is empty. This avoids frequent reader/i2s pause/resume during
       scrub ticks which caused discontinuous PCM = white-noise artifacts. */
    if (!g_decoder && g_fatfs_reader) {
        audio_element_pause(g_fatfs_reader);
    }
}

void audio_player_scrub_exit(bool resume)
{
    if (!g_pipeline) return;
    int final_ms = audio_player_get_position_ms();   /* 释放时显示位置(最后 seek 目标) */
    g_scrub_active = false;
    /* R111: 暂停态退出scrub不resume, 避免RESUME->PAUSE抖动导致管道状态混乱 */
    if (resume) {
        if (g_decoder) {
            /* Compressed formats: pause reader+i2s, seek, clear decoder rb, resume */
            audio_element_pause(g_fatfs_reader);
            if (g_i2s_writer) audio_element_pause(g_i2s_writer);
            audio_player_seek_ms_internal(final_ms);
            audio_element_reset_output_ringbuf(g_decoder);
            audio_element_resume(g_fatfs_reader, 0, pdMS_TO_TICKS(100));
            if (g_i2s_writer) audio_element_resume(g_i2s_writer, 0, pdMS_TO_TICKS(100));
        } else {
            /* R117 WAV PCM passthrough: i2s NEVER paused. Reader was paused in scrub_enter.
               KEY: set_byte_pos only changes file offset — reader's internal prefetch buffer
               still holds OLD-position data. Must resume reader first to flush that stale data,
               THEN clear rb, wait for fresh data, THEN unmute. */
            audio_player_seek_ms_internal(final_ms);
            audio_element_resume(g_fatfs_reader, 0, pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(80));  /* let reader flush internal prefetch (stale) */
            audio_element_reset_output_ringbuf(g_fatfs_reader);  /* discard stale data */
            vTaskDelay(pdMS_TO_TICKS(50));  /* let reader fill rb with fresh new-position data */
        }
        apply_volume_alc(g_volume_saved);
    } else {
        audio_pipeline_pause(g_pipeline);
        audio_player_seek_ms_internal(final_ms);
        if (g_decoder) {
            audio_element_reset_input_ringbuf(g_decoder);
            audio_element_reset_output_ringbuf(g_decoder);
            safe_mp3_decoder_reset();
        }
        g_play_start_us = 0;  /* 保持暂停态, 不累积播放时间 */
        apply_volume_alc(g_volume_saved);
    }
}

int audio_player_get_position_ms(void)
{
    if (!g_pipeline) return 0;

    // 累计播放时间 = 暂停前已累计 + 当前段播放时间（暂停期间不增加）
    int64_t total = g_play_offset_us;
    if (g_is_playing && !g_is_paused && !g_scrub_active && g_play_start_us > 0) {
        total += (int64_t)(esp_timer_get_time() - g_play_start_us);
    }
    return (int)(total / 1000);
}

int audio_player_get_position(void)
{
    return audio_player_get_position_ms() / 1000;
}

int audio_player_get_duration(void)
{
    return g_total_duration_ms / 1000;
}

bool audio_player_is_playing(void)
{
    return g_is_playing && !g_is_paused;
}

bool audio_player_is_paused(void)
{
    return g_is_paused;
}

void audio_player_set_speed(float speed)
{
    if (!g_i2s_writer) return;

    (void)speed;
    /* R098: seek-based FF/REW; keep I2S at file sample rate to avoid i2s-writer
       overriding the variable clock (speed-up lost) and buffer starvation. */
    int sample_rate = g_base_sample_rate;

    if (sample_rate != g_current_sample_rate) {
        g_current_sample_rate = sample_rate;
        // R123-WAV: WAV 直通 I2S 固定 16bit(位深由转换 element 处理)，变速时不能退回 32bit I2S
        int bits = (g_is_wav || g_is_flac) ? 16 : g_file_bits;
        i2s_stream_set_clk(g_i2s_writer, sample_rate, bits, g_file_ch);
    }
}

/* 内部：仅设置 g_volume 并应用 decoder 软件音量（不触达 BT 回传，避免音量循环）。
   R091: 弃用 i2s ALC（IDF5.x 下 alc_volume_setup_process BREAK 崩溃），改由 decoder 缩放 PCM。 */
static void apply_volume_alc(int volume)
{
    g_volume = volume;
    /* R117: MP3 uses decoder internal volume; non-MP3 uses i2s software volume scale.
       Both map level 0..14 to linear gain 0..100%, so perceived loudness is consistent. */
    bool is_mp3 = false;
    if (g_seek_path[0]) {
        is_mp3 = (strcasecmp(get_file_ext(g_seek_path), ".mp3") == 0);
    }
    if (is_mp3) {
        mp3_decoder_set_volume(volume);
        if (g_i2s_writer) i2s_stream_set_sw_volume(g_i2s_writer, 100);
    } else {
        mp3_decoder_set_volume(VOLUME_LEVEL_MAX);  /* decoder at max, avoid double scaling */
        if (g_i2s_writer) {
            int percent = volume * 100 / VOLUME_LEVEL_MAX;
            i2s_stream_set_sw_volume(g_i2s_writer, percent);
        }
    }
}

void audio_player_set_volume(int volume)
{
    // V1.2 音量模型：15 档逻辑音量 (level 0..VOLUME_LEVEL_MAX)，线性 dB 映射 -96..+12 dB
    // 覆盖 MAX98357A ALC 全动态范围 (i2s_alc_volume_set 范围 -96..+12 dB)。
    // dB(level) = -96 + level * (12 - (-96)) / 14，四舍五入。
    //   level 0  → -96 dB（静音）
    //   level 14 → +12 dB（最大增益，约每档 7.7 dB，等感知步进）
    if (volume < 0) volume = 0;
    if (volume > VOLUME_LEVEL_MAX) volume = VOLUME_LEVEL_MAX;
    apply_volume_alc(volume);

#if defined(CONFIG_USE_BT_SPEAKER)
    // BT 模式下把本地音量回传手机（AVRCP 绝对音量 0..127），使两端一致
    if (g_bt_active) {
        bt_speaker_report_volume((uint8_t)((uint32_t)volume * 127 / VOLUME_LEVEL_MAX));
    }
#endif
}

int audio_player_get_volume(void)
{
    return g_volume;
}

/* ============================================================
 * Tick — 处理管道状态 + 快进/快退跳帧
 *
 * 跳帧策略（档位 1.5/2.0/3.0 I2S 变速 + 8.0x 跳帧模式）：
 * - 1.5x / 2.0x / 3.0x：仅变速（I2S 采样率），不跳帧
 * - 8.0x（跳帧模式）：正常 I2S + 每 50ms seek 跳帧（跳 7/8 音频）
 * - 快退：所有档位都通过向后 seek 模拟
 * ============================================================ */
void audio_player_tick(void)
{
#if defined(CONFIG_USE_BT_SPEAKER)
    // BT 模式无 SD pipeline，跳过走带/跳帧/A-B 复读逻辑
    if (g_bt_active) return;
#endif
    if (!g_pipeline || !g_is_playing) return;

    // 检查管道状态（通过 I2S writer 元素状态判断）
    audio_element_state_t el_state = audio_element_get_state(g_i2s_writer);
    // R100: 文件打开/解码失败(如文件名编码 FATS 打不开)会进 AEL_STATE_ERROR，
    // 若不加处理会一直停在"播放中但无声"。此处等同曲终 -> 回调 on_track_finished -> 自动跳下一首。
    audio_element_state_t file_state = g_fatfs_reader ? audio_element_get_state(g_fatfs_reader) : AEL_STATE_NONE;
    audio_element_state_t dec_state  = g_decoder  ? audio_element_get_state(g_decoder)  : AEL_STATE_NONE;
    if (el_state == AEL_STATE_FINISHED || el_state == AEL_STATE_STOPPED ||
        file_state == AEL_STATE_ERROR || dec_state == AEL_STATE_ERROR) {
        ESP_LOGI(TAG, "Track finished (state el=%d file=%d dec=%d)", el_state, file_state, dec_state);
        g_is_playing = false;
        if (g_status_cb) {
            g_status_cb(0, g_user_data); // 0 = finished
        }
        return; // R034-003 / R035-003：终止本帧，避免后续 FF/RW 跳帧代码在 FINISHED pipeline 上 pause/resume
    }

    // C2: duration fallback — 从文件大小估计（ADF 无 direct duration API）
    if (g_total_duration_ms <= 0 && g_total_file_bytes > 0) {
        // 128kbps 估计：文件字节 / 16 ≈ 毫秒
        g_total_duration_ms = g_total_file_bytes / 16;
        ESP_LOGD(TAG, "Duration estimated from file size: %d ms", g_total_duration_ms);
    }

    // R049b：A-B 区间复读循环（仅在播放中、已开且 A<B 时生效）
    if (g_ab_enabled && g_ab_a_ms >= 0 && g_ab_b_ms > g_ab_a_ms) {
        int cur = audio_player_get_position_ms();
        if (cur >= g_ab_b_ms) {
            g_ab_loop_count++;   /* R111: 遍数 +1 */
            ESP_LOGD(TAG, "AB loop #%d: seek back to A (%d ms)", g_ab_loop_count, g_ab_a_ms);
            /* R117: pause reader + i2s (NOT decoder for compressed formats; WAV has none).
               Keep input rb so decoder never sees empty. Reader resumes from new position. */
            audio_element_pause(g_fatfs_reader);
            if (g_i2s_writer) audio_element_pause(g_i2s_writer);
            audio_player_seek_ms_internal(g_ab_a_ms);
            audio_element_resume(g_fatfs_reader, 0, pdMS_TO_TICKS(100));
            if (g_decoder) {
                audio_element_reset_output_ringbuf(g_decoder);
            }
            if (g_i2s_writer) audio_element_resume(g_i2s_writer, 0, pdMS_TO_TICKS(100));
        }
    }

    // 快进/快退跳帧处理
    tape_mode_t mode = tape_control_get_mode();
    if (mode == TAPE_MODE_NORMAL) return;

    float speed = tape_control_get_speed();
    float abs_speed = (speed > 0) ? speed : -speed;

    // 仅高档位（≥最高档位速度）执行跳帧；1.5x/2.0x/3.0x 仅靠 I2S 变速
    // 快退所有档位都跳帧（因为没有"倒放"能力，只能断续 seek）
    // R034-011：阈值由硬编码 4.0f 改为派生 tape_control_get_max_gear_speed()，
    // 避免修改 g_speed_steps[] 后此处 magic number 漂移
    // R098: FF/REW 全部改为跳帧式(持续 seek)，任一档位都 seek，I2S 保持正常速率
    bool need_seek = (mode == TAPE_MODE_FAST_FORWARD) || (mode == TAPE_MODE_REWIND);

    if (!need_seek) return;

uint64_t now = esp_timer_get_time();
    uint64_t elapsed_us = now - g_last_scrub_us;
    if (elapsed_us < 500000) return; // 500ms min interval
    g_last_scrub_us = now;

    int elapsed_ms = (int)(elapsed_us / 1000);
    if (elapsed_ms > 1000) elapsed_ms = 1000;   // cap first-tick anomaly

    // R099: g_scrub_active stops playback-time counting (get_position_ms = last seek target),
    // so each tick advances exactly speed*elapsed. Total = speed*hold-time, precise & linear.
    int skip_ms = (speed > 0) ? (int)(abs_speed * elapsed_ms)
                              : -(int)(abs_speed * elapsed_ms);

    int cur_ms = audio_player_get_position_ms();
    int target_ms = cur_ms + skip_ms;
    if (target_ms < 0) target_ms = 0;
    int duration_ms = g_total_duration_ms > 0 ? g_total_duration_ms : 3600000;
    if (target_ms > duration_ms) target_ms = duration_ms;

    audio_player_scrub_seek(target_ms);
}

void audio_player_set_callback(audio_status_cb_t cb, void *user_data)
{
    g_status_cb = cb;
    g_user_data = user_data;
}

/* ============================================================
 * R049b：A-B 区间复读
 * ============================================================ */
void audio_player_mark_a(void)
{
    if (!g_is_playing) {
        ESP_LOGW(TAG, "AB mark A: not playing");
        return;
    }
    g_ab_a_ms = audio_player_get_position_ms();
    g_ab_b_ms = -1;   // 重新标记 B
    ESP_LOGI(TAG, "AB mark A = %d ms", g_ab_a_ms);
}

void audio_player_mark_b(void)
{
    if (g_ab_a_ms < 0) {
        ESP_LOGW(TAG, "AB mark B: A not set");
        return;
    }
    g_ab_b_ms = audio_player_get_position_ms();
    if (g_ab_b_ms <= g_ab_a_ms) g_ab_b_ms = g_ab_a_ms + 1000; // 保证 B>A
    /* R111: 标记 B 后自动开启循环（间隔>=1s） */
    if (g_ab_b_ms - g_ab_a_ms >= 1000) {
        g_ab_enabled = true;
        g_ab_loop_count = 0;
    }
    ESP_LOGI(TAG, "AB mark B = %d ms (span %d ms), auto-enable=%d",
             g_ab_b_ms, g_ab_b_ms - g_ab_a_ms, g_ab_enabled);
}

void audio_player_clear_ab(void)
{
    g_ab_a_ms = -1;
    g_ab_b_ms = -1;
    g_ab_enabled = false;
    g_ab_loop_count = 0;   /* R111: 重置遍数 */
    ESP_LOGI(TAG, "AB cleared");
}

void audio_player_set_ab_enabled(bool en)
{
    // 未标记 A/B 时强制关闭，避免无效循环
    if (en && (g_ab_a_ms < 0 || g_ab_b_ms <= g_ab_a_ms)) {
        ESP_LOGW(TAG, "AB enable ignored: A/B not set");
        return;
    }
    g_ab_enabled = en;
    if (!en) g_ab_loop_count = 0;   /* R111: 关闭时重置遍数 */
    ESP_LOGI(TAG, "AB enabled = %d", g_ab_enabled);
}

bool audio_player_is_ab_enabled(void) { return g_ab_enabled; }
int  audio_player_ab_a_ms(void) { return g_ab_a_ms; }
int  audio_player_ab_b_ms(void) { return g_ab_b_ms; }
int  audio_player_ab_loop_count(void) { return g_ab_loop_count; }   /* R111 */

/* R051：菜单内 A-B 微调 —— 直接设置 A/B 点到任意毫秒位置 */
void audio_player_set_ab_a_ms(int ms)
{
    if (ms < 0) return;
    if (g_ab_b_ms >= 0 && ms >= g_ab_b_ms) {
        g_ab_b_ms = ms + 1000;   // A 越过 B，则 B 顺延 1s
    }
    g_ab_a_ms = ms;
    ESP_LOGI(TAG, "AB set A = %d ms", g_ab_a_ms);
}

void audio_player_set_ab_b_ms(int ms)
{
    if (ms <= 0) return;
    if (g_ab_a_ms >= 0 && ms <= g_ab_a_ms) ms = g_ab_a_ms + 1000;  // 保证 B>A
    g_ab_b_ms = ms;
    ESP_LOGI(TAG, "AB set B = %d ms", g_ab_b_ms);
}

/* ============================================================
 * R049c：按键提示音
 * 复用空闲的 g_i2s_writer 播放一段内存 PCM（raw_stream → i2s），
 * 与音乐互斥（仅非播放态调用），避免 I2S 冲突。
 * ============================================================ */
static bool g_beep_busy = false;

void audio_player_play_beep(void)
{
    if (g_beep_busy)   return;
    if (g_is_playing)  return;     // 播放音乐时不提示（避免打断音乐）
    if (!g_i2s_writer) return;
    g_beep_busy = true;

    const int rate = 44100, ch = 2, bits = 16, ms = 60;
    const int n = rate * ch * (bits / 8) * ms / 1000;
    static uint8_t *buf = NULL;
    static int      buf_cap = 0;
    if (!buf || buf_cap < n) {
        if (buf) free(buf);
        buf = (uint8_t *)malloc(n);
        buf_cap = n;
    }
    if (!buf) { g_beep_busy = false; return; }

    int16_t *pcm = (int16_t *)buf;
    int nsamp = n / 2;
    float PI = 3.14159265f;
    int fade = (int)(0.002f * rate);   // 2ms 淡入淡出防爆音
    if (fade < 1) fade = 1;
    for (int i = 0; i < nsamp; i++) {
        float t = (float)i / (float)rate;
        float env = 1.0f;
        if (i < fade)            env = (float)i / fade;
        else if (i > nsamp - fade) env = (float)(nsamp - i) / fade;
        pcm[i] = (int16_t)(sinf(2.0f * PI * 880.0f * t) * 6000.0f * env);
    }

    audio_pipeline_cfg_t pcfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t p = audio_pipeline_init(&pcfg);
    if (!p) { g_beep_busy = false; return; }

    raw_stream_cfg_t rcfg = RAW_STREAM_CFG_DEFAULT();
    rcfg.type = AUDIO_STREAM_WRITER;
    audio_element_handle_t raw = raw_stream_init(&rcfg);
    if (!raw) { audio_pipeline_deinit(p); g_beep_busy = false; return; }

    audio_pipeline_register(p, raw, "raw");
    audio_pipeline_register(p, g_i2s_writer, "i2s");
    const char *tags[2] = {"raw", "i2s"};
    audio_pipeline_link(p, tags, 2);
    i2s_stream_set_clk(g_i2s_writer, rate, bits, ch);
    audio_pipeline_run(p);

    int off = 0, left = n;
    while (left > 0) {
        int w = raw_stream_write(raw, (char *)buf + off, left);
        if (w <= 0) break;
        off += w; left -= w;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    audio_element_set_ringbuf_done(raw);

    vTaskDelay(pdMS_TO_TICKS(ms + 60));   // 等播放完

    audio_pipeline_stop(p);
    audio_pipeline_unregister(p, raw);
    audio_pipeline_unregister(p, g_i2s_writer);  // 保留 g_i2s_writer 不销毁
    audio_pipeline_deinit(p);
    audio_element_deinit(raw);

    g_beep_busy = false;
}

#else // 不使用 ESP-ADF 的简易占位实现

#include "esp_log.h"
static const char *TAG = "audio_player";

void audio_player_init(void) {
    ESP_LOGI(TAG, "Audio player init (stub)");
}

bool audio_player_play(const char *filepath) {
    ESP_LOGI(TAG, "Play (stub): %s", filepath);
    return true;
}

void audio_player_pause(void) {}
void audio_player_resume(void) {}
void audio_player_stop(void) {}
void audio_player_seek(int seconds) {}
void audio_player_seek_ms(int ms) {}
int  audio_player_get_position_ms(void) { return 0; }
int  audio_player_get_position(void) { return 0; }
int  audio_player_get_duration(void) { return 0; }
bool audio_player_is_playing(void)  { return false; }
bool audio_player_is_paused(void)   { return false; }
void audio_player_set_speed(float speed) {}
void audio_player_set_volume(int volume) {}
int  audio_player_get_volume(void) { return AUDIO_OUTPUT_VOL; }  // R032-303：使用默认音量常量，消除硬编码耦合
void audio_player_tick(void) {}
void audio_player_set_callback(audio_status_cb_t cb, void *user_data) {}

/* R049b / R049c stub */
void audio_player_mark_a(void) {}
void audio_player_mark_b(void) {}
void audio_player_clear_ab(void) {}
void audio_player_set_ab_enabled(bool en) { (void)en; }
bool audio_player_is_ab_enabled(void) { return false; }
int  audio_player_ab_a_ms(void) { return -1; }
int  audio_player_ab_b_ms(void) { return -1; }
void audio_player_set_ab_a_ms(int ms) { (void)ms; }
void audio_player_set_ab_b_ms(int ms) { (void)ms; }
void audio_player_play_beep(void) {}

#endif // CONFIG_USE_ESP_ADF

/* ============================================================
 * 蓝牙音箱 (A2DP Sink) — 仅 ADF + USE_BT_SPEAKER 编译真实实现
 * 复用 audio_player 的 g_i2s_writer（I2S/MAX98357 输出），链路：
 *   A2DP Sink 解码 PCM (bluetooth_service 元素) → i2s_stream_writer
 * ============================================================ */
#if defined(CONFIG_USE_ESP_ADF) && defined(CONFIG_USE_BT_SPEAKER)

/* 手机端音量回调：映射到 level 并应用本地 ALC（不回传手机，避免循环） */
static void bt_phone_vol_cb(uint8_t vol_0_127)
{
    int level = (int)((uint32_t)vol_0_127 * VOLUME_LEVEL_MAX / 127);
    if (level > VOLUME_LEVEL_MAX) level = VOLUME_LEVEL_MAX;
    apply_volume_alc(level);
}

bool audio_player_start_bt(void)
{
    if (!g_i2s_writer) return false;
    audio_player_stop();   // 释放 SD 管道（保留 g_i2s_writer 跨模式复用）

    if (bt_speaker_start(g_i2s_writer) != ESP_OK) return false;

    bt_speaker_set_volume_cb(bt_phone_vol_cb);
    g_bt_active = true;
    // 同步当前音量到手机 (AVRCP 绝对音量 0..127)
    bt_speaker_report_volume((uint8_t)((uint32_t)g_volume * 127 / VOLUME_LEVEL_MAX));
    return true;
}

void audio_player_stop_bt(void)
{
    if (!g_bt_active) return;
    bt_speaker_stop();
    g_bt_active = false;
}

bool audio_player_is_bt_active(void) { return g_bt_active; }

#else  /* 非 ADF 或 未开 USE_BT_SPEAKER：桩实现，保证链接 */

bool audio_player_start_bt(void) { return false; }
void audio_player_stop_bt(void) {}
bool audio_player_is_bt_active(void) { return false; }

#endif
