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

#ifdef CONFIG_USE_ESP_ADF

#include "audio_pipeline.h"
#include "audio_element.h"
// R035-014：审计确认 audio_common.h 不是间接依赖（注释后构建通过 exit 0），正式删除
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "flac_decoder.h"
#include "ogg_decoder.h"
#include "wav_decoder.h"
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
static audio_element_handle_t   g_i2s_writer = NULL;   // 跨曲目复用

static bool         g_is_playing = false;
static bool         g_is_paused = false;
static int          g_volume = AUDIO_OUTPUT_VOL;

/* V1.2 音量 dB 线性映射边界 (MAX98357A ALC 范围) */
#define VOL_DB_MIN  (-96)   // 静音
#define VOL_DB_MAX  (12)    // 最大增益
static int          g_total_duration_ms = 0;
static uint32_t     g_total_file_bytes = 0;
static int          g_current_sample_rate = AUDIO_SAMPLE_RATE;  // I2S 当前采样率缓存
static uint64_t     g_play_start_us = 0;               // 本次播放起始（pause/resume 时重置）
static int64_t      g_play_offset_us = 0;              // pause 时锁存的已播放时长，resume 时叠加
static uint64_t     g_last_scrub_us = 0;               // M1: 上次跳帧时间戳（模块级全局）

/* R049b：A-B 区间复读状态（ms，-1=未标记） */
static int  g_ab_a_ms = -1;
static int  g_ab_b_ms = -1;
static bool g_ab_enabled = false;

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

static audio_element_handle_t create_decoder(const char *path)
{
    const char *ext = get_file_ext(path);

    mp3_decoder_cfg_t  mp3_cfg  = DEFAULT_MP3_DECODER_CONFIG();
    aac_decoder_cfg_t  aac_cfg  = DEFAULT_AAC_DECODER_CONFIG();
    flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
    ogg_decoder_cfg_t  ogg_cfg  = DEFAULT_OGG_DECODER_CONFIG();
    wav_decoder_cfg_t  wav_cfg  = DEFAULT_WAV_DECODER_CONFIG();

    if (strcasecmp(ext, ".mp3") == 0) {
        ESP_LOGI(TAG, "Using MP3 decoder");
        return mp3_decoder_init(&mp3_cfg);
    } else if (strcasecmp(ext, ".aac") == 0 || strcasecmp(ext, ".m4a") == 0) {
        ESP_LOGI(TAG, "Using AAC decoder");
        return aac_decoder_init(&aac_cfg);
    } else if (strcasecmp(ext, ".flac") == 0) {
        ESP_LOGI(TAG, "Using FLAC decoder");
        return flac_decoder_init(&flac_cfg);
    } else if (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".opus") == 0) {
        ESP_LOGI(TAG, "Using OGG/OPUS decoder");
        return ogg_decoder_init(&ogg_cfg);
    } else if (strcasecmp(ext, ".wav") == 0) {
        ESP_LOGI(TAG, "Using WAV decoder");
        return wav_decoder_init(&wav_cfg);
    }

    ESP_LOGW(TAG, "Unknown format %s, trying MP3 decoder", ext);
    return mp3_decoder_init(&mp3_cfg);
}

/* ============================================================
 * 初始化（仅创建 I2S 输出流，pipeline 在 play() 中重建）
 * ============================================================ */
void audio_player_init(void)
{
    ESP_LOGI(TAG, "Initializing audio subsystem...");
    vTaskDelay(pdMS_TO_TICKS(5));  /* 强制 flush 串口，避免阻塞前日志丢失 */

    // 创建 I2S 输出流（跨曲目复用，无需每次重建）
    // I2S 引脚直接绑定到原理图定义 (BCLK=IO6, WS=IO7, DIN=IO5, MAX98357 U8)
    // IDF v5 的 i2s_stream 通过 std_cfg.gpio_cfg 配置引脚 (i2s_set_pin 已移除)
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.std_cfg.gpio_cfg.bclk = I2S_BCK_IO;
    i2s_cfg.std_cfg.gpio_cfg.ws   = I2S_WS_IO;
    i2s_cfg.std_cfg.gpio_cfg.dout = I2S_DOUT_IO;
    i2s_cfg.std_cfg.gpio_cfg.din  = GPIO_NUM_NC;
    ESP_LOGI(TAG, "i2s_stream_init enter (BCLK=IO%d WS=IO%d DIN=IO%d)",
             I2S_BCK_IO, I2S_WS_IO, I2S_DOUT_IO);
    vTaskDelay(pdMS_TO_TICKS(5));
    g_i2s_writer = i2s_stream_init(&i2s_cfg);
    ESP_LOGI(TAG, "i2s_stream_init returned %p", (void *)g_i2s_writer);
    if (!g_i2s_writer) {
        ESP_LOGE(TAG, "Failed to create I2S writer stream (audio disabled, continue)");
        /* 失败保护：不阻塞、不返回，允许 UI 继续运行 */
    } else {
        ESP_LOGI(TAG, "I2S pins bound: BCLK=IO%d WS=IO%d DIN=IO%d",
                 I2S_BCK_IO, I2S_WS_IO, I2S_DOUT_IO);
    }

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

    // R049b：新曲目清空 A-B 标记（避免跨文件失效）
    g_ab_a_ms = -1;
    g_ab_b_ms = -1;
    g_ab_enabled = false;

    // 1. 创建 pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
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

    // 3. 创建解码器
    g_decoder = create_decoder(filepath);
    if (!g_decoder) {
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
    if (audio_pipeline_register(g_pipeline, g_decoder, "decoder") != ESP_OK) {
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
    // M5：注册 i2s 前 NULL 守卫，避免在 init 失败/未初始化的极端情况崩溃
    if (!g_i2s_writer) {
        ESP_LOGE(TAG, "g_i2s_writer is NULL, refusing to register pipeline");
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_element_deinit(g_fatfs_reader);
        audio_element_deinit(g_decoder);
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }
    // R035-015：第三次 audio_pipeline_register 添加返回值检查 + 失败清理
    // R036-001：i2s_writer 是跨曲目复用资源（见 L50/L112 注释），失败清理仅 unregister，
    // 不 deinit、不置 NULL，与 run/link 失败路径一致。
    if (audio_pipeline_register(g_pipeline, g_i2s_writer, "i2s") != ESP_OK) {
        ESP_LOGE(TAG, "register i2s_writer failed");
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_element_deinit(g_fatfs_reader);
        audio_element_deinit(g_decoder);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);  // 仅 unregister，保留跨曲目复用
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 5. 链接管道: file → decoder → i2s
    const char *link_tags[3] = {"file", "decoder", "i2s"};
    esp_err_t link_err = audio_pipeline_link(g_pipeline, link_tags, 3);
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link pipeline (0x%x)", link_err);
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);
        audio_element_deinit(g_fatfs_reader);
        audio_element_deinit(g_decoder);
        g_fatfs_reader = NULL;
        g_decoder = NULL;
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
        return false;
    }

    // 6. 设置文件 URI
    audio_element_set_uri(g_fatfs_reader, filepath);

    // 7. 设置 I2S 时钟（MAX98357A 无编解码器，仅需 BCLK/LRCLK 匹配）
    g_current_sample_rate = AUDIO_SAMPLE_RATE;
    i2s_stream_set_clk(g_i2s_writer, AUDIO_SAMPLE_RATE, 16, 2);

    // 8. 启动管道（R032-209: 检查返回值，失败即终止，避免进入播放态却无声）
    if (audio_pipeline_run(g_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed");
        // R034-004：复用 link 失败时的清理路径，避免 pipeline + 元素句柄泄漏
        audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
        audio_pipeline_unregister(g_pipeline, g_decoder);
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);
        audio_element_deinit(g_fatfs_reader);
        audio_element_deinit(g_decoder);
        g_fatfs_reader = NULL;
        g_decoder = NULL;
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
    struct stat st;
    if (stat(filepath, &st) == 0) {
        g_total_file_bytes = (uint32_t)st.st_size;
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
        const char *ext = get_file_ext(filepath);
        int bytes_per_ms;
        if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".aac") == 0 ||
            strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".ogg") == 0) {
            bytes_per_ms = 16;
        } else if (strcasecmp(ext, ".opus") == 0) {
            bytes_per_ms = 12;
        } else if (strcasecmp(ext, ".flac") == 0) {
            bytes_per_ms = 64;
        } else if (strcasecmp(ext, ".wav") == 0) {
            bytes_per_ms = 176;
        } else {
            bytes_per_ms = 16;  // fallback 同 MP3
        }
        g_total_duration_ms = g_total_file_bytes / bytes_per_ms;
        ESP_LOGD(TAG, "Duration estimated from file size: %d ms (bytes/ms=%d, ext=%s)",
                 g_total_duration_ms, bytes_per_ms, ext);
        ESP_LOGW(TAG, "Estimated duration is approximate; progress bar/seek may be imprecise");
    }

    // 11. 应用当前音量
    audio_player_set_volume(g_volume);

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
        g_last_scrub_us = 0;
        return;
    }

    if (g_pipeline) {
        audio_pipeline_stop(g_pipeline);
        {
            int retries = AUDIO_STOP_TIMEOUT_MS / AUDIO_STOP_POLL_MS;
            while (retries > 0) {
                audio_element_state_t st = audio_element_get_state(g_i2s_writer);
                if (st == AEL_STATE_FINISHED || st == AEL_STATE_STOPPED || st == AEL_STATE_INIT) break;
                vTaskDelay(pdMS_TO_TICKS(AUDIO_STOP_POLL_MS));
                retries--;
            }
            if (retries == 0) {
                ESP_LOGW(TAG, "Pipeline stop timeout, forcing terminate");
                audio_pipeline_terminate(g_pipeline);
            }
        }

        // 销毁 per-track 元素（fatfs_reader + decoder）
        if (g_fatfs_reader) {
            audio_pipeline_unregister(g_pipeline, g_fatfs_reader);
            audio_element_deinit(g_fatfs_reader);
            g_fatfs_reader = NULL;
        }
        if (g_decoder) {
            audio_pipeline_unregister(g_pipeline, g_decoder);
            audio_element_deinit(g_decoder);
            g_decoder = NULL;
        }
        // i2s_writer 从管道注销但不销毁（跨曲目复用）
        audio_pipeline_unregister(g_pipeline, g_i2s_writer);

        // 销毁管道本身
        audio_pipeline_deinit(g_pipeline);
        g_pipeline = NULL;
    }

    g_is_playing = false;
    g_is_paused = false;

    g_total_duration_ms = 0;
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

static void audio_player_seek_ms_internal(int ms)
{
    if (!g_pipeline || !g_is_playing || !g_decoder || !g_fatfs_reader) return;

    if (g_total_duration_ms > 0 && g_total_file_bytes > 0) {
        // R028/L1: int64_t 中转避免 uint64 隐式截断
        int64_t byte_pos = (int64_t)ms * g_total_file_bytes / g_total_duration_ms;
        // R032-002 复审修订：ADF audio_element_set_byte_pos 入参为 int（32-bit），
        // 必须钳位到 INT32_MAX，避免 >2.1 GB 文件隐式窄化截断导致 seek 失准/跳轨。
        if (byte_pos > INT32_MAX) byte_pos = INT32_MAX;
        audio_element_set_byte_pos(g_fatfs_reader, (int)byte_pos);
    } else if (g_total_file_bytes > 0) {
        int64_t byte_pos = (int64_t)ms * g_total_file_bytes / 3600000;
        // R032-002 复审修订：ADF 入参为 int，必须钳位避免窄化截断。
        if (byte_pos > INT32_MAX) byte_pos = INT32_MAX;
        audio_element_set_byte_pos(g_fatfs_reader, (int)byte_pos);
    }

    // C1: 重置 decoder byte_pos，使其从 reader 新位置重新开始解码
    audio_element_set_byte_pos(g_decoder, 0);

    g_play_start_us = esp_timer_get_time();
    g_play_offset_us = (int64_t)ms * 1000;
}

void audio_player_seek_ms(int ms)
{
    if (!g_pipeline || !g_is_playing) return;
    // S5：保留原暂停态——暂停时 seek 不再静默 resume
    bool was_paused = g_is_paused;
    if (!was_paused) {
        audio_pipeline_pause(g_pipeline);
    }
    audio_player_seek_ms_internal(ms);
    if (!was_paused) {
        audio_pipeline_resume(g_pipeline);
    } else {
        // R035-020：保持 paused：清掉内部函数的 start_us 赋值，避免 get_position_ms 在暂停态累积。
        // 注意此处依赖 audio_player_seek_ms_internal 已写入正确的 g_play_offset_us，
        // 否则 seek 后的位置计算会偏移。如有疑问，请同时审计 seek_ms_internal。
        g_play_start_us = 0;
    }
}

int audio_player_get_position_ms(void)
{
    if (!g_pipeline) return 0;

    // 累计播放时间 = 暂停前已累计 + 当前段播放时间（暂停期间不增加）
    int64_t total = g_play_offset_us;
    if (g_is_playing && !g_is_paused && g_play_start_us > 0) {
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

    int sample_rate;
    if (speed > 0) {
        // C3: 跳帧模式 — I2S 正常速率，seek 跳帧提供"快进"感（R034-011）
        if (tape_control_is_scrub_mode()) {
            sample_rate = AUDIO_SAMPLE_RATE;
        } else {
            sample_rate = (int)(AUDIO_SAMPLE_RATE * speed);
            // R035-009: sample_rate limits derived from AUDIO_SAMPLE_RATE * {0.5, 4.0}
            if (sample_rate < (AUDIO_SAMPLE_RATE / 2)) sample_rate = AUDIO_SAMPLE_RATE / 2;
            if (sample_rate > (AUDIO_SAMPLE_RATE * 4)) sample_rate = AUDIO_SAMPLE_RATE * 4;
        }
    } else {
        // R032-203：快退（speed<0）方向由 audio_player_tick 的跳帧向后 seek 实现，
        // 此处保持正常音高不变调（负采样率只会让音高失真，且跳帧已能模拟快退听感）。
        sample_rate = AUDIO_SAMPLE_RATE;
    }

    // 缓存命中则跳过冗余的 i2s_set_clk 调用
    if (sample_rate != g_current_sample_rate) {
        g_current_sample_rate = sample_rate;
        i2s_stream_set_clk(g_i2s_writer, sample_rate, 16, 2);
    }
}

/* 内部：仅设置 g_volume 并应用 I2S ALC（不触达 BT 回传，避免音量循环） */
static void apply_volume_alc(int volume)
{
    g_volume = volume;
    if (g_i2s_writer) {
        int alc_vol;
        if (volume <= 0) {
            alc_vol = VOL_DB_MIN;                          // -96 dB 静音
        } else {
            // 线性 dB：level 0..14 映射到 -96..+12，四舍五入
            alc_vol = VOL_DB_MIN + (volume * (VOL_DB_MAX - VOL_DB_MIN) + VOLUME_LEVEL_MAX / 2) / VOLUME_LEVEL_MAX;
        }
        // 安全钳位 (i2s_alc_volume_set 仅接受 -96..+12)
        if (alc_vol < VOL_DB_MIN) alc_vol = VOL_DB_MIN;
        if (alc_vol > VOL_DB_MAX) alc_vol = VOL_DB_MAX;
        i2s_alc_volume_set(g_i2s_writer, alc_vol);
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
    if (el_state == AEL_STATE_FINISHED || el_state == AEL_STATE_STOPPED) {
        ESP_LOGI(TAG, "Track finished");
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
            ESP_LOGD(TAG, "AB loop: seek back to A (%d ms)", g_ab_a_ms);
            audio_pipeline_pause(g_pipeline);
            audio_player_seek_ms_internal(g_ab_a_ms);
            audio_pipeline_resume(g_pipeline);
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
    bool need_seek = (abs_speed >= tape_control_get_max_gear_speed()) || (mode == TAPE_MODE_REWIND);

    if (!need_seek) return;

    uint64_t now = esp_timer_get_time();

    if ((now - g_last_scrub_us) < 50000) return; // 50ms 间隔
    g_last_scrub_us = now;

    // C3: 跳帧模式 — 跳 7/8（skip 350ms/50ms），其他档位 50ms × abs_speed（R034-011）
    int skip_ms;
    if (tape_control_is_scrub_mode() && mode == TAPE_MODE_FAST_FORWARD) {
        skip_ms = (int)(50.0f * (abs_speed - 1.0f));  // 跳帧模式：50 × (8-1) = 350ms
    } else {
        skip_ms = (int)(50.0f * abs_speed);           // 常规跳帧：50 × speed
    }

    int cur_ms = audio_player_get_position_ms();
    int target_ms;

    if (speed > 0) {
        target_ms = cur_ms + skip_ms;
        int duration_ms = g_total_duration_ms > 0 ? g_total_duration_ms : 3600000;
        if (target_ms > duration_ms) target_ms = duration_ms;
    } else {
        target_ms = cur_ms - skip_ms;
        if (target_ms < 0) target_ms = 0;
    }

    // M2 + C1: pause 确保 reader idle，seek 后 resume
    audio_pipeline_pause(g_pipeline);
    audio_player_seek_ms_internal(target_ms);
    audio_pipeline_resume(g_pipeline);
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
    ESP_LOGI(TAG, "AB mark B = %d ms (span %d ms)", g_ab_b_ms, g_ab_b_ms - g_ab_a_ms);
}

void audio_player_clear_ab(void)
{
    g_ab_a_ms = -1;
    g_ab_b_ms = -1;
    g_ab_enabled = false;
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
    ESP_LOGI(TAG, "AB enabled = %d", g_ab_enabled);
}

bool audio_player_is_ab_enabled(void) { return g_ab_enabled; }
int  audio_player_ab_a_ms(void) { return g_ab_a_ms; }
int  audio_player_ab_b_ms(void) { return g_ab_b_ms; }

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
