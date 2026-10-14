/*
 * flac_decoder_drflac.c
 *
 * C 方案 FLAC 解码器: 基于 dr_flac (单头文件, public domain)。
 *
 * 历史与定论:
 * - R138/R139: 播放 FLAC 崩 LVGL 对象树(EXCVADDR=0x14 / 0x4038036e), MP3 不崩。一度误判为
 *   "dr_flac/解码任务栈溢出踩 LVGL"(R138~R140 都在查解码栈, 均错)。
 * - R141 定论: 真凶是 audio_player.cpp 的 flac_parse_duration_ms(64KB)/flac_sniff_*(4KB) 在
 *   8KB main 任务栈上开大数组, main 栈溢出踩坏 DRAM 堆 + PSRAM LVGL; dr_flac 解码根本没跑就崩。
 * - R144: ① dr_flac 内部分配改用 MALLOC_CAP_DEFAULT; ② 按真实声道数解码并统一输出立体声(单声道
 *   上混), 修复单声道 FLAC 语速翻倍 + 哒哒噪音。
 * - R149: 分配器改"DRAM 优先 + PSRAM 兜底"(见 drflac_malloc_cb)。R144 大缓冲落 PSRAM 虽放得下但
 *   解码慢, 96kHz 24bit 解不动(30s 曲子播成 47s, 实测); DRAM 访存快才实时, R145 后 DRAM 最大连续块
 *   ~70KB 放得下典型解码缓冲, 放不下再回落 PSRAM。
 *
 * 流式解码: drflac_open_relaxed 的 read 回调从 audio_element_input (上游 in_rb) 拉数据,
 * seek/tell 回调返回 DRFLAC_FALSE 表示不可寻址, dr_flac 以顺序流式模式解码 FLAC 帧。
 */

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "audio_element.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "flac_decoder_drflac.h"

static const char *TAG = "FLAC_DRFLAC";

/* R140-diag: 记录解码任务配置栈大小, 供 _drflac_process 打印高水位时计算已用量。 */
static uint32_t g_cfg_stack = 0;

/* 单次解码输出的 PCM 帧数 (16bit stereo -> 4096*2*2 = 16KB) */
#define DRFLAC_PCM_FRAMES  4096
#define DRFLAC_PCM_BYTES   (DRFLAC_PCM_FRAMES * 2 * 2)

typedef struct {
    drflac        *flac;
    int16_t       *pcm;
    bool           info_reported;
} drflac_dec_t;

/* dr_flac 读回调: 从上游 in_rb 拉 FLAC 字节。返回实际读到的字节, 0 表示 EOF。 */
static size_t drflac_read_cb(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    audio_element_handle_t self = (audio_element_handle_t)pUserData;
    int r = audio_element_input(self, (char *)pBufferOut, (int)bytesToRead);
    if (r > 0) return (size_t)r;
    return 0;   /* AEL_IO_DONE(<0) 或错误 => dr_flac 视作 EOF */
}

/* dr_flac seek 回调: 流式不可寻址。 */
static drflac_bool32 drflac_seek_cb(void *pUserData, int offset, drflac_seek_origin origin)
{
    (void)pUserData; (void)offset; (void)origin;
    return DRFLAC_FALSE;
}

/* dr_flac tell 回调: 流式不支持, 返回失败即可 (仅原生封装且不可寻址, dr_flac 不会依赖它)。 */
static drflac_bool32 drflac_tell_cb(void *pUserData, drflac_int64 *pCursor)
{
    (void)pUserData; (void)pCursor;
    return DRFLAC_FALSE;
}

/* R144: dr_flac 分配器改用 MALLOC_CAP_DEFAULT(优先 DRAM, 大缓冲自动回落 PSRAM)。
 * 背景: R138 时代误判"dr_flac PSRAM 缓冲踩 LVGL"曾强制 DRAM; 真凶其实是 main 栈嗅探缓冲溢出(R141)。
 * 强制 DRAM 导致立体声 FLAC 解码缓冲(单声道 2 倍)在碎片化 DRAM(最大连续块 ~31KB)分不到 ->
 * drflac_open 失败 -> 立体声 FLAC 无法播放(单声道缓冲小一半反而能开)。PSRAM 有 8MB(最大连续块
 * ~7.6MB), 软件解码无 DMA, 放 PSRAM 安全且充裕。 */
static void *drflac_malloc_cb(size_t sz, void *pUserData)
{
    (void)pUserData;
    /* R149: 优先 DRAM(解码访存快, 96kHz 24bit 才实时); DRAM 不够(碎片化/过大)回落 PSRAM 保证能开。
       背景: R144 全落 PSRAM 虽能放, 但 PSRAM 慢, 96kHz 输出速率翻倍解不动(30s 曲子播成 47s, 实测);
       R145 把 AAC/MP3 栈挪去 PSRAM 后 DRAM 最大连续块回到 ~70KB, 放得下典型解码缓冲。
       R151 诊断(已定论): 44.1/48k 解码缓冲 41264B→DRAM(快); 96k 24bit 缓冲 69936B 超 DRAM 最大
       连续块→回落 PSRAM→解码慢(30s 播 47s)。定论=96kHz 24bit 超本硬件实时能力, 建议转码 48k。
       R152 收尾: 该分配位置日志降为 LOGD(默认不显示), 保留备查。 */
    void *p = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char *loc = "DRAM";
    if (!p) { p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); loc = "PSRAM"; }
    if (sz >= 24576) ESP_LOGD(TAG, "R151-diag: drflac alloc %uB -> %s", (unsigned)sz, loc);
    return p;
}
static void *drflac_realloc_cb(void *p, size_t sz, void *pUserData)
{
    (void)pUserData;
    if (!p) return drflac_malloc_cb(sz, pUserData);
    void *q = heap_caps_realloc(p, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!q) q = heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q) {   /* 跨区搬移: 手动 alloc(DRAM/PSRAM 回落) + copy + free */
        size_t old = heap_caps_get_allocated_size(p);
        void *n = drflac_malloc_cb(sz, pUserData);
        if (!n) return NULL;
        if (old) memcpy(n, p, old < sz ? old : sz);
        heap_caps_free(p);
        q = n;
    }
    return q;
}
static void drflac_free_cb(void *p, void *pUserData)
{
    (void)pUserData;
    if (p) heap_caps_free(p);
}
static const drflac_allocation_callbacks g_drflac_alloc = {
    NULL, drflac_malloc_cb, drflac_realloc_cb, drflac_free_cb
};

static esp_err_t _drflac_open(audio_element_handle_t self)
{
    drflac_dec_t *d = (drflac_dec_t *)audio_element_getdata(self);
    if (!d) return ESP_FAIL;

    /* 打开时 dr_flac 会通过 read 回调顺序读 STREAMINFO/frames; 上游 file element 已启动并写 in_rb, 此处阻塞等数据。
     * 用 open_relaxed + drflac_container_native: 部分读不会立刻被判为 EOF, 直到 onRead 返回 0 才认为流结束, 更适合流式。
     * 关键: 第 6 参传 g_drflac_alloc; R144 起大缓冲(如 pDecodeBuffer)可回落 PSRAM, 避免 DRAM 碎片化致立体声 FLAC 分配失败。 */
    d->flac = drflac_open_relaxed(drflac_read_cb, drflac_seek_cb, drflac_tell_cb,
                                  drflac_container_native, (void *)self, &g_drflac_alloc);
    if (!d->flac) {
        ESP_LOGE(TAG, "drflac_open_relaxed failed (DRAM 不足或 STREAMINFO 读取失败)");
        return ESP_FAIL;
    }

    /* R144: PCM 输出缓冲改用 MALLOC_CAP_DEFAULT(16KB 通常仍落 DRAM; DRAM 紧时回落 PSRAM)。 */
    d->pcm = (int16_t *)heap_caps_malloc(DRFLAC_PCM_BYTES, MALLOC_CAP_DEFAULT);
    if (!d->pcm) {
        drflac_close(d->flac);
        d->flac = NULL;
        ESP_LOGE(TAG, "out of memory for PCM buffer");
        return ESP_FAIL;
    }
    d->info_reported = false;
    ESP_LOGI(TAG, "dr_flac opened (%u Hz, %u ch, %u bit)",
             (unsigned)d->flac->sampleRate, (unsigned)d->flac->channels,
             (unsigned)d->flac->bitsPerSample);
    return ESP_OK;
}

static audio_element_err_t _drflac_process(audio_element_handle_t self, char *el_buffer, int el_buf_len)
{
    (void)el_buffer; (void)el_buf_len;
    drflac_dec_t *d = (drflac_dec_t *)audio_element_getdata(self);
    if (!d || !d->flac) return AEL_PROCESS_FAIL;

    /* R141: 日常监控解码任务栈高水位(运行期最小值)。注意: FLAC 崩溃真凶是 main 栈嗅探缓冲
     * 溢出(见 audio_player.cpp R141), 与此解码栈无关; 此日志仅用于观察 dr_flac 真实栈用量。 */
    {
        static uint32_t s_min_wm = UINT32_MAX;
        uint32_t wm = uxTaskGetStackHighWaterMark(NULL);
        if (wm < s_min_wm) {
            s_min_wm = wm;
            ESP_LOGI(TAG, "DRFLAC stack high-water min=%u bytes free (used~=%u of cfg)",
                     (unsigned)wm, (unsigned)(g_cfg_stack - wm));
        }
    }

    /* R144: 按文件真实声道数读取, 并按 d->pcm 容量限制帧数(防 >2ch 写爆 16KB 缓冲);
     * 统一输出立体声(单声道上混 / >2ch 取前两声轨), 供 2ch i2s 消费。
     * 旧代码写死 bytes=frames*2*2(按立体声), 单声道文件被当立体声输出 -> 语速翻倍+哒哒噪音(R144 实测)。 */
    uint32_t ch = d->flac->channels ? d->flac->channels : 1;
    uint32_t max_frames = DRFLAC_PCM_BYTES / (ch * 2);   /* 读入字节不超 d->pcm(16KB) */
    if (max_frames > DRFLAC_PCM_FRAMES) max_frames = DRFLAC_PCM_FRAMES;
    if (max_frames == 0) max_frames = 1;
    uint64_t frames = drflac_read_pcm_frames_s16(d->flac, max_frames, d->pcm);
    if (frames == 0) {
        return AEL_IO_DONE;   /* 流结束 */
    }

    uint32_t bytes;
    if (ch == 1) {
        /* 单声道 -> 立体声(复制到 L/R); 原地从后往前扩, 避免覆盖未读的单声道样本 */
        for (int32_t i = (int32_t)frames - 1; i >= 0; i--) {
            d->pcm[2*i]     = d->pcm[i];
            d->pcm[2*i + 1] = d->pcm[i];
        }
        bytes = (uint32_t)frames * 4;
    } else if (ch == 2) {
        bytes = (uint32_t)frames * 4;
    } else {
        /* >2ch -> 取每帧前两声轨成立体声(原地前向, 读 index i*ch > 写 index 2i, 安全) */
        for (uint32_t i = 0; i < (uint32_t)frames; i++) {
            d->pcm[2*i]     = d->pcm[i * ch];
            d->pcm[2*i + 1] = d->pcm[i * ch + 1];
        }
        bytes = (uint32_t)frames * 4;
    }
    int wlen = audio_element_output(self, (char *)d->pcm, (int)bytes);
    if (wlen <= 0) {
        return (audio_element_err_t)wlen;
    }

    if (!d->info_reported) {
        audio_element_info_t info = {0};
        info.sample_rates = (int)d->flac->sampleRate;
        info.channels     = 2;   /* R144: 输出恒为立体声(单声道已上混) */
        info.bits         = 16;  /* dr_flac 已转出 16bit PCM */
        info.codec_fmt    = ESP_CODEC_TYPE_FLAC;
        audio_element_setinfo(self, &info);
        audio_element_report_info(self);
        d->info_reported = true;
        ESP_LOGI(TAG, "FLAC info: %u Hz, %u ch(src) -> stereo, 16 bit",
                 (unsigned)d->flac->sampleRate, (unsigned)ch);
    }
    return AEL_IO_TIMEOUT;   /* 继续驱动解码, 不可返回 AEL_IO_OK(0) 否则 ADF 视为完成 */
}

static esp_err_t _drflac_close(audio_element_handle_t self)
{
    drflac_dec_t *d = (drflac_dec_t *)audio_element_getdata(self);
    if (d) {
        if (d->flac) { drflac_close(d->flac); d->flac = NULL; }
        if (d->pcm)  { free(d->pcm);          d->pcm  = NULL; }
    }
    ESP_LOGI(TAG, "dr_flac closed");
    return ESP_OK;
}

static esp_err_t _drflac_destroy(audio_element_handle_t self)
{
    drflac_dec_t *d = (drflac_dec_t *)audio_element_getdata(self);
    if (d) free(d);
    return ESP_OK;
}

audio_element_handle_t flac_decoder_drflac_init(flac_decoder_drflac_cfg_t *config)
{
    flac_decoder_drflac_cfg_t cfg_buf;
    if (config == NULL) {
        cfg_buf = (flac_decoder_drflac_cfg_t)DEFAULT_FLAC_DECODER_DRFLAC_CONFIG();
        config = &cfg_buf;
    }
    drflac_dec_t *d = (drflac_dec_t *)calloc(1, sizeof(drflac_dec_t));

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open    = _drflac_open;
    cfg.process = _drflac_process;
    cfg.close   = _drflac_close;
    cfg.destroy = _drflac_destroy;
    cfg.task_stack   = config->task_stack > 0 ? config->task_stack : DEFAULT_ELEMENT_STACK_SIZE;
    cfg.out_rb_size  = config->out_rb_size > 0 ? config->out_rb_size : DEFAULT_ELEMENT_RINGBUF_SIZE;
    cfg.stack_in_ext = config->stack_in_ext;
    cfg.tag = "drflac";
    g_cfg_stack = cfg.task_stack;

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        free(d);
        return NULL;
    }
    audio_element_setdata(el, d);
    ESP_LOGI(TAG, "drflac element created (stack=%u, out_rb=%u, ext=%d)",
             (unsigned)cfg.task_stack, (unsigned)cfg.out_rb_size, cfg.stack_in_ext);
    return el;
}
